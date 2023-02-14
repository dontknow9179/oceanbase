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

const std::string TEST_NAME = "arb_service";

using namespace oceanbase::common;
using namespace oceanbase;
namespace oceanbase
{
using namespace logservice;

namespace logservice
{

void ObArbitrationService::update_arb_timeout_()
{
  arb_timeout_us_ = 2 * 1000 * 1000L;
  if (REACH_TIME_INTERVAL(2 * 1000 * 1000)) {
    CLOG_LOG_RET(WARN, OB_ERR_UNEXPECTED, "update_arb_timeout_", K_(self), K_(arb_timeout_us));
  }
}
}

namespace unittest
{

class TestObSimpleLogClusterArbService : public ObSimpleLogClusterTestEnv
{
public:
  TestObSimpleLogClusterArbService() :  ObSimpleLogClusterTestEnv()
  {}
};

int64_t ObSimpleLogClusterTestBase::member_cnt_ = 3;
int64_t ObSimpleLogClusterTestBase::node_cnt_ = 5;
bool ObSimpleLogClusterTestBase::need_add_arb_server_ = true;
std::string ObSimpleLogClusterTestBase::test_name_ = TEST_NAME;

TEST_F(TestObSimpleLogClusterArbService, test_2f1a_degrade_upgrade)
{
  oceanbase::common::ObClusterVersion::get_instance().cluster_version_ = CLUSTER_VERSION_4_1_0_0;
  SET_CASE_LOG_FILE(TEST_NAME, "arb_2f1a_degrade_upgrade");
  OB_LOGGER.set_log_level("DEBUG");
  MockLocCB loc_cb;
  int ret = OB_SUCCESS;
  PALF_LOG(INFO, "begin test_2f1a_degrade_upgrade");
	int64_t leader_idx = 0;
  int64_t arb_replica_idx = -1;
  PalfHandleImplGuard leader;
  std::vector<PalfHandleImplGuard*> palf_list;
  const int64_t CONFIG_CHANGE_TIMEOUT = 10 * 1000 * 1000L; // 10s
	const int64_t id = ATOMIC_AAF(&palf_id_, 1);
  common::ObMember dummy_member;
	EXPECT_EQ(OB_SUCCESS, create_paxos_group_with_arb(id, arb_replica_idx, leader_idx, leader));
  EXPECT_EQ(OB_SUCCESS, get_cluster_palf_handle_guard(id, palf_list));
  const int64_t another_f_idx = (leader_idx+1)%3;
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 100, id));
  sleep(2);
  // 为备副本设置location cb，用于备副本找leader
  palf_list[another_f_idx]->get_palf_handle_impl()->set_location_cache_cb(&loc_cb);
  block_net(leader_idx, another_f_idx);
  // do not check OB_SUCCESS, may return OB_NOT_MASTER during degrading member
  submit_log(leader, 100, id);

  bool has_degraded = false;
  while (!has_degraded) {
    common::GlobalLearnerList degraded_learner_list;
    leader.palf_handle_impl_->config_mgr_.get_degraded_learner_list(degraded_learner_list);
    has_degraded = degraded_learner_list.contains(palf_list[another_f_idx]->palf_handle_impl_->self_);
    sleep(1);
    PALF_LOG(INFO, "wait degrade");
  }
  EXPECT_TRUE(has_degraded);
  // 等待lease过期，验证location cb是否可以找到leader
  sleep(5);
  loc_cb.leader_ = leader.palf_handle_impl_->self_;
  unblock_net(leader_idx, another_f_idx);
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 1, id));

  bool has_upgraded = false;
  while (!has_upgraded) {
    common::GlobalLearnerList degraded_learner_list;
    leader.palf_handle_impl_->config_mgr_.get_degraded_learner_list(degraded_learner_list);
    has_upgraded = (0 == degraded_learner_list.get_member_number());
    sleep(1);
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, 1, id));
    PALF_LOG(INFO, "wait upgrade");
  }
  revert_cluster_palf_handle_guard(palf_list);
  leader.reset();
  delete_paxos_group(id);
  PALF_LOG(INFO, "end test_2f1a_degrade_upgrade", K(id));
}

