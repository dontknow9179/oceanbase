/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#include "rpc/obrpc/ob_poc_rpc_server.h"
#include "lib/oblog/ob_log_module.h"
#include "rpc/obrpc/ob_net_keepalive.h"

#define rk_log_macro(level, ret, format, ...) _OB_LOG_RET(level, ret, "PNIO " format, ##__VA_ARGS__)
#include "lib/lock/ob_futex.h"
extern "C" {
#include "rpc/pnio/interface/group.h"
};
#include "rpc/obrpc/ob_rpc_endec.h"
#define cfgi(k, v) atoi(getenv(k)?:v)

namespace oceanbase
{
namespace obrpc
{
extern const int easy_head_size;
ObPocRpcServer global_poc_server;
ObListener* global_ob_listener;
bool __attribute__((weak)) enable_pkt_nio() {
  return false;
}
}; // end namespace obrpc
}; // end namespace oceanbase

using namespace oceanbase::common;
using namespace oceanbase::obrpc;
using namespace oceanbase::rpc;

frame::ObReqDeliver* global_deliver;
int ObPocServerHandleContext::create(int64_t resp_id, const char* buf, int64_t sz, ObRequest*& req)
{
  int ret = OB_SUCCESS;
  ObPocServerHandleContext* ctx = NULL;
  ObRpcPacket tmp_pkt;
#ifndef PERF_MODE
  const int64_t alloc_payload_sz = sz;
#else
  const int64_t alloc_payload_sz = 0;
#endif
  if (OB_FAIL(tmp_pkt.decode(buf, sz))) {
    RPC_LOG(ERROR, "decode packet fail", K(ret));
  } else {
    obrpc::ObRpcPacketCode pcode = tmp_pkt.get_pcode();
    auto &set = obrpc::ObRpcPacketSet::instance();
    const char* pcode_label = set.name_of_idx(set.idx_of_pcode(pcode));
    const int64_t pool_size = sizeof(ObPocServerHandleContext) + sizeof(ObRequest) + sizeof(ObRpcPacket) + alloc_payload_sz;
    ObRpcMemPool* pool = ObRpcMemPool::create(tmp_pkt.get_tenant_id(), pcode_label, pool_size);
    void *temp = NULL;
    if (OB_ISNULL(pool)) {
      ret = common::OB_ALLOCATE_MEMORY_FAILED;
      RPC_LOG(WARN, "create memory pool failed", K(ret));
    } else if (OB_ISNULL(temp = pool->alloc(sizeof(ObPocServerHandleContext) + sizeof(ObRequest)))){
      ret = common::OB_ALLOCATE_MEMORY_FAILED;
      RPC_LOG(WARN, "pool allocate memory failed", K(ret));
    } else {
      ctx = new(temp)ObPocServerHandleContext(*pool, resp_id);
      req = new(ctx + 1)ObRequest(ObRequest::OB_RPC, ObRequest::TRANSPORT_PROTO_POC);
      ObRpcPacket* pkt = (ObRpcPacket*)pool->alloc(sizeof(ObRpcPacket) + alloc_payload_sz);
      if (NULL == pkt) {
        RPC_LOG(WARN, "pool allocate rpc packet memory failed", K(ret));
        ret = common::OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(reinterpret_cast<void *>(pkt), reinterpret_cast<void *>(&tmp_pkt), sizeof(ObRpcPacket));
        const char* packet_data = NULL;
        if (alloc_payload_sz > 0) {
          packet_data = reinterpret_cast<char *>(pkt + 1);
          MEMCPY(const_cast<char*>(packet_data), tmp_pkt.get_cdata(), tmp_pkt.get_clen());
        } else {
          packet_data = tmp_pkt.get_cdata();
        }
        int64_t receive_ts = ObTimeUtility::current_time();
        pkt->set_receive_ts(receive_ts);
        pkt->set_content(packet_data, tmp_pkt.get_clen());
        req->set_server_handle_context(ctx);
        req->set_packet(pkt);
        req->set_receive_timestamp(pkt->get_receive_ts());
        req->set_request_arrival_time(pkt->get_receive_ts());
        req->set_arrival_push_diff(common::ObTimeUtility::current_time());

        const int64_t fly_ts = receive_ts - pkt->get_timestamp();
        if (fly_ts > oceanbase::common::OB_MAX_PACKET_FLY_TS && TC_REACH_TIME_INTERVAL(100 * 1000)) {
          RPC_LOG(WARN, "PNIO packet wait too much time between proxy and server_cb", "pcode", pkt->get_pcode(),
                  "fly_ts", fly_ts, "send_timestamp", pkt->get_timestamp());
        }
      }
    }
  }
  return ret;
}

