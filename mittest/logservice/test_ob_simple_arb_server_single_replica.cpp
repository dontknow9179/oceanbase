// Copyright (c) 2021 OceanBase
// OceanBase is licensed under Mulan PubL v2.
// You can use this software according to the terms and conditions of the Mulan PubL v2.
// You may obtain a copy of Mulan PubL v2 at:
//          http://license.coscl.org.cn/MulanPubL-2.0
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
// See the Mulan PubL v2 for more details.
#include <cstdio>
#include <gtest/gtest.h>
#include <signal.h>
#define private public
#include "env/ob_simple_log_cluster_env.h"
#undef private

const std::string TEST_NAME = "single_arb_server";

using namespace oceanbase::common;
using namespace oceanbase;
namespace oceanbase
{
namespace unittest
{

class TestObSimpleMutilArbServer : public ObSimpleLogClusterTestEnv
{
public:
  TestObSimpleMutilArbServer() :  ObSimpleLogClusterTestEnv()
  {}
};

int64_t ObSimpleLogClusterTestBase::member_cnt_ = 1;
int64_t ObSimpleLogClusterTestBase::node_cnt_ = 1;
bool ObSimpleLogClusterTestBase::need_add_arb_server_ = true;
std::string ObSimpleLogClusterTestBase::test_name_ = TEST_NAME;

bool check_dir_exist(const char *base_dir, const int64_t id)
{
  char dir[OB_MAX_FILE_NAME_LENGTH] = {'\0'};
  snprintf(dir, OB_MAX_FILE_NAME_LENGTH, "%s/tenant_%ld", base_dir, id);
  int ret = OB_SUCCESS;
  bool result = false;
  if (OB_FAIL(FileDirectoryUtils::is_exists(dir, result))) {
    CLOG_LOG(WARN, "dir is not exist", K(ret), K(errno), K(dir), K(dir));
  }
  return result;
}

TEST_F(TestObSimpleMutilArbServer, create_mutil_tenant)
{
  SET_CASE_LOG_FILE(TEST_NAME, "create_mutil_tenant");
  OB_LOGGER.set_log_level("TRACE");
  ObISimpleLogServer *iserver = get_cluster()[0];
  EXPECT_EQ(true, iserver->is_arb_server());
  ObSimpleArbServer *arb_server = dynamic_cast<ObSimpleArbServer*>(iserver);
  palflite::PalfEnvLiteMgr *palf_env_mgr = &arb_server->palf_env_mgr_;
  int64_t cluster_id = 1;
  sleep(2);
  CLOG_LOG(INFO, "one tenant");
  sleep(2);
  EXPECT_EQ(OB_SUCCESS, palf_env_mgr->create_palf_env_lite(palflite::PalfEnvKey(cluster_id, 1)));
  sleep(2);
  CLOG_LOG(INFO, "two tenant");
  sleep(2);
  EXPECT_EQ(OB_SUCCESS, palf_env_mgr->create_palf_env_lite(palflite::PalfEnvKey(cluster_id, 2)));
  sleep(2);
  CLOG_LOG(INFO, "three tenant");
  sleep(2);
  EXPECT_EQ(OB_SUCCESS, palf_env_mgr->create_palf_env_lite(palflite::PalfEnvKey(cluster_id, 3)));
  sleep(2);
  CLOG_LOG(INFO, "four tenant");
  sleep(2);
  {
    PalfBaseInfo info_2; info_2.generate_by_default();
    AccessMode mode(palf::AccessMode::APPEND);
    IPalfHandleImpl *ipalf_handle_impl_2 = NULL;
    palflite::PalfEnvLite *palf_env_lite_2 = NULL;
    EXPECT_EQ(OB_SUCCESS, palf_env_mgr->palf_env_lite_map_.get(palflite::PalfEnvKey(cluster_id, 2), palf_env_lite_2));
    EXPECT_EQ(OB_SUCCESS, palf_env_lite_2->create_palf_handle_impl(1, mode, info_2, ipalf_handle_impl_2));
    palf_env_lite_2->revert_palf_handle_impl(ipalf_handle_impl_2);
    palf_env_mgr->revert_palf_env_lite(palf_env_lite_2);
    CLOG_LOG(INFO, "revert_palf_env_lite2");
  }
  EXPECT_EQ(OB_SUCCESS, palf_env_mgr->remove_palf_env_lite(palflite::PalfEnvKey(cluster_id, 1)));
  EXPECT_EQ(OB_SUCCESS, palf_env_mgr->remove_palf_env_lite(palflite::PalfEnvKey(cluster_id, 2)));
  EXPECT_EQ(OB_SUCCESS, palf_env_mgr->remove_palf_env_lite(palflite::PalfEnvKey(cluster_id, 3)));
  EXPECT_EQ(false, check_dir_exist(palf_env_mgr->base_dir_, 1));
  EXPECT_EQ(false, check_dir_exist(palf_env_mgr->base_dir_, 2));
  EXPECT_EQ(false, check_dir_exist(palf_env_mgr->base_dir_, 3));
  CLOG_LOG(INFO, "before restart_paxos_groups1");
  EXPECT_EQ(OB_SUCCESS, restart_paxos_groups());
  auto &map = palf_env_mgr->cluster_meta_info_map_;
  EXPECT_EQ(OB_SUCCESS, palf_env_mgr->create_palf_env_lite(palflite::PalfEnvKey(cluster_id, 1)));
  palflite::ClusterMetaInfo info;
  EXPECT_EQ(OB_SUCCESS, map.get_refactored(cluster_id, info));
  EXPECT_EQ(2, info.tenant_count_);
  EXPECT_EQ(OB_SUCCESS, palf_env_mgr->create_palf_env_lite(palflite::PalfEnvKey(cluster_id, 2)));
  EXPECT_EQ(OB_SUCCESS, map.get_refactored(cluster_id, info));
  EXPECT_EQ(3, info.tenant_count_);
  EXPECT_EQ(OB_SUCCESS, palf_env_mgr->create_palf_env_lite(palflite::PalfEnvKey(cluster_id, 3)));
  EXPECT_EQ(OB_SUCCESS, map.get_refactored(cluster_id, info));
  EXPECT_EQ(4, info.tenant_count_);
  EXPECT_EQ(OB_SUCCESS, palf_env_mgr->create_arbitration_instance(
        palflite::PalfEnvKey(cluster_id, 1), arb_server->self_, 1001,
        ObTenantRole(ObTenantRole::PRIMARY_TENANT)));
  auto update_meta = [](palflite::PalfEnvLiteMgr::MapPair &pair) -> void{
    auto &info = pair.second;
    info.epoch_ = arbserver::GCMsgEpoch(100, 100);
  };
  EXPECT_EQ(OB_SUCCESS, palf_env_mgr->update_cluster_meta_info_(cluster_id, update_meta));
  EXPECT_EQ(OB_SUCCESS, palf_env_mgr->get_cluster_meta_info_(cluster_id, info));
  EXPECT_EQ(arbserver::GCMsgEpoch(100, 100), info.epoch_);
  CLOG_LOG(INFO, "before restart_paxos_groups2");
  EXPECT_EQ(OB_SUCCESS, restart_paxos_groups());
  {
    palflite::PalfEnvLite *palf_env_lite_2 = NULL;
    EXPECT_EQ(OB_SUCCESS, palf_env_mgr->palf_env_lite_map_.get(palflite::PalfEnvKey(cluster_id, 2), palf_env_lite_2));
    palf_env_mgr->revert_palf_env_lite(palf_env_lite_2);
  }
  EXPECT_EQ(OB_SUCCESS, palf_env_mgr->remove_palf_env_lite(palflite::PalfEnvKey(cluster_id, 1)));
  EXPECT_EQ(OB_SUCCESS, map.get_refactored(cluster_id, info));
  EXPECT_EQ(3, info.tenant_count_);
  EXPECT_EQ(OB_SUCCESS, palf_env_mgr->remove_palf_env_lite(palflite::PalfEnvKey(cluster_id, 2)));
  EXPECT_EQ(OB_SUCCESS, map.get_refactored(cluster_id, info));
  EXPECT_EQ(2, info.tenant_count_);
  EXPECT_EQ(OB_SUCCESS, palf_env_mgr->remove_palf_env_lite(palflite::PalfEnvKey(cluster_id, 3)));
  EXPECT_EQ(OB_SUCCESS, map.get_refactored(cluster_id, info));
  EXPECT_EQ(1, info.tenant_count_);
  EXPECT_EQ(OB_ENTRY_NOT_EXIST, palf_env_mgr->remove_palf_env_lite(palflite::PalfEnvKey(cluster_id, 1001)));
  CLOG_LOG(INFO, "end restart_paxos_groups");
}

TEST_F(TestObSimpleMutilArbServer, out_interface)
{
  SET_CASE_LOG_FILE(TEST_NAME, "out_interface");
  OB_LOGGER.set_log_level("TRACE");
  ObISimpleLogServer *iserver = get_cluster()[0];
  EXPECT_EQ(true, iserver->is_arb_server());
  ObMember member(iserver->get_addr(), 100);
  ObSimpleArbServer *arb_server = dynamic_cast<ObSimpleArbServer*>(iserver);
  ObTenantRole tenant_role(ObTenantRole::PRIMARY_TENANT);
  int64_t cluster_id = 1;
  EXPECT_EQ(OB_ENTRY_NOT_EXIST, arb_server->palf_env_mgr_.set_initial_member_list(
        palflite::PalfEnvKey(cluster_id, 1), arb_server->self_,
        1000, get_member_list(), member, get_member_cnt()));
  EXPECT_EQ(OB_SUCCESS, arb_server->palf_env_mgr_.create_arbitration_instance(
        palflite::PalfEnvKey(cluster_id, 1), arb_server->self_,
        1000, tenant_role));
  EXPECT_EQ(OB_SUCCESS, arb_server->palf_env_mgr_.create_arbitration_instance(
        palflite::PalfEnvKey(cluster_id, 1), arb_server->self_,
        1000, tenant_role));
  EXPECT_EQ(OB_INVALID_ARGUMENT, arb_server->palf_env_mgr_.set_initial_member_list(
        palflite::PalfEnvKey(cluster_id, 1), arb_server->self_,
        1000, get_member_list(), member, get_member_cnt()));
  ObMemberList member_list = get_member_list();
  member_list.add_server(arb_server->self_);
  EXPECT_EQ(OB_INVALID_ARGUMENT, arb_server->palf_env_mgr_.set_initial_member_list(
        palflite::PalfEnvKey(cluster_id, 1), arb_server->self_,
        1000, member_list, member, get_member_cnt()));
  EXPECT_EQ(OB_SUCCESS, arb_server->palf_env_mgr_.delete_arbitration_instance(
        palflite::PalfEnvKey(cluster_id, 1), arb_server->self_, 1000));
  palflite::PalfEnvLite *palf_env_lite = NULL;
  EXPECT_EQ(OB_SUCCESS, arb_server->palf_env_mgr_.get_palf_env_lite(
        palflite::PalfEnvKey(cluster_id, OB_SERVER_TENANT_ID), palf_env_lite));
  arb_server->palf_env_mgr_.revert_palf_env_lite(palf_env_lite);
  EXPECT_EQ(OB_SUCCESS, arb_server->palf_env_mgr_.remove_palf_env_lite(
        palflite::PalfEnvKey(cluster_id, OB_SERVER_TENANT_ID)));
  EXPECT_EQ(OB_SUCCESS, arb_server->palf_env_mgr_.delete_arbitration_instance(
        palflite::PalfEnvKey(cluster_id, 1), arb_server->self_, 1000));
  CLOG_LOG(INFO, "end test out_interface");
}

} // end unittest
} // end oceanbase

int main(int argc, char **argv)
{
  RUN_SIMPLE_LOG_CLUSTER_TEST(TEST_NAME);
}