TEST_F(TestObSimpleLogClusterArbService, test_4f1a_degrade_upgrade)
{
  SET_CASE_LOG_FILE(TEST_NAME, "arb_4f1a_degrade_upgrade");
  OB_LOGGER.set_log_level("TRACE");
  MockLocCB loc_cb;
  int ret = OB_SUCCESS;
  PALF_LOG(INFO, "begin test_4f1a_degrade_upgrade");
	int64_t leader_idx = 0;
  int64_t arb_replica_idx = -1;
  PalfHandleImplGuard leader;
  const int64_t CONFIG_CHANGE_TIMEOUT = 10 * 1000 * 1000L; // 10s
	const int64_t id = ATOMIC_AAF(&palf_id_, 1);
  common::ObMember dummy_member;
  std::vector<PalfHandleImplGuard*> palf_list;
	EXPECT_EQ(OB_SUCCESS, create_paxos_group_with_arb(id, arb_replica_idx, leader_idx, leader));
  EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->add_member(ObMember(get_cluster()[3]->get_addr(), 1), 3, CONFIG_CHANGE_TIMEOUT));
  EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->add_member(ObMember(get_cluster()[4]->get_addr(), 1), 4, CONFIG_CHANGE_TIMEOUT));
  EXPECT_EQ(OB_SUCCESS, get_cluster_palf_handle_guard(id, palf_list));

  const int64_t another_f1_idx = (leader_idx+3)%5;
  const int64_t another_f2_idx = (leader_idx+4)%5;
  palf_list[another_f1_idx]->palf_handle_impl_->set_location_cache_cb(&loc_cb);
  palf_list[another_f2_idx]->palf_handle_impl_->set_location_cache_cb(&loc_cb);
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 100, id));
  sleep(2);
  block_all_net(another_f1_idx);
  block_all_net(another_f2_idx);

  bool has_degraded = false;
  while (!has_degraded) {
    common::GlobalLearnerList degraded_learner_list;
    leader.palf_handle_impl_->config_mgr_.get_degraded_learner_list(degraded_learner_list);
    has_degraded = degraded_learner_list.contains(get_cluster()[another_f1_idx]->get_addr());
    has_degraded = has_degraded && degraded_learner_list.contains(get_cluster()[another_f2_idx]->get_addr());
    sleep(1);
  }
  EXPECT_TRUE(has_degraded);
  // 确保lease过期，验证loc_cb是否可以找到leader拉日志
  sleep(5);
  unblock_all_net(another_f1_idx);
  unblock_all_net(another_f2_idx);
  loc_cb.leader_ = leader.palf_handle_impl_->self_;
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 1, id));

  bool has_upgraded = false;
  while (!has_upgraded) {
    common::GlobalLearnerList degraded_learner_list;
    leader.palf_handle_impl_->config_mgr_.get_degraded_learner_list(degraded_learner_list);
    has_upgraded = (0 == degraded_learner_list.get_member_number());
    sleep(1);
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, 1, id));
  }
  revert_cluster_palf_handle_guard(palf_list);
  leader.reset();
  delete_paxos_group(id);
  PALF_LOG(INFO, "end test_4f1a_degrade_upgrade", K(id));
}