void ObPocServerHandleContext::resp(ObRpcPacket* pkt)
{
  int ret = OB_SUCCESS;
  int sys_err = 0;
  char* buf = NULL;
  int64_t sz = 0;
  if (NULL == pkt) {
    RPC_LOG(WARN, "resp pkt is null", K(pkt));
  } else if (OB_FAIL(rpc_encode_ob_packet(pool_, pkt, buf, sz))) {
    RPC_LOG(WARN, "rpc_encode_ob_packet fail", K(pkt));
    buf = NULL;
    sz = 0;
  }
  if ((sys_err = pn_resp(resp_id_, buf, sz)) != 0) {
    RPC_LOG(WARN, "pn_resp fail", K(resp_id_), K(sys_err));
  }
}

int serve_cb(int grp, const char* b, int64_t sz, uint64_t resp_id)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  if (NULL == b || sz <= easy_head_size) {
    tmp_ret = OB_INVALID_DATA;
    RPC_LOG(WARN, "rpc request is invalid", K(tmp_ret), K(b), K(sz));
  } else {
    b = b + easy_head_size;
    sz = sz - easy_head_size;
    ObRequest* req = NULL;
    if (OB_TMP_FAIL(ObPocServerHandleContext::create(resp_id, b, sz, req))) {
      RPC_LOG(WARN, "created req is null", K(tmp_ret), K(sz), K(resp_id));
    } else {
      global_deliver->deliver(*req);
    }
  }
  if (OB_SUCCESS != tmp_ret) {
    int sys_err = 0;
    if ((sys_err = pn_resp(resp_id, NULL, 0)) != 0) {
      RPC_LOG(WARN, "pn_resp fail", K(resp_id), K(sys_err));
    }
  }
  return ret;
}

int ObPocRpcServer::start(int port, int net_thread_count, frame::ObReqDeliver* deliver)
{
  int ret = OB_SUCCESS;
  // init pkt-nio framework
  int lfd = -1;
  int grp = 1;
  if ((lfd = pn_listen(port, serve_cb)) == -1) {
    ret = OB_SERVER_LISTEN_ERROR;
    RPC_LOG(ERROR, "pn_listen failed", K(ret));
  } else {
    global_deliver = deliver;
    int count = pn_provision(lfd, grp, net_thread_count);
    if (count != net_thread_count) {
      ret = OB_ERR_SYS;
      RPC_LOG(WARN, "pn_provision error", K(count), K(net_thread_count));
    }
    has_start_ = true;
  }
  return ret;
}
int ObPocRpcServer::update_tcp_keepalive_params(int64_t user_timeout) {
  int ret = OB_SUCCESS;
  if (pn_set_keepalive_timeout(user_timeout) != user_timeout) {
    ret = OB_INVALID_ARGUMENT;
    RPC_LOG(WARN, "invalid user_timeout", K(ret), K(user_timeout));
  }
  return ret;
}

bool ObPocRpcServer::client_use_pkt_nio() {
  return has_start() && enable_pkt_nio();
}

extern "C" {
void* pkt_nio_malloc(int64_t sz, const char* label) {
  ObMemAttr attr(OB_SERVER_TENANT_ID, label, ObCtxIds::PKT_NIO);
  return oceanbase::common::ob_malloc(sz, attr);
}
void pkt_nio_free(void *ptr) {
  oceanbase::common::ob_free(ptr);
}
bool server_in_black(struct sockaddr* sa) {
  easy_addr_t ez_addr;
  easy_inet_atoe(sa, &ez_addr);
  return ObNetKeepAlive::get_instance().in_black(ez_addr);
}
int dispatch_to_ob_listener(int accept_fd) {
  int ret = -1;
  if (oceanbase::obrpc::global_ob_listener) {
    ret = oceanbase::obrpc::global_ob_listener->do_one_event(accept_fd);
  }
  return ret;
}
#define PKT_NIO_MALLOC(sz, label)  pkt_nio_malloc(sz, label)
#define PKT_NIO_FREE(ptr)   pkt_nio_free(ptr)
#define SERVER_IN_BLACK(sa) server_in_black(sa)
#define DISPATCH_EXTERNAL(accept_fd) dispatch_to_ob_listener(accept_fd)
#include "rpc/pnio/pkt-nio.c"
};
