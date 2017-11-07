#include <zmq.hpp>
#include <string>
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <pthread.h>
#include <unistd.h>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <ctime>
#include "rc_kv_store.h"
#include "message.pb.h"
#include "socket_cache.h"
#include "zmq_util.h"
#include "consistent_hash_map.hpp"
#include "common.h"
#include "server_utility.h"

// TODO: Everything that's currently writing to cout and cerr should be replaced with a logfile.
using namespace std;

// If the total number of updates to the kvs before the last gossip reaches THRESHOLD, then the thread gossips to others.
#define THRESHOLD 1

// Define the gossip period (frequency)
#define PERIOD 5

// Define the number of memory threads
#define MEMORY_THREAD_NUM 1

// Define the number of ebs threads
#define EBS_THREAD_NUM 3

// Define local ebs replication factor
#define LOCAL_EBS_REPLICATION 2

// Define the locatioon of the conf file with the ebs root path
#define EBS_ROOT_FILE "conf/server/ebs_root.txt"

// TODO: reconsider type names here
typedef KV_Store<string, RC_KVS_PairLattice<string>> Database;

typedef consistent_hash_map<worker_node_t,ebs_hasher> ebs_hash_t;

struct pair_hash {
  template <class T1, class T2>
    std::size_t operator () (const std::pair<T1,T2> &p) const {
      auto h1 = std::hash<T1>{}(p.first);
      auto h2 = std::hash<T2>{}(p.second);

      return h1 ^ h2;
    }
};

// an unordered map to represent the gossip we are sending
typedef unordered_map<string, RC_KVS_PairLattice<string>> gossip_data;

// a pair to keep track of where each key in the changeset should be sent
typedef pair<size_t, unordered_set<string>> changeset_data;

// A wrapper around changeset data that indicates whether or not the data
// transfer is local or not
struct changeset_data_wrapper {
  changeset_data_wrapper() : local(false) {}

  changeset_data c_data;
  bool local;
};

// a map that represents which keys should be sent to which IP-port
// combinations
typedef unordered_map<string, unordered_set<string>> changeset_address;

// similar to the above but also tells each worker node whether or not it
// should delete the key
typedef unordered_map<string, unordered_set<pair<string, bool>, pair_hash>> redistribution_address;

// represents the replication state for each key
struct key_info {
  key_info(): tier_('E'), global_ebs_replication_(GLOBAL_EBS_REPLICATION), local_ebs_replication_(LOCAL_EBS_REPLICATION) {}
  key_info(char tier, int global_ebs_replication, int local_ebs_replication)
    : tier_(tier), global_ebs_replication_(global_ebs_replication), local_ebs_replication_(local_ebs_replication) {}

  char tier_;
  int global_ebs_replication_;
  int local_ebs_replication_;
};

// TODO:  this should be changed to something more globally robust
atomic<int> lww_timestamp(0);

bool enable_ebs(false);
string ebs_root("empty");

string get_ebs_path(string subpath) {
  if (ebs_root == "empty") {
    ifstream address;

    address.open(EBS_ROOT_FILE);
    std::getline(address, ebs_root);
    address.close();

    if (ebs_root.back() != '/') {
      ebs_root = ebs_root + "/";
    }
  }

  return ebs_root + subpath;
}

// TODO: more intelligent error mesages throughout
pair<RC_KVS_PairLattice<string>, bool> process_ebs_get(string key, int thread_id) {
  RC_KVS_PairLattice<string> res;
  bool succeed = true;

  communication::Payload pl;
  string fname = get_ebs_path("ebs_" + to_string(thread_id) + "/" + key);

  // open a new filestream for reading in a binary
  fstream input(fname, ios::in | ios::binary);

  if (!input) {
    succeed = false;
  } else if (!pl.ParseFromIstream(&input)) {
    cerr << "Failed to parse payload." << endl;
    succeed = false;
  } else {
    res = RC_KVS_PairLattice<string>(timestamp_value_pair<string>(pl.timestamp(), pl.value()));
  }

  // we return a lattice here because this method is invoked for gossip in
  // addition to user requests
  return pair<RC_KVS_PairLattice<string>, bool>(res, succeed);
}

bool process_ebs_put(string key, int timestamp, string value, int thread_id, unordered_set<string>& key_set) {
  bool succeed = true;
  timestamp_value_pair<string> p = timestamp_value_pair<string>(timestamp, value);

  communication::Payload pl_orig;
  communication::Payload pl;

  string fname = get_ebs_path("ebs_" + to_string(thread_id) + "/" + key);
  fstream input(fname, ios::in | ios::binary);

  if (!input) { // in this case, this key has never been seen before, so we attempt to create a new file for it
    pl.set_timestamp(timestamp);
    pl.set_value(value);

    // ios::trunc means that we overwrite the existing file
    fstream output(fname, ios::out | ios::trunc | ios::binary);
    if (!pl.SerializeToOstream(&output)) {
      cerr << "Failed to write payload." << endl;
      succeed = false;
    }
  } else if (!pl_orig.ParseFromIstream(&input)) { // if we have seen the key before, attempt to parse what was there before
    cerr << "Failed to parse payload." << endl;
    succeed = false;
  } else {
    // get the existing value that we have and merge
    RC_KVS_PairLattice<string> l = RC_KVS_PairLattice<string>(timestamp_value_pair<string>(pl_orig.timestamp(), pl_orig.value()));
    l.merge(p);

    // set the payload's data to the merged values of the value and timestamp
    pl.set_timestamp(l.reveal().timestamp);
    pl.set_value(l.reveal().value);

    // write out the new payload.
    fstream output(fname, ios::out | ios::trunc | ios::binary);
    if (!pl.SerializeToOstream(&output)) {
      cerr << "Failed to write payload\n";
      succeed = false;
    }
  }

  if (succeed) {
    key_set.insert(key);
  }

  return succeed;
}