TEST_F(TestObSimpleLogClusterArbService, test_2f1a_reconfirm_degrade_upgrade)
{
  SET_CASE_LOG_FILE(TEST_NAME, "arb_2f1a_reconfirm_test");
  OB_LOGGER.set_log_level("TRACE");
  int ret = OB_SUCCESS;
  PALF_LOG(INFO, "begin test_2f1a_reconfirm_degrade_upgrade");
  MockLocCB loc_cb;
	int64_t leader_idx = 0;
  int64_t arb_replica_idx = -1;
  PalfHandleImplGuard leader;
  std::vector<PalfHandleImplGuard*> palf_list;
  const int64_t CONFIG_CHANGE_TIMEOUT = 10 * 1000 * 1000L; // 10s
	const int64_t id = ATOMIC_AAF(&palf_id_, 1);
  common::ObMember dummy_member;
	EXPECT_EQ(OB_SUCCESS, create_paxos_group_with_arb(id, arb_replica_idx, leader_idx, leader));
  EXPECT_EQ(OB_SUCCESS, get_cluster_palf_handle_guard(id, palf_list));
  const int64_t another_f_idx = (leader_idx+1)%3;
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 100, id));
  sleep(2);
  palf_list[leader_idx]->palf_handle_impl_->set_location_cache_cb(&loc_cb);
  // block net of old leader, new leader will be elected
  // and degrade in RECONFIRM state
  block_net(leader_idx, another_f_idx);
  block_net(leader_idx, arb_replica_idx);
  // block_net后会理解进行降级操作，导致旧主上有些单副本写成功的日志被committed
  submit_log(leader, 20, id);
  // submit some logs which will be truncated

  bool has_degraded = false;
  while (!has_degraded) {
    common::GlobalLearnerList degraded_learner_list;
    palf_list[another_f_idx]->palf_handle_impl_->config_mgr_.get_degraded_learner_list(degraded_learner_list);
    ObAddr old_leader_addr = palf_list[leader_idx]->palf_handle_impl_->self_;
    has_degraded = degraded_learner_list.contains(old_leader_addr);
    PALF_LOG(INFO, "check has_degraded", K(has_degraded), K(degraded_learner_list), K(old_leader_addr));
    sleep(1);
  }
  EXPECT_TRUE(has_degraded);
  int64_t new_leader_idx = -1;
  PalfHandleImplGuard new_leader;
  EXPECT_EQ(OB_SUCCESS, get_leader(id, new_leader, new_leader_idx));
  sleep(5);
  loc_cb.leader_ = new_leader.palf_handle_impl_->self_;
  unblock_net(leader_idx, another_f_idx);
  unblock_net(leader_idx, arb_replica_idx);
  EXPECT_EQ(OB_SUCCESS, submit_log(new_leader, 100, id));

  bool has_upgraded = false;
  while (!has_upgraded) {
    common::GlobalLearnerList degraded_learner_list;
    new_leader.palf_handle_impl_->config_mgr_.get_degraded_learner_list(degraded_learner_list);
    has_upgraded = (0 == degraded_learner_list.get_member_number());
    PALF_LOG(INFO, "wait upgrade", K(degraded_learner_list));
    EXPECT_EQ(OB_SUCCESS, submit_log(new_leader, 1, id));
    sleep(1);
  }
  revert_cluster_palf_handle_guard(palf_list);
  leader.reset();
  new_leader.reset();
  delete_paxos_group(id);
  PALF_LOG(INFO, "end test_2f1a_reconfirm_degrade_upgrade", K(id));
}

TEST_F(TestObSimpleLogClusterArbService, test_4f1a_reconfirm_degrade_upgrade)
{
  SET_CASE_LOG_FILE(TEST_NAME, "arb_4f1a_reconfirm_test");
  OB_LOGGER.set_log_level("TRACE");
  MockLocCB loc_cb;
  int ret = OB_SUCCESS;
  PALF_LOG(INFO, "begin test_4f1a_reconfirm_degrade_upgrade");
	int64_t leader_idx = 0;
  int64_t arb_replica_idx = -1;
  auto cluster = get_cluster();
  PalfHandleImplGuard leader;
  const int64_t CONFIG_CHANGE_TIMEOUT = 10 * 1000 * 1000L; // 10s
	const int64_t id = ATOMIC_AAF(&palf_id_, 1);
  common::ObMember dummy_member;
  std::vector<PalfHandleImplGuard*> palf_list;

	EXPECT_EQ(OB_SUCCESS, create_paxos_group_with_arb(id, arb_replica_idx, leader_idx, leader));
  EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->add_member(ObMember(get_cluster()[3]->get_addr(), 1), 3, CONFIG_CHANGE_TIMEOUT));
  EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->add_member(ObMember(get_cluster()[4]->get_addr(), 1), 4, CONFIG_CHANGE_TIMEOUT));

  EXPECT_EQ(OB_SUCCESS, get_cluster_palf_handle_guard(id, palf_list));

  const int64_t another_f1_idx = 3;
  const int64_t another_f2_idx = 4;
  palf_list[leader_idx]->palf_handle_impl_->set_location_cache_cb(&loc_cb);
  palf_list[another_f1_idx]->palf_handle_impl_->set_location_cache_cb(&loc_cb);
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 100, id));
  sleep(2);
  // stop leader and a follower
  block_all_net(leader_idx);
  block_all_net(another_f1_idx);

  //EXPECT_EQ(OB_SUCCESS, submit_log(leader, 100, id));
  // wait for new leader is elected
  int64_t new_leader_idx = leader_idx;
  PalfHandleImplGuard new_leader;
  while (leader_idx == new_leader_idx) {
    new_leader.reset();
    EXPECT_EQ(OB_SUCCESS, get_leader(id, new_leader, new_leader_idx));
  }

  bool has_degraded = false;
  while (!has_degraded) {
    common::GlobalLearnerList degraded_learner_list;
    new_leader.palf_handle_impl_->config_mgr_.get_degraded_learner_list(degraded_learner_list);
    has_degraded = degraded_learner_list.contains(cluster[another_f1_idx]->get_addr());
    has_degraded = has_degraded && degraded_learner_list.contains(cluster[leader_idx]->get_addr());
    sleep(1);
    EXPECT_EQ(OB_SUCCESS, submit_log(new_leader, 1, id));
  }
  EXPECT_TRUE(has_degraded);

  sleep(5);
  loc_cb.leader_ = new_leader.palf_handle_impl_->self_;
  // restart two servers
  unblock_all_net(leader_idx);
  unblock_all_net(another_f1_idx);

  EXPECT_EQ(OB_SUCCESS, submit_log(new_leader, 100, id));

  bool has_upgraded = false;
  while (!has_upgraded) {
    common::GlobalLearnerList degraded_learner_list;
    new_leader.palf_handle_impl_->config_mgr_.get_degraded_learner_list(degraded_learner_list);
    has_upgraded = (0 == degraded_learner_list.get_member_number());
    PALF_LOG(INFO, "wait upgrade", K(ret), K(degraded_learner_list));
    EXPECT_EQ(OB_SUCCESS, submit_log(new_leader, 1, id));
    sleep(1);
  }
  leader.reset();
  new_leader.reset();
  revert_cluster_palf_handle_guard(palf_list);
  delete_paxos_group(id);
  PALF_LOG(INFO, "end test_4f1a_reconfirm_degrade_upgrade", K(id));
}

