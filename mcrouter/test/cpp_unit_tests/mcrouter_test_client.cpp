/**
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 */
#include "mcrouter_test_client.h"

#include <semaphore.h>

#include <queue>

#include "mcrouter/lib/fbi/cpp/sfrlock.h"
#include "mcrouter/lib/fbi/cpp/util.h"
#include "mcrouter/router.h"

using namespace facebook::memcache::mcrouter;

using folly::dynamic;
using std::string;

namespace facebook { namespace memcache { namespace test {
class ResultsSet {
public:
  ResultsSet() {
    sem_init(&outstanding_, 0, 0);
  }

  void push(std::pair<mc_msg_t*, mc_msg_t*> &&msg) {
    std::lock_guard<SFRWriteLock> lck(queueLock_.writeLock());
    replies_.push(std::move(msg));
    sem_post(&outstanding_);
  }

  bool try_pop(std::pair<mc_msg_t*, mc_msg_t*> &msg) {
    std::lock_guard<SFRReadLock> lck(queueLock_.readLock());
    if (replies_.empty()) {
      return false;
    } else {
      msg = replies_.front();
      replies_.pop();
      return true;
    }
  }

  void wait() {
    sem_wait(&outstanding_);
  }

private:
  SFRLock queueLock_;
  std::queue<std::pair<mc_msg_t*, mc_msg_t*>> replies_;
  sem_t outstanding_;
};
}}}

using namespace facebook::memcache::test;

static void on_reply(mcrouter_client_t *client,
                     mcrouter_msg_t *router_req,
                     void *context) {
  facebook::memcache::test::ResultsSet *rs =
    (facebook::memcache::test::ResultsSet*) context;
  mc_msg_incref(router_req->reply);
  rs->push(std::make_pair(router_req->req, router_req->reply));
}


MCRouterTestClient::MCRouterTestClient(const std::string& name,
                                       const McrouterOptions& opts) {
  rs_ = new ResultsSet();
  router_ = mcrouter_init(name, opts);
  client_ = mcrouter_client_new(router_,
                                nullptr,
                                {on_reply, nullptr},
                                rs_,
                                0, false);
}

MCRouterTestClient::~MCRouterTestClient() {
  delete rs_;
  mcrouter_client_disconnect(client_);
  mcrouter_free(router_);
}

static inline mcrouter_msg_t
make_get_request(const std::string& key) {
  mc_msg_t *mc_msg = mc_msg_new_with_key(key.c_str());
  mc_msg->op = mc_op_get;
  mcrouter_msg_t msg;
  msg.req = mc_msg;
  return msg;
}

static inline mcrouter_msg_t
make_delete_request(const std::string& key) {
  mc_msg_t *mc_msg = mc_msg_new_with_key(key.c_str());
  mc_msg->op = mc_op_delete;
  mcrouter_msg_t msg;
  msg.req = mc_msg;
  return msg;
}

static inline mcrouter_msg_t
make_set_request(const std::string& key,
                 const std::string& value) {
  mc_msg_t *mc_msg = mc_msg_new_with_key_and_value(key.c_str(),
                                                   value.data(),
                                                   value.size());
  mc_msg->op = mc_op_set;
  mcrouter_msg_t msg;
  msg.req = mc_msg;
  return msg;
}

bool MCRouterTestClient::issueRequests(const mcrouter_msg_t* msgs,
                                       size_t nreqs,
                                       dynamic &results) {
  bool no_errors = true;
  mcrouter_send(client_, msgs, nreqs);
  int outstanding = nreqs;
  while (outstanding > 0) {
    std::pair<mc_msg_t*, mc_msg_t*> pr;
    rs_->wait();
    rs_->try_pop(pr);

    mc_msg_t *req = pr.first;
    mc_msg_t *reply = pr.second;

    if (mc_res_is_err(reply->result)) {
      no_errors = false;
    } else {
      dynamic result = dynamic::object("result", (int) reply->result);
      if (reply->value.len > 0) {
        result["value"] = to<string>(reply->value);
      }
      results[to<string>(req->key)] = result;

      mc_msg_decref(req);
      mc_msg_decref(reply);
    }
    outstanding --;
  }

  return no_errors;
}

int MCRouterTestClient::get(const dynamic &keys,
                            dynamic &results) {
  mcrouter_msg_t msgs[keys.size()];
  int ret = 0;
  dynamic raw_results = dynamic::object;

  for (int i = 0; i < keys.size(); i ++) {
    msgs[i] = make_get_request(keys[i].asString().toStdString());
  }

  bool res = issueRequests(msgs, keys.size(), raw_results);
  assert(res);
  for ( auto & raw_reply : raw_results.items() ) {
    if (raw_reply.second["result"] == (int) mc_res_found) {
      results[raw_reply.first] = raw_reply.second["value"];
      ret ++;
    }
  }
  return ret;

}

int MCRouterTestClient::set(const dynamic &kv_pairs,
                            dynamic &results) {
  mcrouter_msg_t msgs[kv_pairs.size()];
  int i = 0;
  dynamic raw_results = dynamic::object;

  for (auto &kv_pair : kv_pairs.items()) {
    msgs[i] = make_set_request(kv_pair.first.asString().toStdString(),
                               kv_pair.second.asString().toStdString());
    i ++;
  }

  int ret = 0;
  bool res = issueRequests(msgs, kv_pairs.size(), raw_results);
  if (res) {
    for ( auto& raw_reply : raw_results.items()) {
      bool stored = (raw_reply.second["result"] == (int) mc_res_stored);
      results[raw_reply.first] = stored;
      ret += (int) stored;
    }
  }
  return ret;
}

int MCRouterTestClient::del(const dynamic &keys, bool local,
                            dynamic &results) {

  mcrouter_msg_t msgs[keys.size()];
  dynamic raw_results = dynamic::object;

  for (int i = 0; i < keys.size(); i ++) {
    auto key = keys[i].asString().toStdString();
    if (!local) {
      key = "/*/*/" + key;
    }
    msgs[i] = make_delete_request(key);
  }

  int ret = 0;
  bool res = issueRequests(msgs, keys.size(), raw_results);
  if (res) {
    for ( auto& raw_reply : raw_results.items() ) {
      bool found = (raw_reply.second["result"] == (int) mc_res_deleted);
      results[raw_reply.first] = found;
      ret += (int) found;
    }
  }
  return ret;
}


#pragma GCC diagnostic push // stats shadows a member
#pragma GCC diagnostic ignored "-Wshadow"
void MCRouterTestClient::stats(dynamic& out_stats, bool clear) {
  stat_t *stats;
  size_t num_stats;
  mcrouter_get_stats(router_, (int) clear, ods_stats, &stats, &num_stats);

  for (int i = 0; i < num_stats; i ++) {
    auto key = stats[i].name.str();
    switch (stats[i].type) {
      case stat_string:
        out_stats[key] = std::string(stats[i].data.string);
        break;
      case stat_uint64:
        out_stats[key] = stats[i].data.uint64;
        break;
      case stat_int64:
        out_stats[key] = stats[i].data.int64;
        break;
      case stat_double:
        out_stats[key] = stats[i].data.dbl;
        break;
      default:
        break;
    }
  }
}
#pragma GCC diagnostic pop