string process_client_request(communication::Request& req, int thread_id, unordered_set<string>& local_changeset, unordered_set<string>& key_set) {
  communication::Response response;

  if (req.has_get()) {
    cout << "Received get on thread " << thread_id << "\n";
    auto res = process_ebs_get(req.get().key(), thread_id);

    response.set_succeed(res.second);
    response.set_value(res.first.reveal().value);
  } else if (req.has_put()) {
    cout << "Received put on thread " << thread_id << "\n";

    response.set_succeed(process_ebs_put(req.put().key(), lww_timestamp.load(), req.put().value(), thread_id, key_set));

    local_changeset.insert(req.put().key());
  } else {
    response.set_succeed(false);
  }

  string data;
  response.SerializeToString(&data);
  return data;
}

void process_distributed_gossip(communication::Gossip& gossip, int thread_id, unordered_set<string>& key_set) {
  for (int i = 0; i < gossip.tuple_size(); i++) {
    process_put(gossip.tuple(i).key(), gossip.tuple(i).timestamp(), gossip.tuple(i).value(), thread_id, key_set);
  }
}

// This is not serialized into protobuf and using the method above for serializiation overhead
void process_local_gossip(gossip_data* g_data, int thread_id, unordered_set<string>& key_set) {
  for (auto it = g_data->begin(); it != g_data->end(); it++) {
    process_put(it->first, it->second.reveal().timestamp, it->second.reveal().value, thread_id, key_set);
  }
  delete g_data;
}

void send_gossip(changeset_address* change_set_addr, SocketCache& cache, string ip, int thread_id) {
  unordered_map<string, gossip_data*> local_gossip_map;
  unordered_map<string, communication::Gossip> distributed_gossip_map;

  for (auto map_it = change_set_addr->begin(); map_it != change_set_addr->end(); map_it++) {
    vector<string> v;
    split(map_it->first, ':', v);
    worker_node_t wnode = worker_node_t(v[0], stoi(v[1]) - SERVER_PORT);

    if (v[0] == ip) { // add to local gossip map
      local_gossip_map[wnode.local_gossip_addr_] = new gossip_data;

      // iterate over all of the gossip going to this destination
      for (auto set_it = map_it->second.begin(); set_it != map_it->second.end(); set_it++) {
        cout << "Local gossip key " + *set_it + " sent on thread " + to_string(thread_id) + ".\n";

        auto res = process_get(*set_it, thread_id);
        local_gossip_map[wnode.local_gossip_addr_]->emplace(*set_it, res.first);
      }
    } else { // add to distributed gossip map
      string gossip_addr = wnode.distributed_gossip_connect_addr;
      for (auto set_it = map_it->second.begin(); set_it != map_it->second.end(); set_it++) {
        cout << "Distributed gossip key " + *set_it + " sent on thread " + to_string(thread_id) + ".\n";

        communication::Gossip_Tuple* tp = distributed_gossip_map[gossip_addr].add_tuple();
        tp->set_key(*set_it);

        auto res = process_get(*set_it, thread_id);
        tp->set_value(res.first.reveal().value);
        tp->set_timestamp(res.first.reveal().timestamp);
      }
    }
  }

  // send local gossip
  for (auto it = local_gossip_map.begin(); it != local_gossip_map.end(); it++) {
    zmq_util::send_msg((void*)it->second, &cache[it->first]);
  }

  // send distributed gossip
  for (auto it = distributed_gossip_map.begin(); it != distributed_gossip_map.end(); it++) {
    string data;
    it->second.SerializeToString(&data);
    zmq_util::send_string(data, &cache[it->first]);
  }
}

// we have this template because we use responsible for with different hash
// functions depending on whether we are checking local or remote
// responsibility
template<typename N, typename H>
bool responsible(string key, int rep, consistent_hash_map<N,H>& hash_ring, string ip, node_t& sender_node, bool& remove) {
  // the ip of the node for which we are checking responsibility
  string self_id = ip + ":" + to_string(port);
  bool resp = false;

  auto pos = hash_ring.find(key);
  // this is just a dummy value
  auto target_pos = hash_ring.begin();

  // iterate once for every value in the replication factor
  for (int i = 0; i < rep; i++) {
    // if one of the replicas is the node we care about, we set response to
    // true; we also store that node in target_pos
    if (pos->second.id_.compare(self_id) == 0) {
      resp = true;
      target_pos = pos;
    }

    if (++pos == hash_ring.end()) {
      pos = hash_ring.begin();
    }
  }


  // at the end of the previous loop, pos has the last node responsible for the
  // key; we only remove that key if we have more nodes than there should be replicas
  if (resp && (hash_ring.size() > rep)) {
    remove = true;
    sender_node = pos->second;
  } else if (resp && (hash_ring.size() <= rep)) {
    remove = false;
    if (++target_pos == hash_ring.end()) {
      target_pos = hash_ring.begin();
    }

    sender_node = target_pos->second;
  }

  return resp;
}