TEST_F(TestObSimpleLogClusterArbService, test_2f1a_config_change)
{
  SET_CASE_LOG_FILE(TEST_NAME, "arb_2f1a_config_change");
  OB_LOGGER.set_log_level("DEBUG");
  MockLocCB loc_cb;
  int ret = OB_SUCCESS;
  PALF_LOG(INFO, "begin arb_2f1a_config_change");
	int64_t leader_idx = 0;
  int64_t arb_replica_idx = -1;
  PalfHandleImplGuard leader;
  std::vector<PalfHandleImplGuard*> palf_list;
  const int64_t CONFIG_CHANGE_TIMEOUT = 10 * 1000 * 1000L; // 10s
	const int64_t id = ATOMIC_AAF(&palf_id_, 1);
  common::ObMember dummy_member;
	EXPECT_EQ(OB_SUCCESS, create_paxos_group_with_arb(id, arb_replica_idx, leader_idx, leader));
  EXPECT_EQ(OB_SUCCESS, get_cluster_palf_handle_guard(id, palf_list));
  // 为备副本设置location cb，用于备副本找leader
  const int64_t another_f_idx = (leader_idx+1)%3;
  loc_cb.leader_ = leader.palf_handle_impl_->self_;
  palf_list[another_f_idx]->get_palf_handle_impl()->set_location_cache_cb(&loc_cb);
  palf_list[3]->get_palf_handle_impl()->set_location_cache_cb(&loc_cb);
  palf_list[4]->get_palf_handle_impl()->set_location_cache_cb(&loc_cb);
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 100, id));
  sleep(2);

  // replace member
  EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->replace_member(
      ObMember(palf_list[3]->palf_handle_impl_->self_, 1),
      ObMember(palf_list[another_f_idx]->palf_handle_impl_->self_, 1),
      CONFIG_CHANGE_TIMEOUT));

  // add learner
  EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->add_learner(
      ObMember(palf_list[4]->palf_handle_impl_->self_, 1),
      CONFIG_CHANGE_TIMEOUT));

  // switch learner
  EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->switch_learner_to_acceptor(
      ObMember(palf_list[4]->palf_handle_impl_->self_, 1),
      CONFIG_CHANGE_TIMEOUT));
  revert_cluster_palf_handle_guard(palf_list);
  leader.reset();
  delete_paxos_group(id);
  PALF_LOG(INFO, "end arb_2f1a_config_change", K(id));
}

TEST_F(TestObSimpleLogClusterArbService, test_2f1a_arb_with_highest_version)
{
  oceanbase::common::ObClusterVersion::get_instance().cluster_version_ = CLUSTER_VERSION_4_1_0_0;
  SET_CASE_LOG_FILE(TEST_NAME, "test_2f1a_arb_with_highest_version");
  OB_LOGGER.set_log_level("DEBUG");
  MockLocCB loc_cb;
  int ret = OB_SUCCESS;
  PALF_LOG(INFO, "begin test_2f1a_arb_with_highest_version");
	int64_t leader_idx = 0;
  int64_t arb_replica_idx = -1;
  PalfHandleImplGuard leader;
  std::vector<PalfHandleImplGuard*> palf_list;
  const int64_t CONFIG_CHANGE_TIMEOUT = 10 * 1000 * 1000L; // 10s
	const int64_t id = ATOMIC_AAF(&palf_id_, 1);
  common::ObMember dummy_member;
	EXPECT_EQ(OB_SUCCESS, create_paxos_group_with_arb(id, arb_replica_idx, leader_idx, leader));
  EXPECT_EQ(OB_SUCCESS, get_cluster_palf_handle_guard(id, palf_list));
  // 为备副本设置location cb，用于备副本找leader
  const int64_t another_f_idx = (leader_idx+1)%3;
  loc_cb.leader_ = leader.palf_handle_impl_->self_;
  palf_list[another_f_idx]->get_palf_handle_impl()->set_location_cache_cb(&loc_cb);
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 500, id));
  sleep(2);

  LogConfigChangeArgs args(ObMember(palf_list[3]->palf_handle_impl_->self_, 1), 0, ADD_LEARNER);
  const int64_t proposal_id = leader.palf_handle_impl_->state_mgr_.get_proposal_id();
  const int64_t election_epoch = leader.palf_handle_impl_->state_mgr_.get_leader_epoch();
  LogConfigVersion config_version;
  EXPECT_EQ(OB_EAGAIN, leader.palf_handle_impl_->config_mgr_.change_config(args, proposal_id, election_epoch, config_version));
  // learner list and state_ has been changed
  EXPECT_TRUE(config_version.is_valid());
  EXPECT_EQ(1, leader.palf_handle_impl_->config_mgr_.state_);
  // only send config log to arb member
  ObMemberList member_list;
  member_list.add_server(get_cluster()[2]->get_addr());
  const int64_t prev_log_proposal_id = leader.palf_handle_impl_->config_mgr_.prev_log_proposal_id_;
  const LSN prev_lsn = leader.palf_handle_impl_->config_mgr_.prev_lsn_;
  const int64_t prev_mode_pid = leader.palf_handle_impl_->config_mgr_.prev_mode_pid_;
  const LogConfigMeta config_meta = leader.palf_handle_impl_->config_mgr_.log_ms_meta_;
  EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->log_engine_.submit_change_config_meta_req( \
    member_list, proposal_id, prev_log_proposal_id, prev_lsn, prev_mode_pid, config_meta));
  sleep(1);
  // check if arb member has received and persisted the config log
  while (true) {
    PalfHandleLiteGuard arb_member;
    if (OB_FAIL(get_arb_member_guard(id, arb_member))) {
    } else if (arb_member.palf_handle_lite_->config_mgr_.persistent_config_version_ == config_version) {
      break;
    } else {
      EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->log_engine_.submit_change_config_meta_req( \
        member_list, proposal_id, prev_log_proposal_id, prev_lsn, prev_mode_pid, config_meta));
    }
    ::ob_usleep(10 * 1000);
  }
  EXPECT_GT(config_version, leader.palf_handle_impl_->config_mgr_.persistent_config_version_);
  EXPECT_GT(config_version, palf_list[1]->palf_handle_impl_->config_mgr_.persistent_config_version_);

  // restart cluster, close a follower, restart leader
  revert_cluster_palf_handle_guard(palf_list);
  leader.reset();
  // block_net, so two F cann't reach majority
  block_net(another_f_idx, leader_idx);
  restart_paxos_groups();

  const int64_t restart_finish_time_us_ = common::ObTimeUtility::current_time();
  PalfHandleImplGuard new_leader;
  int64_t new_leader_idx;
  get_leader(id, new_leader, new_leader_idx);
  EXPECT_EQ(OB_SUCCESS, submit_log(new_leader, 500, id));
  PALF_LOG(ERROR, "RTO", "RTO", common::ObTimeUtility::current_time() - restart_finish_time_us_);

  new_leader.reset();
  // must delete paxos group in here, otherwise memory of
  // MockLocCB will be relcaimed and core dump will occur
  // blacklist will not be deleted after reboot, clean it manually
  unblock_net(another_f_idx, leader_idx);
  delete_paxos_group(id);
  PALF_LOG(INFO, "end test_2f1a_arb_with_highest_version", K(id));
}