// ebs worker event loop
void ebs_worker_routine (zmq::context_t* context, string ip, int thread_id) {
  size_t port = SERVER_PORT + thread_id;

  // the set of all the keys that this particular thread is responsible for
  unordered_set<string> key_set;

  // the set of changes made on this thread since the last round of gossip
  unordered_set<string> local_changeset;
  worker_node_t wnode = worker_node_t(ip, thread_id);

  // initialize the thread's kvs replica
  unique_ptr<Database> kvs(new Database);

  // socket that respond to client requests
  zmq::socket_t responder(*context, ZMQ_REP);
  responder.bind(wnode.client_connection_bind_addr_);

  // socket that listens for distributed gossip
  zmq::socket_t dgossip_puller(*context, ZMQ_PULL);
  dgossip_puller.bind(wnode.distributed_gossip_bind_addr_);

  // socket that listens for local gossip
  zmq::socket_t lgossip_puller(*context, ZMQ_PULL);
  lgossip_puller.bind(wnode.local_gossip_addr_);

  // socket that listens for local gossip
  zmq::socket_t lredistribute_puller(*context, ZMQ_PULL);
  lredistribute_puller.bind(wnode.local_redistribute_addr_);

  // socket that listens for departure command
  zmq::socket_t depart_puller(*context, ZMQ_PULL);
  depart_puller.bind(wnode.local_depart_addr_);

  // used to communicate with master thread for changeset addresses
  zmq::socket_t changeset_address_requester(*context, ZMQ_REQ);
  changeset_address_requester.connect(CHANGESET_ADDR);

  // used to send gossip
  SocketCache cache(context, ZMQ_PUSH);

  //  Initialize poll set
  vector<zmq::pollitem_t> pollitems = {
    { static_cast<void *>(responder), 0, ZMQ_POLLIN, 0 },
    { static_cast<void *>(dgossip_puller), 0, ZMQ_POLLIN, 0 },
    { static_cast<void *>(lgossip_puller), 0, ZMQ_POLLIN, 0 },
    { static_cast<void *>(lredistribute_puller), 0, ZMQ_POLLIN, 0 },
    { static_cast<void *>(depart_puller), 0, ZMQ_POLLIN, 0 },
  };

  auto start = std::chrono::system_clock::now();
  auto end = std::chrono::system_clock::now();

  // Enter the event loop
  while (true) {
    zmq_util::poll(0, &pollitems);

    if (pollitems[0].revents & ZMQ_POLLIN) { // process a request from the client
      string data = zmq_util::recv_string(&responder);
      communication::Request req;
      req.ParseFromString(data);

      //  Process request
      string result = process_client_request(req, thread_id, local_changeset, key_set);

      //  Send reply back to client
      zmq_util::send_string(result, &responder);
    } else if (pollitems[1].revents & ZMQ_POLLIN) { // process distributed gossip
      cout << "Received distributed gossip on thread " + to_string(thread_id) + ".\n";

      string data = zmq_util::recv_string(&dgossip_puller);
      communication::Gossip gossip;
      gossip.ParseFromString(data);

      //  Process distributed gossip
      process_distributed_gossip(gossip, thread_id, key_set);
    } else if (pollitems[2].revents & ZMQ_POLLIN) { // process local gossip
      cout << "Received local gossip on thread " + to_string(thread_id) + ".\n";

      zmq::message_t msg;
      zmq_util::recv_msg(&lgossip_puller, msg);
      gossip_data* g_data = *(gossip_data **)(msg.data());

      //  Process local gossip
      process_local_gossip(g_data, thread_id, key_set);
    } else if (pollitems[3].revents & ZMQ_POLLIN) { // process a local redistribute request
      cout << "Received local redistribute request on thread " + to_string(thread_id) + ".\n";

      zmq::message_t msg;
      zmq_util::recv_msg(&lredistribute_puller, msg);

      redistribution_address* r_data = *(redistribution_address **)(msg.data());
      changeset_address c_address;
      unordered_set<string> remove_set;

      // converting the redistribution_address to a changeset_address
      for (auto map_it = r_data->begin(); map_it != r_data->end(); map_it++) {
        for (auto set_it = map_it->second.begin(); set_it != map_it->second.end(); set_it++) {
          c_address[map_it->first].insert(set_it->first);
          if (set_it->second) {
            remove_set.insert(set_it->first);
          }
        }
      }

      send_gossip(&c_address, cache, ip, thread_id);
      delete r_data;

      // remove keys in the remove set
      for (auto it = remove_set.begin(); it != remove_set.end(); it++) {
        key_set.erase(*it);
        string fname = get_ebs_path("ebs_" + to_string(thread_id) + "/" + *it);
        if(remove(fname.c_str()) != 0) {
          cout << "Error deleting file";
        } else {
          cout << "File successfully deleted";
        }
      }
    } else if (pollitems[4].revents & ZMQ_POLLIN) { // process a departure 
      cout << "Thread " + to_string(thread_id) + " received departure command.\n";

      string device_name = zmq_util::recv_string(&depart_puller);

      changeset_data_wrapper* data = new changeset_data_wrapper();
      data->local = true;
      data->c_data.first = port;

      // for every key this thread is responsible for, add it to the changeset
      // wrapper
      for (auto it = key_set.begin(); it != key_set.end(); it++) {
        (data->c_data.second).insert(*it);
      }

      zmq_util::send_msg((void*)data, &changeset_address_requester);
      zmq::message_t msg;
      zmq_util::recv_msg(&changeset_address_requester, msg);

      changeset_address* res = *(changeset_address **)(msg.data());
      send_gossip(res, cache, ip, thread_id);
      delete res;

      string shell_command;
      // remove the volume locally
      if (enable_ebs) {
        shell_command = "scripts/remove_volume.sh " + device_name + " " + to_string(thread_id);
      } else {
        shell_command = "scripts/remove_volume_dummy.sh " + device_name + " " + to_string(thread_id);
      }

      system(shell_command.c_str());
      zmq_util::send_string(device_name + ":" + to_string(thread_id), &cache[master_node_t(ip).local_depart_done_addr_]);
      
      // break because we have now successfully removed the volume and are
      // ending this thread's execution
      break;
    }

    end = std::chrono::system_clock::now();
    // check whether or not we should gossip
    if (chrono::duration_cast<std::chrono::seconds>(end-start).count() >= PERIOD || local_changeset.size() >= THRESHOLD) {
      // only gossip if we have changes
      if (local_changeset.size() > 0) {
        // populate the data that has changed
        changeset_data_wrapper* data = new changeset_data_wrapper();
        data->c_data.first = port;

        for (auto it = local_changeset.begin(); it != local_changeset.end(); it++) {
          (data->c_data.second).insert(*it);
        }

        // send a message to the master thread requesting the addresses of the
        // desinations for gossip
        zmq_util::send_msg((void*)data, &changeset_address_requester);
        zmq::message_t msg;
        zmq_util::recv_msg(&changeset_address_requester, msg);

        // send the gossip
        changeset_address* res = *(changeset_address **)(msg.data());
        send_gossip(res, cache, ip, thread_id);
        delete res;
        local_changeset.clear();
      }

      start = std::chrono::system_clock::now();
    }
  }
}