TEST_F(TestObSimpleLogClusterArbService, test_2f1a_defensive)
{
  SET_CASE_LOG_FILE(TEST_NAME, "test_2f1a_defensive");
  OB_LOGGER.set_log_level("DEBUG");
  MockLocCB loc_cb;
  int ret = OB_SUCCESS;
  PALF_LOG(INFO, "begin test_2f1a_defensive");
	int64_t leader_idx = 0;
  int64_t arb_replica_idx = -1;
  PalfHandleImplGuard leader;
  std::vector<PalfHandleImplGuard*> palf_list;
  const int64_t CONFIG_CHANGE_TIMEOUT = 10 * 1000 * 1000L; // 10s
	const int64_t id = ATOMIC_AAF(&palf_id_, 1);
  common::ObMember dummy_member;
	EXPECT_EQ(OB_SUCCESS, create_paxos_group_with_arb(id, arb_replica_idx, leader_idx, leader));
  EXPECT_EQ(OB_SUCCESS, get_cluster_palf_handle_guard(id, palf_list));
  // 为备副本设置location cb，用于备副本找leader
  const int64_t another_f_idx = (leader_idx+1)%3;
  loc_cb.leader_ = leader.palf_handle_impl_->self_;
  palf_list[another_f_idx]->get_palf_handle_impl()->set_location_cache_cb(&loc_cb);
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 100, id));
  sleep(2);
  const int64_t added_member_idx = 3;
  const common::ObMember added_member = ObMember(palf_list[added_member_idx]->palf_handle_impl_->self_, 1);

  // add a member, do not allow to append logs until config log reaches majority
  LogConfigChangeArgs args(added_member, 3, ADD_MEMBER);
  const int64_t proposal_id = leader.palf_handle_impl_->state_mgr_.get_proposal_id();
  const int64_t election_epoch = leader.palf_handle_impl_->state_mgr_.get_leader_epoch();
  LogConfigVersion config_version;
  EXPECT_EQ(OB_EAGAIN, leader.palf_handle_impl_->config_mgr_.change_config(args, proposal_id, election_epoch, config_version));
  // do not allow to append log when changing config with arb
  EXPECT_TRUE(leader.palf_handle_impl_->state_mgr_.is_changing_config_with_arb());
  while (true) {
    if (OB_SUCC(leader.palf_handle_impl_->config_mgr_.change_config(args, proposal_id, election_epoch, config_version))) {
      break;
    } else {
      (void) leader.palf_handle_impl_->config_mgr_.pre_sync_config_log_and_mode_meta(args.server_, proposal_id);
      ::ob_usleep(10 * 1000);
    }
  }

  // flashback one follower
  LogEntryHeader header_origin;
  SCN base_scn;
  base_scn.set_base();
  SCN flashback_scn;
  palf::AccessMode unused_access_mode;
  int64_t mode_version;
  EXPECT_EQ(OB_SUCCESS, get_middle_scn(50, leader, flashback_scn, header_origin));
  switch_append_to_flashback(leader, mode_version);
  EXPECT_EQ(OB_SUCCESS, palf_list[another_f_idx]->palf_handle_impl_->flashback(mode_version, flashback_scn, CONFIG_CHANGE_TIMEOUT));

  // remove another follower
  EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->remove_member(added_member, 2, CONFIG_CHANGE_TIMEOUT));

  revert_cluster_palf_handle_guard(palf_list);
  leader.reset();
  delete_paxos_group(id);
  PALF_LOG(INFO, "end test_2f1a_defensive", K(id));
}


} // end unittest
} // end oceanbase

int main(int argc, char **argv)
{
  RUN_SIMPLE_LOG_CLUSTER_TEST(TEST_NAME);
}