void add_thread(map<string, int> ebs_device_map, 
    unordered_map<int, string> inverse_ebs_device_map, 
    vector<thread> ebs_threads,
    ebs_hash_t ebs_hash_ring,
    unordered_map>string, key_info> placement,
    set<int> active_thread_id) {

  cout << "Adding a new thread.\n";
  string ebs_device_id;

  if (ebs_device_map.size() == 0) {
    ebs_device_id = "ba";
    ebs_device_map[ebs_device_id] = next_thread_id;
    inverse_ebs_device_map[next_thread_id] = ebs_device_id;
  } else {
    bool has_slot = false;
    for (auto it = ebs_device_map.begin(); it != ebs_device_map.end(); it++) {
      if (it->second == -1) {
        has_slot = true;
        ebs_device_id = it->first;

        ebs_device_map[it->first] = next_thread_id;
        inverse_ebs_device_map[next_thread_id] = it->first;
        break;
      }
    }

    if (!has_slot) {
      ebs_device_id = getNextDeviceID((ebs_device_map.rbegin())->first);
      ebs_device_map[ebs_device_id] = next_thread_id;
      inverse_ebs_device_map[next_thread_id] = ebs_device_id;
    }
  }

  cout << "Adding device with ID " + ebs_device_id + " and thread with ID " + to_string(next_thread_id) + ".\n";

  string shell_command;
  if (enable_ebs) {
    shell_command = "scripts/add_volume.sh " + ebs_device_id + " 10 " + to_string(next_thread_id);
  } else {
    shell_command = "scripts/add_volume_dummy.sh " + ebs_device_id + " 10 " + to_string(next_thread_id);
  }

  system(shell_command.c_str());
  ebs_threads.push_back(thread(ebs_worker_routine, &context, ip, next_thread_id));

  active_thread_id.insert(next_thread_id);
  ebs_hash_ring.insert(worker_node_t(ip, next_thread_id));

  // repartition data
  unordered_map<string, redistribution_address*> redistribution_map;

  string target_address = ip + ":" + to_string(SERVER_PORT + next_thread_id);
  for (auto it = placement.begin(); it != placement.end(); it++) {
    worker_node_t sender_node;
    bool remove = false;
    bool resp = responsible<worker_node_t, ebs_hasher>(it->first, it->second.local_ebs_replication_, ebs_hash_ring, target_address, sender_node, remove);

    if (resp) {
      cout << "The new thread is responsible for key " + it->first + ".\n";
      string sender_address = sender_node.local_redistribute_addr_;
      cout << "The sender address is " + sender_address + ".\n";

      if (redistribution_map.find(sender_address) == redistribution_map.end()) {
        redistribution_map[sender_address] = new redistribution_address();
      }

      (*redistribution_map[sender_address])[target_address].insert(pair<string, bool>(it->first, remove));
    }
  }

  for (auto it = redistribution_map.begin(); it != redistribution_map.end(); it++) {
    zmq_util::send_msg((void*)it->second, &cache[it->first]);
  }

  next_thread_id += 1;
}

void remove_thread(set<int> active_thread_id, ebs_hash_t ebs_hash_ring, unordered_map<int, string> inverse_ebs_device_map) {
  if (active_thread_id.rbegin() == active_thread_id.rend()) {
    cout << "Error: No remaining threads are running. Nothing to remove.\n";
  } else {
    int target_thread_id = *(active_thread_id.rbegin());
    cout << "Removing thread " + to_string(target_thread_id) + ".\n";

    worker_node_t wnode = worker_node_t(ip, target_thread_id);
    ebs_hash_ring.erase(wnode);
    active_thread_id.erase(target_thread_id);

    string device_id = inverse_ebs_device_map[target_thread_id];
    zmq_util::send_string(device_id, &cache[wnode.local_depart_addr_]);
  }
}

int main(int argc, char* argv[]) {
  if (argc != 3) {
    cerr << "usage:" << argv[0] << " <new_node> <enable_ebs>" << endl;
    return 1;
  }

  if (string(argv[1]) != "y" && string(argv[1]) != "n") {
    cerr << "Invalid first argument: " << string(argv[1]) << "." << endl;
    return 1;
  }

  if (string(argv[2]) != "y" && string(argv[2]) != "n") {
    cerr << "Invalid second argument: " << string(argv[2]) << "." << endl;
    return 1;
  }

  string ip = getIP();
  string new_node = argv[1];
  if (string(argv[2]) == "y") {
    enable_ebs = true;
  } else {
    enable_ebs = false;
  }

  master_node_t mnode = master_node_t(ip);

  // prepare the zmq context
  zmq::context_t context(1);
  SocketCache cache(&context, ZMQ_PUSH);
  SocketCache key_address_requesters(&context, ZMQ_REQ);

  // create our hash rings
  global_hash_t global_hash_ring;
  ebs_hash_t ebs_hash_ring;
  unordered_map<string, key_info> placement;

  set<int> active_ebs_thread_id = set<int>();
  for (int i = 1; i <= EBS_THREAD_NUM; i++) {
    active_ebs_thread_id.insert(i);
  }

  // read address of client proxies from conf file
  unordered_set<string> client_address;
  string ip_line;
  ifstream address;
  address.open("conf/server/client_address.txt");
  while (getline(address, ip_line)) {
    client_address.insert(ip_line);
  }
  address.close();

  // read server address from the file
  if (new_node == "n") {
    address.open("conf/server/start_servers.txt");
    // add yourself to the ring
    global_hash_ring.insert(mnode);

    // add all other servers
    while (getline(address, ip_line)) {
      global_hash_ring.insert(master_node_t(ip_line));
    }
    address.close();
  } else { // get server address from the seed node
    address.open("conf/server/seed_server.txt");
    getline(address, ip_line);
    address.close();

    // tell the seed node that you are joining
    zmq::socket_t addr_requester(context, ZMQ_REQ);
    addr_requester.connect(master_node_t(ip_line).seed_connection_connect_addr_);
    zmq_util::send_string("join", &addr_requester);

    // receive and add all the addresses that seed node sent
    vector<string> addresses;
    split(zmq_util::recv_string(&addr_requester), '|', addresses);
    for (auto it = addresses.begin(); it != addresses.end(); it++) {
      global_hash_ring.insert(master_node_t(*it));
    }

    // add itself to global hash ring
    global_hash_ring.insert(mnode);
  }

  // this map is ordered, so that we can figure out the device name when adding
  // new devices
  map<string, int> ebs_device_map;

  unordered_map<int, string> inverse_ebs_device_map;
  vector<thread> ebs_threads;

  // this is the smallest device name that we can have because of EBS
  // idiosyncrasies
  string eid = "ba";

  // create the initial volumes and start the initial threads based on
  // EBS_THREAD_NUM
  for (int thread_id = 1; thread_id <= EBS_THREAD_NUM; thread_id++) {
    ebs_device_map[eid] = thread_id;
    inverse_ebs_device_map[thread_id] = eid;

    cout << "device id to be added is " + eid + "\n";
    cout << "thread id is " + to_string(thread_id) + "\n";

    string shell_command;
    if (enable_ebs) {
      shell_command = "scripts/add_volume.sh " + eid + " 10 " + to_string(thread_id);
    }
    else {
      shell_command = "scripts/add_volume_dummy.sh " + eid + " 10 " + to_string(thread_id);
    }

    system(shell_command.c_str());
    eid = getNextDeviceID(eid);
    ebs_threads.push_back(thread(ebs_worker_routine, &context, ip, thread_id));
    ebs_hash_ring.insert(worker_node_t(ip, thread_id));
  }

  set<int> active_thread_id = set<int>();
  for (int i = 1; i <= EBS_THREAD_NUM; i++) {
    active_thread_id.insert(i);
  }

  if (new_node == "y") {
    // notify other servers that there is a new node
    for (auto it = global_hash_ring.begin(); it != global_hash_ring.end(); it++) {
      if (it->second.ip_.compare(ip) != 0) {
        zmq_util::send_string(ip, &cache[(it->second).node_join_connect_addr_]);
      }
    }
  }

  // notify clients that this node has joined the service
  for (auto it = client_address.begin(); it != client_address.end(); it++) {
    zmq_util::send_string("join:" + ip, &cache[master_node_t(*it).client_notify_connect_addr_]);
  }

  // responsible for sending the server address to a new node
  zmq::socket_t addr_responder(context, ZMQ_REP);
  addr_responder.bind(SEED_BIND_ADDR);

  // listens for a new node joining
  zmq::socket_t join_puller(context, ZMQ_PULL);
  join_puller.bind(NODE_JOIN_BIND_ADDR);

  // listens for a node departing
  zmq::socket_t depart_puller(context, ZMQ_PULL);
  depart_puller.bind(NODE_DEPART_BIND_ADDR);

  // responsible for sending the worker address (responsible for the requested key) to the client or other to servers
  zmq::socket_t key_address_responder(context, ZMQ_REP);
  key_address_responder.bind(KEY_EXCHANGE_BIND_ADDR);

  // responsible for responding to changeset address requests from workers
  zmq::socket_t changeset_address_responder(context, ZMQ_REP);
  changeset_address_responder.bind(CHANGESET_ADDR);

  // responsible for monitoring when a worker has finished departing
  zmq::socket_t depart_done_puller(context, ZMQ_PULL);
  depart_done_puller.bind(LOCAL_DEPART_DONE_ADDR);

  // responsible for listening for a command that this node should leave
  zmq::socket_t self_depart_responder(context, ZMQ_REP);
  self_depart_responder.bind(SELF_DEPART_BIND_ADDR);

  // set up zmq receivers
  zmq_pollitem_t pollitems [7];
  pollitems[0].socket = static_cast<void *>(addr_responder);
  pollitems[0].events = ZMQ_POLLIN;
  pollitems[1].socket = static_cast<void *>(join_puller);
  pollitems[1].events = ZMQ_POLLIN;
  pollitems[2].socket = static_cast<void *>(depart_puller);
  pollitems[2].events = ZMQ_POLLIN;
  pollitems[3].socket = static_cast<void *>(key_address_responder);
  pollitems[3].events = ZMQ_POLLIN;
  pollitems[4].socket = static_cast<void *>(changeset_address_responder);
  pollitems[4].events = ZMQ_POLLIN;
  pollitems[5].socket = static_cast<void *>(depart_done_puller);
  pollitems[5].events = ZMQ_POLLIN;
  pollitems[6].socket = static_cast<void *>(self_depart_responder);
  pollitems[6].events = ZMQ_POLLIN;


  string input;
  int next_thread_id = EBS_THREAD_NUM + 1;

  // enter event loop
  while (true) {
    zmq::poll(pollitems, 7, -1);

    if (pollitems[0].revents & ZMQ_POLLIN) {
      cout << "Received an address request.\n";
      string request = zmq_util::recv_string(&addr_responder);

      string addresses;
      for (auto it = global_hash_ring.begin(); it != global_hash_ring.end(); it++) {
        addresses += (it->second.ip_ + "|");
      }

      // remove the trailing pipe
      addresses.pop_back();
      zmq_util::send_string(addresses, &addr_responder);
    } else if (pollitems[1].revents & ZMQ_POLLIN) {
      string new_server_ip = zmq_util::recv_string(&join_puller);
      cout << "Received a node join. New node is " << new_server_ip << ".\n";
      master_node_t mnode = master_node_t(new_server_ip);

      // update global hash ring
      global_hash_ring.insert(mnode);

      // a map for each local worker that has to redistribute to the remote
      // node
      unordered_map<string, redistribution_address*> redistribution_map;

      // for each key in this set, we will ask the new node which worker the
      // key should be sent to
      unordered_set<string> key_to_query;

      // determine which keys should be removed
      unordered_map<string, bool> key_remove_map;

      // iterate through all of my keys
      for (auto it = placement.begin(); it != placement.end(); it++) {
        master_node_t sender_node;
        bool remove = false;

        string key = it->first;
        key_info info = it->second;

        // NOTE: We are assuming all keys have the default global replication
        // factor for now. This won't be true in the future.
        // TODO: can we clean up this logic? especially if we have the
        // constraint that there are never fewer nodes than the replication
        // factor
        bool resp = responsible<master_node_t, crc32_hasher>(key, info.global_ebs_replication_, global_hash_ring, mnode.id_, sender_node, remove);

        if (resp && (sender_node.ip_.compare(ip) == 0)) {
          key_to_query.insert(it->first);
          key_remove_map[it->first] = remove;
        }
      }

      // prepare & send key address request
      communication::Key_Request req;
      req.set_sender("server");
      for (auto it = key_to_query.begin(); it != key_to_query.end(); it++) {
        communication::Key_Request_Tuple* tp = req.add_tuple();
        tp->set_key(*it);
      }

      string key_req;
      req.SerializeToString(&key_req);
      // make a key exchange request to the new server for each of the keys we
      // are sending
      zmq_util::send_string(key_req, &key_address_requesters[mnode.key_exchange_connect_addr_]);

      string key_res = zmq_util::recv_string(&key_address_requesters[mnode.key_exchange_connect_addr_]);
      communication::Key_Response resp;
      resp.ParseFromString(key_res);

      // for each key in the response
      for (int i = 0; i < resp.tuple_size(); i++) {
        // for each of the workers we are sending the key to (if the local
        // replication factor is > 1)
        for (int j = 0; j < resp.tuple(i).address_size(); j++) {
          string key = resp.tuple(i).key();
          string target_address = resp.tuple(i).address(j).addr();

          // TODO; only send from *one* local replica not from many...
          // figure out how to tell local replica to *remove* but not gossip
          // for each replica of the key on this machine
          auto pos = ebs_hash_ring.find(key);
          for (int k = 0; k < placement[key].local_ebs_replication_; k++) {
            // create a redistribution_address object for each of the local
            // workers
            string worker_address = pos->second.local_redistribute_addr_;
            if (redistribution_map.find(worker_address) == redistribution_map.end()) {
              redistribution_map[worker_address] = new redistribution_address();
            }

            // insert this key into the redistrution address for each of the
            // local workers based on the destination
            (*redistribution_map[worker_address])[target_address].insert(pair<string, bool>(key, key_remove_map[key]));

            // go to the next position on the ring
            if (++pos == ebs_hash_ring.end()) {
              pos = ebs_hash_ring.begin();
            }
          }
        }
      }

      // send a local message for each local worker that has to redistribute
      // data to a remote node
      for (auto it = redistribution_map.begin(); it != redistribution_map.end(); it++) {
        zmq_util::send_msg((void*)it->second, &cache[it->first]);
      }
    } else if (pollitems[2].revents & ZMQ_POLLIN) {
      string departing_server_ip = zmq_util::recv_string(&depart_puller);
      cout << "Received departure for node " << departing_server_ip << ".\n";

      // update hash ring
      global_hash_ring.erase(master_node_t(departing_server_ip));
    } else if (pollitems[3].revents & ZMQ_POLLIN) {
      cout << "Received a key address request.\n";
      lww_timestamp++;

      string key_req = zmq_util::recv_string(&key_address_responder);
      communication::Key_Request req;
      req.ParseFromString(key_req);

      string sender = req.sender();
      communication::Key_Response res;

      // for every requested key
      for (int i = 0; i < req.tuple_size(); i++) {
        string key = req.tuple(i).key();
        cout << "Received a key request for key " + key + ".\n";

        communication::Key_Response_Tuple* tp = res.add_tuple();
        tp->set_key(key);

        // for now, just use EBS as tier and LOCAL_EBS_REPLICATION as rep factor
        // update placement metadata
        if (placement.find(key) == placement.end()) {
          placement[key] = key_info('E', GLOBAL_EBS_REPLICATION, LOCAL_EBS_REPLICATION);
        }

        // find all the local worker threads that are assigned to this key
        auto it = ebs_hash_ring.find(key);
        for (int i = 0; i < placement[key].local_ebs_replication_; i++) {
          // add each one to the response
          communication::Key_Response_Address* tp_addr = tp->add_address();
          tp_addr->set_addr(it->second.id_);

          if (++it == ebs_hash_ring.end()) {
            it = ebs_hash_ring.begin();
          }
        }

        string response;
        res.SerializeToString(&response);
        zmq_util::send_string(response, &key_address_responder);
      } 
    } else if (pollitems[4].revents & ZMQ_POLLIN) {
      cout << "Received a changeset address request from a worker thread.\n"

      zmq::message_t msg;
      zmq_util::recv_msg(&changeset_address_responder, msg);
      changeset_data_wrapper* data = *(changeset_data_wrapper **)(msg.data());
      changeset_data changeset = data->c_data;

      // determine the IP and port of the thread that made the request
      string self_id = ip + ":" + to_string(changeset.first);
      changeset_address* res = new changeset_address();
      unordered_map<master_node_t, unordered_set<string>, node_hash> node_map;

      // look for every key requestsed by the worker thread
      for (auto it = changeset.second.begin(); it != changeset.second.end(); it++) {
        string key = *it;
        // first, check the local ebs ring
        auto ebs_pos = ebs_hash_ring.find(key);
        for (int i = 0; i < placement[key].local_ebs_replication_; i++) {
          // add any thread that is responsible for this key but is not the
          // thread that made the request
          if (ebs_pos->second.id_.compare(self_id) != 0) {
            (*res)[ebs_pos->second.id_].insert(key);
          }

          if (++ebs_pos == ebs_hash_ring.end()) {
            ebs_pos = ebs_hash_ring.begin();
          }
        }

        // second, check the global ring for any nodes that might also be
        // responsible for this key
        if (!data->local) {
          auto pos = global_hash_ring.find(key);
          for (int i = 0; i < placement[key].global_ebs_replication_; i++) {
            if (pos->second.ip_.compare(ip) != 0) {
              node_map[pos->second].insert(key);
            }

            if (++pos == global_hash_ring.end()) {
              pos = global_hash_ring.begin();
            }
          }
        }
      }

      if (!data->local) {
        // for any remote nodes that should receive gossip, we make key
        // requests
        for (auto map_iter = node_map.begin(); map_iter != node_map.end(); map_iter++) {
          // create key request
          communication::Key_Request req;
          req.set_sender("server");

          // add each key that is going to this particular node
          for (auto set_iter = map_iter->second.begin(); set_iter != map_iter->second.end(); set_iter++) {
            communication::Key_Request_Tuple* tp = req.add_tuple();
            tp->set_key(*set_iter);
          }

          // send the request to the node
          string key_req;
          req.SerializeToString(&key_req);
          zmq_util::send_string(key_req, &key_address_requesters[map_iter->first.key_exchange_connect_addr_]);

          // receive the request
          string key_res = zmq_util::recv_string(&key_address_requesters[map_iter->first.key_exchange_connect_addr_]);
          communication::Key_Response resp;
          resp.ParseFromString(key_res);

          // for each key, add the address of *every* (there can be multiple)
          // worker thread on the other node that should receive this key
          for (int i = 0; i < resp.tuple_size(); i++) {
            for (int j = 0; j < resp.tuple(i).address_size(); j++) {
              (*res)[resp.tuple(i).address(j).addr()].insert(resp.tuple(i).key());
            }
          }
        }
      }

      // send the resulting changeset_address object back to the thread
      zmq_util::send_msg((void*)res, &changeset_address_responder);
      delete data;
    } else if (pollitems[5].revents & ZMQ_POLLIN) {
      // we wait for this message to come back from the worker thread because
      // we want to avoid a race condition where the master thread tries to
      // reuse the volume name before it's removed
      vector<string> v;
      split(zmq_util::recv_string(&depart_done_puller), ':', v);
      cout << "Thread " + v[1] + " has departed.\n";

      ebs_device_map[v[0]] = -1;
      inverse_ebs_device_map.erase(stoi(v[1]));
    } else if (pollitems[6].revents & ZMQ_POLLIN) {
      cout << "Node is departing.\n";

      global_hash_ring.erase(mnode);
      for (auto it = global_hash_ring.begin(); it != global_hash_ring.end(); it++) {
        zmq_util::send_string(ip, &cache[it->second.node_depart_connect_addr_]);
      }
      
      // notify clients
      for (auto it = client_address.begin(); it != client_address.end(); it++) {
        zmq_util::send_string("depart:" + ip, &cache[master_node_t(*it).client_notify_connect_addr_]);
      }

      // form the key_request map
      unordered_map<string, communication::Key_Request> key_request_map;
      for (auto it = placement.begin(); it != placement.end(); it++) {
        string key = it->first;
        auto pos = global_hash_ring.find(key);

        for (int i = 0; i < placement[key].global_ebs_replication_; i++) {
          key_request_map[pos->second.key_exchange_connect_addr_].set_sender("server");
          communication::Key_Request_Tuple* tp = key_request_map[pos->second.key_exchange_connect_addr_].add_tuple();
          tp->set_key(key);

          if (++pos == global_hash_ring.end()) {
            pos = global_hash_ring.begin();
          }
        }
      }

      // send key address requests to other server nodes
      unordered_map<string, redistribution_address*> redistribution_map;
      for (auto it = key_request_map.begin(); it != key_request_map.end(); it++) {
        string key_req;
        it->second.SerializeToString(&key_req);
        zmq_util::send_string(key_req, &key_address_requesters[it->first]);

        string key_res = zmq_util::recv_string(&key_address_requesters[it->first]);
        communication::Key_Response resp;
        resp.ParseFromString(key_res);

        for (int i = 0; i < resp.tuple_size(); i++) {
          for (int j = 0; j < resp.tuple(i).address_size(); j++) {
            string key = resp.tuple(i).key();
            string target_address = resp.tuple(i).address(j).addr();
            auto pos = ebs_hash_ring.find(key);

            for (int k = 0; k < placement[key].local_ebs_replication_; k++) {
              string worker_address = pos->second.local_redistribute_addr_;
              if (redistribution_map.find(worker_address) == redistribution_map.end()) {
                redistribution_map[worker_address] = new redistribution_address();
              }

              (*redistribution_map[worker_address])[target_address].insert(pair<string, bool>(key, false));

              if (++pos == ebs_hash_ring.end()) {
                pos = ebs_hash_ring.begin();
              }
            }
          }
        }
      }

      for (auto it = redistribution_map.begin(); it != redistribution_map.end(); it++) {
        zmq_util::send_msg((void*)it->second, &cache[it->first]);
      }

      cout << "Killing all EBS instances.\n";

      for (auto it = inverse_ebs_device_map.begin(); it != inverse_ebs_device_map.end(); it++) {
        active_thread_id.erase(it->first);
        ebs_hash_ring.erase(worker_node_t(ip, it->first));
        string shell_command;
        if (enable_ebs) {
          shell_command = "scripts/remove_volume.sh " + it->second + " " + to_string(it->first);
        } else {
          shell_command = "scripts/remove_volume_dummy.sh " + it->second + " " + to_string(it->first);
        }

        system(shell_command.c_str());
      }
      
      // TODO: once we break here, I don't think that the threads will have
      // finished. they will still be looping.
      break;
    } else {
      cout << "Invalid Request\n";
    }
  }

  for (auto& th: ebs_threads) {
    th.join();
  }

  return 0;
}
