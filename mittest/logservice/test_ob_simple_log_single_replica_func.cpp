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

#include "lib/ob_define.h"
#include "lib/ob_errno.h"
#include <cstdio>
#include <gtest/gtest.h>
#include <signal.h>
#include <stdexcept>
#define private public
#define protected public
#include "env/ob_simple_log_cluster_env.h"
#undef private
#undef protected
#include "logservice/palf/log_reader_utils.h"
#include "logservice/palf/log_define.h"
#include "logservice/palf/log_group_entry_header.h"
#include "logservice/palf/log_io_worker.h"
#include "logservice/palf/lsn.h"

const std::string TEST_NAME = "single_replica";

using namespace oceanbase::common;
using namespace oceanbase;
namespace oceanbase
{
using namespace logservice;
namespace unittest
{
class TestObSimpleLogClusterSingleReplica : public ObSimpleLogClusterTestEnv
{
public:
  TestObSimpleLogClusterSingleReplica() : ObSimpleLogClusterTestEnv()
  {
    int ret = init();
    if (OB_SUCCESS != ret) {
      throw std::runtime_error("TestObSimpleLogClusterLogEngine init failed");
    }
  }
  ~TestObSimpleLogClusterSingleReplica()
  {
    destroy();
  }
  int init()
  {
    return OB_SUCCESS;
  }
  void destroy()
  {}
  int64_t id_;
  PalfHandleImplGuard leader_;
};

int64_t ObSimpleLogClusterTestBase::member_cnt_ = 1;
int64_t ObSimpleLogClusterTestBase::node_cnt_ = 1;
std::string ObSimpleLogClusterTestBase::test_name_ = TEST_NAME;
bool ObSimpleLogClusterTestBase::need_add_arb_server_  = false;
constexpr int64_t timeout_ts_us = 3 * 1000 * 1000;

TEST_F(TestObSimpleLogClusterSingleReplica, update_disk_options)
{
  SET_CASE_LOG_FILE(TEST_NAME, "update_disk_options");
  OB_LOGGER.set_log_level("TRACE");
  const int64_t id = ATOMIC_AAF(&palf_id_, 1);
  PALF_LOG(INFO, "start update_disk_options", K(id));
  int64_t leader_idx = 0;
  PalfHandleImplGuard leader;
  PalfEnv *palf_env = NULL;
  EXPECT_EQ(OB_SUCCESS, create_paxos_group(id, leader_idx, leader));
  EXPECT_EQ(OB_SUCCESS, get_palf_env(leader_idx, palf_env));
  PalfOptions opts;
  EXPECT_EQ(OB_SUCCESS, palf_env->get_options(opts));
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 4 * 32 + 10, id, MAX_LOG_BODY_SIZE));
  while (leader.palf_handle_impl_->log_engine_.log_storage_.log_tail_
         < LSN(4 * 32 * MAX_LOG_BODY_SIZE)) {
    sleep(1);
  }
  block_id_t min_block_id, max_block_id;
  EXPECT_EQ(OB_SUCCESS,
            leader.palf_handle_impl_->log_engine_.get_block_id_range(
                min_block_id, max_block_id));
  EXPECT_EQ(4, max_block_id);
  opts.disk_options_.log_disk_usage_limit_size_ = 8 * PALF_PHY_BLOCK_SIZE;
  EXPECT_EQ(OB_SUCCESS, palf_env->update_options(opts));
  sleep(1);
  opts.disk_options_.log_disk_utilization_limit_threshold_ = 50;
  opts.disk_options_.log_disk_utilization_threshold_ = 40;
  EXPECT_EQ(OB_SUCCESS, palf_env->update_options(opts));

  opts.disk_options_.log_disk_usage_limit_size_ = 4 * PALF_PHY_BLOCK_SIZE;
  EXPECT_EQ(OB_STATE_NOT_MATCH, palf_env->update_options(opts));

  palf_env->palf_env_impl_.disk_options_wrapper_.disk_opts_for_recycling_blocks_ = palf_env->palf_env_impl_.disk_options_wrapper_.disk_opts_for_stopping_writing_;
  palf_env->palf_env_impl_.disk_options_wrapper_.status_ = PalfDiskOptionsWrapper::Status::NORMAL_STATUS;

  opts.disk_options_.log_disk_utilization_limit_threshold_ = 95;
  opts.disk_options_.log_disk_utilization_threshold_ = 80;
  EXPECT_EQ(OB_SUCCESS, palf_env->update_options(opts));
  EXPECT_EQ(OB_STATE_NOT_MATCH, palf_env->update_options(opts));
  sleep(1);
  EXPECT_EQ(PalfDiskOptionsWrapper::Status::SHRINKING_STATUS,
            palf_env->palf_env_impl_.disk_options_wrapper_.status_);
  EXPECT_GT(palf_env->palf_env_impl_.disk_options_wrapper_.disk_opts_for_stopping_writing_.log_disk_usage_limit_size_,
            palf_env->palf_env_impl_.disk_options_wrapper_.disk_opts_for_recycling_blocks_.log_disk_usage_limit_size_);
  EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->set_base_lsn(LSN(4 * PALF_BLOCK_SIZE)));
  //EXPECT_EQ(LSN(4*PALF_BLOCK_SIZE), leader.palf_handle_.palf_handle_impl_->log_engine_.get_log_meta().get_log_snapshot_meta().base_lsn_);
  while (leader.palf_handle_impl_->log_engine_.base_lsn_for_block_gc_
         != LSN(4 * PALF_BLOCK_SIZE)) {
    sleep(1);
  }
  // wait blocks to be recycled
  while (leader.palf_handle_impl_->log_engine_.get_base_lsn_used_for_block_gc() != (LSN(4*PALF_BLOCK_SIZE))) {
    sleep(1);
  }
  sleep(1);
  EXPECT_EQ(PalfDiskOptionsWrapper::Status::NORMAL_STATUS,
            palf_env->palf_env_impl_.disk_options_wrapper_.status_);
  EXPECT_EQ(
      opts.disk_options_,
      palf_env->palf_env_impl_.disk_options_wrapper_.disk_opts_for_recycling_blocks_);
  EXPECT_EQ(
      opts.disk_options_,
      palf_env->palf_env_impl_.disk_options_wrapper_.disk_opts_for_stopping_writing_);
  PALF_LOG(INFO, "runlin trace", K(opts),
           "holders:", palf_env->palf_env_impl_.disk_options_wrapper_);
  // test expand
  opts.disk_options_.log_disk_usage_limit_size_ = 5 * 1024 * 1024 * 1024ul;
  EXPECT_EQ(OB_SUCCESS, palf_env->update_options(opts));
  opts.disk_options_.log_disk_usage_limit_size_ = 6 * 1024 * 1024 * 1024ul;
  EXPECT_EQ(OB_SUCCESS, palf_env->update_options(opts));
  EXPECT_EQ(
      opts.disk_options_,
      palf_env->palf_env_impl_.disk_options_wrapper_.disk_opts_for_recycling_blocks_);
  EXPECT_EQ(
      opts.disk_options_,
      palf_env->palf_env_impl_.disk_options_wrapper_.disk_opts_for_stopping_writing_);
}

TEST_F(TestObSimpleLogClusterSingleReplica, delete_paxos_group)
{
  SET_CASE_LOG_FILE(TEST_NAME, "delete_paxos_group");
  const int64_t id = ATOMIC_AAF(&palf_id_, 1);
  PALF_LOG(INFO, "start test delete_paxos_group", K(id));
  int64_t leader_idx = 0;
  {
    unittest::PalfHandleImplGuard leader;
    EXPECT_EQ(OB_SUCCESS, create_paxos_group(id, leader_idx, leader));
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, 100, leader_idx));
  }
  sleep(1);
  // EXPECT_EQ(OB_SUCCESS, delete_paxos_group(id));
  // TODO by yunlong: check log sync
  PALF_LOG(INFO, "end test delete_paxos_group", K(id));
}

TEST_F(TestObSimpleLogClusterSingleReplica, advance_base_lsn)
{
  SET_CASE_LOG_FILE(TEST_NAME, "advance_base_lsn");
  OB_LOGGER.set_log_level("INFO");
  const int64_t id = ATOMIC_AAF(&palf_id_, 1);
  PALF_LOG(INFO, "start advance_base_lsn", K(id));
  int64_t leader_idx = 0;
  int64_t log_ts = 1;
  {
    PalfHandleImplGuard leader;
    EXPECT_EQ(OB_SUCCESS, create_paxos_group(id, leader_idx, leader));
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, 100, id));
    sleep(2);
    LSN log_tail =
        leader.palf_handle_impl_->log_engine_.log_meta_storage_.log_tail_;
    for (int64_t i = 0; i < 4096; i++) {
      EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->enable_vote());
    }
    while (LSN(4096 * 4096 + log_tail.val_) !=
        leader.palf_handle_impl_->log_engine_.log_meta_storage_.log_tail_)
    {
      sleep(1);
    }
  }
  EXPECT_EQ(OB_SUCCESS, restart_paxos_groups());
  {
    PalfHandleImplGuard leader;
    EXPECT_EQ(OB_SUCCESS, get_leader(id, leader, leader_idx));
    EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->set_base_lsn(LSN(0)));
  }
}

TEST_F(TestObSimpleLogClusterSingleReplica, single_replica_flashback)
{
  SET_CASE_LOG_FILE(TEST_NAME, "single_replica_flashback");
  OB_LOGGER.set_log_level("INFO");
  const int64_t id = ATOMIC_AAF(&palf_id_, 1);
  int64_t leader_idx = 0;
  PALF_LOG(INFO, "start test single replica flashback", K(id));
  SCN max_scn;
  unittest::PalfHandleImplGuard leader;
  int64_t mode_version = INVALID_PROPOSAL_ID;
  SCN ref_scn;
  EXPECT_EQ(OB_SUCCESS, create_paxos_group(id, leader_idx, leader));
  {
    SCN tmp_scn;
    LSN tmp_lsn;
    // 提交1条日志后进行flashback
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, 1, leader_idx, 100));
    EXPECT_EQ(OB_SUCCESS, wait_until_has_committed(leader, leader.palf_handle_impl_->sw_.get_max_lsn()));
    tmp_scn = leader.palf_handle_impl_->get_max_scn();
    switch_append_to_flashback(leader, mode_version);
    EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->flashback(mode_version, SCN::minus(tmp_scn, 10), timeout_ts_us));
    // 预期日志起点为LSN(0)
    EXPECT_EQ(LSN(0), leader.palf_handle_impl_->get_max_lsn());
    EXPECT_EQ(SCN::minus(tmp_scn, 10), leader.palf_handle_impl_->get_max_scn());
    EXPECT_EQ(LSN(0), leader.palf_handle_impl_->log_engine_.log_storage_.log_tail_);

    // flashback到PADDING日志
    switch_flashback_to_append(leader, mode_version);
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, 31, leader_idx, MAX_LOG_BODY_SIZE));
    EXPECT_EQ(OB_SUCCESS, wait_until_has_committed(leader, leader.palf_handle_impl_->sw_.get_max_lsn()));
    EXPECT_EQ(OB_ITER_END, read_log(leader));
    EXPECT_GT(LSN(PALF_BLOCK_SIZE), leader.palf_handle_impl_->sw_.get_max_lsn());
    int remained_log_size = LSN(PALF_BLOCK_SIZE) - leader.palf_handle_impl_->sw_.get_max_lsn();
    EXPECT_LT(remained_log_size, MAX_LOG_BODY_SIZE);
    int need_log_size = remained_log_size - 5*1024;
    PALF_LOG(INFO, "runlin trace print sw1", K(leader.palf_handle_impl_->sw_));
    // 保证末尾只剩小于1KB的空间
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, 1, leader_idx, need_log_size));
    PALF_LOG(INFO, "runlin trace print sw2", K(leader.palf_handle_impl_->sw_));
    SCN mid_scn;
    LogEntryHeader header;
    // 此时一共存在32条日志
    EXPECT_EQ(OB_SUCCESS, get_middle_scn(32, leader, mid_scn, header));
    EXPECT_EQ(OB_ITER_END, get_middle_scn(33, leader, mid_scn, header));
    EXPECT_GT(LSN(PALF_BLOCK_SIZE), leader.palf_handle_impl_->sw_.get_max_lsn());
    remained_log_size = LSN(PALF_BLOCK_SIZE) - leader.palf_handle_impl_->sw_.get_max_lsn();
    EXPECT_LT(remained_log_size, 5*1024);
    EXPECT_GT(remained_log_size, 0);
    // 写一条大小为2KB的日志
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, 1, leader_idx, 5*1024));
    PALF_LOG(INFO, "runlin trace print sw3", K(leader.palf_handle_impl_->sw_));
    // Padding日志预期不占用日志条数，因此存在33条日志
    EXPECT_EQ(OB_SUCCESS, get_middle_scn(33, leader, mid_scn, header));
    EXPECT_LT(LSN(PALF_BLOCK_SIZE), leader.palf_handle_impl_->sw_.get_max_lsn());
    max_scn = leader.palf_handle_impl_->sw_.get_max_scn();
    EXPECT_EQ(OB_SUCCESS, wait_until_has_committed(leader, leader.palf_handle_impl_->get_max_lsn()));
    switch_append_to_flashback(leader, mode_version);
    // flashback到padding日志尾部
    tmp_scn = leader.palf_handle_impl_->get_max_scn();
    EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->flashback(mode_version, SCN::minus(tmp_scn, 1), timeout_ts_us));
    PALF_LOG(INFO, "flashback to padding tail");
    EXPECT_EQ(leader.palf_handle_impl_->get_max_lsn(), LSN(PALF_BLOCK_SIZE));
    EXPECT_EQ(OB_ITER_END, read_log(leader));
    // flashback后存在32条日志
    EXPECT_EQ(OB_SUCCESS, get_middle_scn(32, leader, mid_scn, header));
    EXPECT_EQ(OB_ITER_END, get_middle_scn(33, leader, mid_scn, header));

    // flashback到padding日志头部
    tmp_scn = leader.palf_handle_impl_->get_max_scn();
    EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->flashback(mode_version, SCN::minus(tmp_scn, 1), timeout_ts_us));
    EXPECT_LT(leader.palf_handle_impl_->get_max_lsn(), LSN(PALF_BLOCK_SIZE));
    EXPECT_EQ(OB_SUCCESS, get_middle_scn(32, leader, mid_scn, header));
    EXPECT_EQ(OB_ITER_END, get_middle_scn(33, leader, mid_scn, header));
    switch_flashback_to_append(leader, mode_version);
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, 10, leader_idx, 1000));
    EXPECT_EQ(OB_SUCCESS, wait_lsn_until_flushed(LSN(PALF_BLOCK_SIZE), leader));
    EXPECT_EQ(OB_ITER_END, read_log(leader));
    switch_append_to_flashback(leader, mode_version);
    EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->flashback(mode_version, SCN::min_scn(), timeout_ts_us));
    EXPECT_EQ(LSN(0), leader.palf_handle_impl_->get_max_lsn());
    switch_flashback_to_append(leader, mode_version);

    ref_scn.convert_for_tx(10000);
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, ref_scn, tmp_lsn, tmp_scn));
    LSN tmp_lsn1 = leader.palf_handle_impl_->get_max_lsn();
    ref_scn.convert_for_tx(50000);
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, ref_scn, tmp_lsn, tmp_scn));
    sleep(1);
    wait_until_has_committed(leader, leader.palf_handle_impl_->sw_.get_max_lsn());
    switch_append_to_flashback(leader, mode_version);
    ref_scn.convert_for_tx(30000);
    EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->flashback(mode_version, ref_scn, timeout_ts_us));
    // 验证重复的flashback任务
    EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->inner_flashback(ref_scn));
    EXPECT_EQ(tmp_lsn1, leader.palf_handle_impl_->log_engine_.log_storage_.log_tail_);
    // 验证flashback时间戳比过小
    ref_scn.convert_from_ts(1);
    EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->inner_flashback(ref_scn));
    EXPECT_GT(tmp_lsn1, leader.palf_handle_impl_->log_engine_.log_storage_.log_tail_);
    CLOG_LOG(INFO, "runlin trace 3");
  }
  switch_flashback_to_append(leader, mode_version);
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 300, leader_idx));
  wait_until_has_committed(leader, leader.palf_handle_impl_->sw_.get_max_lsn());
  EXPECT_EQ(OB_ITER_END, read_log(leader));

  // flashback到中间某条日志
	// 1. 比较log_storage和日位点和滑动窗口是否相同

  switch_append_to_flashback(leader, mode_version);
  LogEntryHeader header_origin;
	EXPECT_EQ(OB_SUCCESS, get_middle_scn(200, leader, max_scn, header_origin));
  EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->flashback(mode_version, max_scn, timeout_ts_us));
  LogEntryHeader header_new;
  SCN new_scn;
	EXPECT_EQ(OB_SUCCESS, get_middle_scn(200, leader, new_scn, header_new));
  EXPECT_EQ(new_scn, max_scn);
  EXPECT_EQ(header_origin.data_checksum_, header_origin.data_checksum_);
  switch_flashback_to_append(leader, mode_version);
  LSN new_log_tail = leader.palf_handle_impl_->log_engine_.log_storage_.log_tail_;
  EXPECT_EQ(new_log_tail, leader.palf_handle_impl_->sw_.committed_end_lsn_);
  EXPECT_EQ(max_scn, leader.palf_handle_impl_->sw_.last_slide_scn_);
  EXPECT_EQ(OB_ITER_END, read_log(leader));
	// 验证flashback后能否继续提交日志
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 500, leader_idx));
  wait_until_has_committed(leader, leader.palf_handle_impl_->sw_.get_max_lsn());
  EXPECT_EQ(OB_ITER_END, read_log(leader));

  // 再次执行flashback到上一次的flashback位点
  switch_append_to_flashback(leader, mode_version);
  EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->flashback(mode_version, max_scn, timeout_ts_us));
  switch_flashback_to_append(leader, mode_version);
  EXPECT_EQ(new_log_tail, leader.palf_handle_impl_->sw_.committed_end_lsn_);
  EXPECT_EQ(max_scn, leader.palf_handle_impl_->sw_.last_slide_scn_);
  EXPECT_EQ(OB_ITER_END, read_log(leader));
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 500, leader_idx));
  wait_until_has_committed(leader, leader.palf_handle_impl_->sw_.get_max_lsn());
  EXPECT_EQ(OB_ITER_END, read_log(leader));

  // 再次执行flashback到上一次的flashback后提交日志的某个时间点
	EXPECT_EQ(OB_SUCCESS, get_middle_scn(634, leader, max_scn, header_origin));
  switch_append_to_flashback(leader, mode_version);
  EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->flashback(mode_version, max_scn, timeout_ts_us));
  switch_flashback_to_append(leader, mode_version);
  new_log_tail = leader.palf_handle_impl_->log_engine_.log_storage_.log_tail_;
  EXPECT_EQ(max_scn, leader.palf_handle_impl_->sw_.last_slide_scn_);
  EXPECT_EQ(OB_ITER_END, read_log(leader));
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 300, leader_idx));
  wait_until_has_committed(leader, leader.palf_handle_impl_->sw_.get_max_lsn());
  EXPECT_EQ(OB_ITER_END, read_log(leader));
  PALF_LOG(INFO, "flashback to middle success");

  // flashback到某个更大的时间点
  max_scn = leader.palf_handle_impl_->get_end_scn();
  new_log_tail = leader.palf_handle_impl_->log_engine_.log_storage_.log_tail_;
  switch_append_to_flashback(leader, mode_version);
  EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->flashback(mode_version, SCN::plus(max_scn, 1000000000000), timeout_ts_us));
  switch_flashback_to_append(leader, mode_version);
  new_log_tail = leader.palf_handle_impl_->log_engine_.log_storage_.log_tail_;
  EXPECT_EQ(new_log_tail.val_, leader.palf_handle_impl_->sw_.committed_end_lsn_.val_);
  EXPECT_EQ(max_scn, leader.palf_handle_impl_->sw_.last_slide_scn_);
  EXPECT_EQ(OB_ITER_END, read_log(leader));
  PALF_LOG(INFO, "flashback to greater success");

  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 300, leader_idx));
  wait_until_has_committed(leader, leader.palf_handle_impl_->sw_.get_max_lsn());
  new_log_tail = leader.palf_handle_impl_->get_max_lsn();
  max_scn = leader.palf_handle_impl_->get_max_scn();
  PALF_LOG(INFO, "runlin trace 3", K(new_log_tail), K(max_scn));
  switch_append_to_flashback(leader, mode_version);
  LSN new_log_tail_1 = leader.palf_handle_impl_->get_end_lsn();
  SCN max_scn1 = leader.palf_handle_impl_->get_end_scn();
  PALF_LOG(INFO, "runlin trace 4", K(new_log_tail), K(max_scn), K(new_log_tail_1), K(max_scn1));
  EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->flashback(mode_version, max_scn, timeout_ts_us));
  LSN log_tail_after_flashback = leader.palf_handle_impl_->get_end_lsn();
  SCN max_ts_after_flashback = leader.palf_handle_impl_->get_end_scn();
  PALF_LOG(INFO, "runlin trace 5", K(log_tail_after_flashback), K(max_ts_after_flashback));
  switch_flashback_to_append(leader, mode_version);
  EXPECT_EQ(new_log_tail, leader.palf_handle_impl_->sw_.committed_end_lsn_);
  EXPECT_EQ(OB_ITER_END, read_log(leader));
  PALF_LOG(INFO, "flashback to max_scn success");

  // 再次执行flashback到提交日志前的max_scn
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 300, leader_idx));
  wait_until_has_committed(leader, leader.palf_handle_impl_->sw_.get_max_lsn());
  LSN curr_lsn = leader.palf_handle_impl_->get_end_lsn();
  EXPECT_NE(curr_lsn, new_log_tail);
  EXPECT_EQ(OB_ITER_END, read_log(leader));
  switch_append_to_flashback(leader, mode_version);
  EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->flashback(mode_version, max_scn, timeout_ts_us));
  switch_flashback_to_append(leader, mode_version);
  EXPECT_EQ(new_log_tail, leader.palf_handle_impl_->get_end_lsn());
  EXPECT_EQ(OB_ITER_END, read_log(leader));

  // 数据全部清空
  wait_until_has_committed(leader, leader.palf_handle_impl_->sw_.get_max_lsn());
  switch_append_to_flashback(leader, mode_version);
  EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->flashback(mode_version, SCN::min_scn(), timeout_ts_us));
  EXPECT_EQ(LSN(0), leader.palf_handle_impl_->get_max_lsn());
  EXPECT_EQ(SCN::min_scn(), leader.palf_handle_impl_->get_max_scn());
  switch_flashback_to_append(leader, mode_version);
  EXPECT_EQ(OB_ITER_END, read_log(leader));
  PALF_LOG(INFO, "flashback to 0 success");
  leader.reset();
  delete_paxos_group(id);

}

TEST_F(TestObSimpleLogClusterSingleReplica, single_replica_flashback_restart)
{
  SET_CASE_LOG_FILE(TEST_NAME, "single_replica_flashback_restart");
  OB_LOGGER.set_log_level("INFO");
  const int64_t id = ATOMIC_AAF(&palf_id_, 1);
  int64_t leader_idx = 0;
  SCN max_scn = SCN::min_scn();
  SCN ref_scn;
  int64_t mode_version = INVALID_PROPOSAL_ID;
  {
    unittest::PalfHandleImplGuard leader;
    EXPECT_EQ(OB_SUCCESS, create_paxos_group(id, leader_idx, leader));
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, 1000, leader_idx));
    LogEntryHeader header_origin;
		EXPECT_EQ(OB_SUCCESS, wait_until_has_committed(leader, leader.palf_handle_impl_->get_max_lsn()));
		EXPECT_EQ(OB_SUCCESS, get_middle_scn(323, leader, max_scn, header_origin));
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, 100, leader_idx));
		wait_until_has_committed(leader, leader.palf_handle_impl_->sw_.get_max_lsn());
    EXPECT_EQ(OB_ITER_END, read_log(leader));
    switch_append_to_flashback(leader, mode_version);
    EXPECT_EQ(OB_SUCCESS, leader.palf_handle_impl_->flashback(mode_version, max_scn, timeout_ts_us));
    LogEntryHeader header_new;
    SCN new_scn;
		EXPECT_EQ(OB_SUCCESS, get_middle_scn(323, leader, new_scn, header_new));
    EXPECT_EQ(new_scn, max_scn);
    EXPECT_EQ(header_origin.data_checksum_, header_new.data_checksum_);
		EXPECT_EQ(OB_ITER_END, get_middle_scn(324, leader, new_scn, header_new));
    switch_flashback_to_append(leader, mode_version);
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, 1000, leader_idx));
		EXPECT_EQ(OB_SUCCESS, get_middle_scn(1323, leader, new_scn, header_new));
		EXPECT_EQ(OB_ITER_END, get_middle_scn(1324, leader, new_scn, header_new));
    EXPECT_EQ(OB_ITER_END, read_log(leader));
  }
  EXPECT_EQ(OB_SUCCESS, restart_paxos_groups());
  // 验证重启场景
  PalfHandleImplGuard new_leader;
  int64_t curr_mode_version = INVALID_PROPOSAL_ID;
  AccessMode curr_access_mode = AccessMode::INVALID_ACCESS_MODE;
  EXPECT_EQ(OB_SUCCESS, get_leader(id, new_leader, leader_idx));
  EXPECT_EQ(OB_SUCCESS, new_leader.palf_handle_impl_->get_access_mode(curr_mode_version, curr_access_mode));
  EXPECT_EQ(curr_mode_version, mode_version);
  EXPECT_EQ(OB_SUCCESS, submit_log(new_leader, 1000, leader_idx));
	wait_until_has_committed(new_leader, new_leader.palf_handle_impl_->sw_.get_max_lsn());
  EXPECT_EQ(OB_ITER_END, read_log(new_leader));
    ref_scn.convert_for_tx(1000);
  LogEntryHeader header_new;
	EXPECT_EQ(OB_SUCCESS, get_middle_scn(1329, new_leader, max_scn, header_new));
  switch_append_to_flashback(new_leader, mode_version);
  EXPECT_EQ(OB_SUCCESS, new_leader.palf_handle_impl_->flashback(mode_version, max_scn, timeout_ts_us));
  switch_flashback_to_append(new_leader, mode_version);
  PALF_LOG(INFO, "flashback after restart");
  EXPECT_EQ(OB_ITER_END, read_log(new_leader));
  EXPECT_EQ(OB_SUCCESS, submit_log(new_leader, 1000, leader_idx));
  sleep(1);
  EXPECT_EQ(OB_ITER_END, read_log(new_leader));
  new_leader.reset();
  delete_paxos_group(id);
}

TEST_F(TestObSimpleLogClusterSingleReplica, test_truncate_failed)
{
  SET_CASE_LOG_FILE(TEST_NAME, "test_truncate_failed");
  int64_t id = ATOMIC_AAF(&palf_id_, 1);
  int64_t leader_idx = 0;
  char block_path[OB_MAX_FILE_NAME_LENGTH] = {'\0'};
  int64_t file_size = 0;
  {
    PalfHandleImplGuard leader;
    EXPECT_EQ(OB_SUCCESS, create_paxos_group(id, leader_idx, leader));
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, 10, id, 1000));
    wait_lsn_until_flushed(leader.palf_handle_impl_->get_max_lsn(), leader);
    LSN max_lsn = leader.palf_handle_impl_->log_engine_.log_storage_.log_tail_;
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, 10, id, 1000));
    wait_lsn_until_flushed(leader.palf_handle_impl_->get_max_lsn(), leader);
    int64_t fd = leader.palf_handle_impl_->log_engine_.log_storage_.block_mgr_.curr_writable_handler_.io_fd_;
    block_id_t block_id = leader.palf_handle_impl_->log_engine_.log_storage_.block_mgr_.curr_writable_block_id_;
    char *log_dir = leader.palf_handle_impl_->log_engine_.log_storage_.block_mgr_.log_dir_;
    convert_to_normal_block(log_dir, block_id, block_path, OB_MAX_FILE_NAME_LENGTH);
    EXPECT_EQ(OB_ITER_END, read_log(leader));
    PALF_LOG_RET(ERROR, OB_SUCCESS, "truncate pos", K(max_lsn));
    EXPECT_EQ(0, ftruncate(fd, max_lsn.val_));
    FileDirectoryUtils::get_file_size(block_path, file_size);
    EXPECT_EQ(file_size, max_lsn.val_);
  }
  PalfHandleImplGuard leader;
  EXPECT_EQ(OB_SUCCESS, restart_paxos_groups());;
  FileDirectoryUtils::get_file_size(block_path, file_size);
  EXPECT_EQ(file_size, PALF_PHY_BLOCK_SIZE);
  get_leader(id, leader, leader_idx);
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 10, id, 1000));
  EXPECT_EQ(OB_ITER_END, read_log(leader));
}

TEST_F(TestObSimpleLogClusterSingleReplica, test_meta)
{
  SET_CASE_LOG_FILE(TEST_NAME, "test_meta");
  int64_t id = ATOMIC_AAF(&palf_id_, 1);
  int64_t leader_idx = 0;
  LSN upper_aligned_log_tail;
  {
    PalfHandleImplGuard leader;
    EXPECT_EQ(OB_SUCCESS, create_paxos_group(id, leader_idx, leader));
    sleep(1);
    // 测试meta文件刚好写满的重启场景
    LogEngine *log_engine = &leader.palf_handle_impl_->log_engine_;
    LogStorage *log_meta_storage = &log_engine->log_meta_storage_;
    LSN log_meta_tail = log_meta_storage->log_tail_;
    upper_aligned_log_tail.val_ = (lsn_2_block(log_meta_tail, PALF_META_BLOCK_SIZE) + 1) * PALF_META_BLOCK_SIZE;
    int64_t delta = upper_aligned_log_tail - log_meta_tail;
    int64_t delta_cnt = delta / MAX_META_ENTRY_SIZE;
    while (delta_cnt-- > 0) {
      log_engine->append_log_meta_(log_engine->log_meta_);
    }
    EXPECT_EQ(upper_aligned_log_tail, log_meta_storage->log_tail_);
    PALF_LOG_RET(ERROR, OB_SUCCESS, "runlin trace before restart", K(upper_aligned_log_tail), KPC(log_meta_storage));
  }

  EXPECT_EQ(OB_SUCCESS, restart_paxos_groups());

  {
    PalfHandleImplGuard leader;
    EXPECT_EQ(OB_SUCCESS, get_leader(id, leader, leader_idx));
    LogEngine *log_engine = &leader.palf_handle_impl_->log_engine_;
    LogStorage *log_meta_storage = &log_engine->log_meta_storage_;
    LSN log_meta_tail = log_meta_storage->log_tail_;
    upper_aligned_log_tail.val_ = (lsn_2_block(log_meta_tail, PALF_META_BLOCK_SIZE) + 1) * PALF_META_BLOCK_SIZE;
    int64_t delta = upper_aligned_log_tail - log_meta_tail;
    int64_t delta_cnt = delta / MAX_META_ENTRY_SIZE;
    while (delta_cnt-- > 0) {
      log_engine->append_log_meta_(log_engine->log_meta_);
    }
    EXPECT_EQ(upper_aligned_log_tail, log_meta_storage->log_tail_);
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, 32, id, MAX_LOG_BODY_SIZE));
    sleep(1);
    wait_lsn_until_flushed(leader.palf_handle_impl_->get_max_lsn(), leader);
    block_id_t min_block_id, max_block_id;
    EXPECT_EQ(OB_SUCCESS, log_meta_storage->get_block_id_range(min_block_id, max_block_id));
    EXPECT_EQ(min_block_id, max_block_id);
  }
}

TEST_F(TestObSimpleLogClusterSingleReplica, test_restart)
{
  SET_CASE_LOG_FILE(TEST_NAME, "test_restart");
  int64_t id = ATOMIC_AAF(&palf_id_, 1);
  int64_t leader_idx = 0;
  char meta_fd[OB_MAX_FILE_NAME_LENGTH] = {'\0'};
  char log_fd[OB_MAX_FILE_NAME_LENGTH] = {'\0'};
  ObServerLogBlockMgr *pool = NULL;
  {
    PalfHandleImplGuard leader;
    EXPECT_EQ(OB_SUCCESS, create_paxos_group(id, leader_idx, leader));
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, 1, id, MAX_LOG_BODY_SIZE));
    wait_lsn_until_flushed(leader.palf_handle_impl_->get_max_lsn(), leader);
    LogEngine *log_engine = &leader.palf_handle_impl_->log_engine_;
    char *meta_log_dir = log_engine->log_meta_storage_.block_mgr_.log_dir_;
    char *log_dir = log_engine->log_storage_.block_mgr_.log_dir_;
    EXPECT_EQ(OB_SUCCESS, get_log_pool(leader_idx, pool));
    char *pool_dir = pool->log_pool_path_;
    snprintf(meta_fd, OB_MAX_FILE_NAME_LENGTH, "mv %s/%d %s/%d", meta_log_dir, 0, pool_dir, 10000000);
    snprintf(log_fd, OB_MAX_FILE_NAME_LENGTH, "mv %s/%d %s/%d", log_dir, 0, pool_dir, 100000001);
    system(meta_fd);
  }
  OB_LOGGER.set_log_level("TRACE");
  sleep(1);
  EXPECT_EQ(OB_ERR_UNEXPECTED, restart_paxos_groups());
  system(log_fd);
  EXPECT_EQ(OB_SUCCESS, restart_paxos_groups());
  PalfHandleImplGuard leader;
  EXPECT_EQ(OB_SUCCESS, create_paxos_group(id, leader_idx, leader));
}

TEST_F(TestObSimpleLogClusterSingleReplica, test_iterator)
{
  SET_CASE_LOG_FILE(TEST_NAME, "test_iterator");
  OB_LOGGER.set_log_level("TRACE");
  const int64_t id = ATOMIC_AAF(&palf_id_, 1);
  int64_t leader_idx = 0;
  int64_t mode_version_v = 1;
  int64_t *mode_version = &mode_version_v;
  LSN end_lsn_v = LSN(100000000);
  LSN *end_lsn = &end_lsn_v;
  {
    SCN max_scn_case1, max_scn_case2, max_scn_case3;
    PalfHandleImplGuard leader;
    PalfHandleImplGuard raw_write_leader;
    EXPECT_EQ(OB_SUCCESS, create_paxos_group(id, leader_idx, leader));
    PalfHandleImpl *palf_handle_impl = leader.palf_handle_impl_;
    const int64_t id_raw_write = ATOMIC_AAF(&palf_id_, 1);
    EXPECT_EQ(OB_SUCCESS, create_paxos_group(id_raw_write, leader_idx, raw_write_leader));
    EXPECT_EQ(OB_SUCCESS, change_access_mode_to_raw_write(raw_write_leader));
    int64_t count = 5;
    // 提交1024条日志，记录max_scn，用于后续next迭代验证，case1
    for (int i = 0; i < count; i++) {
      EXPECT_EQ(OB_SUCCESS, submit_log(leader, 1, id, 4*1024));
      EXPECT_EQ(OB_SUCCESS, wait_lsn_until_flushed(leader.palf_handle_impl_->get_max_lsn(), leader));
    }
    max_scn_case1 = palf_handle_impl->get_max_scn();
    // 提交5条日志，case1成功后，执行case2
    for (int i = 0; i < count; i++) {
      EXPECT_EQ(OB_SUCCESS, submit_log(leader, 1, id, 4*1024));
      EXPECT_EQ(OB_SUCCESS, wait_lsn_until_flushed(leader.palf_handle_impl_->get_max_lsn(), leader));
    }
    max_scn_case2 = palf_handle_impl->get_max_scn();

    // 提交5条日志, case3, 验证next(replayable_point_scn, &next_log_min_scn, &bool)
    std::vector<LSN> lsns;
    std::vector<SCN> logts;
    const int64_t log_size = 500;
    auto submit_log_private =[this](PalfHandleImplGuard &leader,
                              const int64_t count,
                              const int64_t id,
                              const int64_t wanted_data_size,
                              std::vector<LSN> &lsn_array,
                              std::vector<SCN> &scn_array)-> int{
      int ret = OB_SUCCESS;
      lsn_array.resize(count);
      scn_array.resize(count);
      for (int i = 0; i < count && OB_SUCC(ret); i++) {
        SCN ref_scn;
        ref_scn.convert_from_ts(ObTimeUtility::current_time() + 10000000);
        std::vector<LSN> tmp_lsn_array;
        std::vector<SCN> tmp_log_scn_array;
        if (OB_FAIL(submit_log_impl(leader, 1, id, wanted_data_size, ref_scn, tmp_lsn_array, tmp_log_scn_array))) {
        } else {
          lsn_array[i] = tmp_lsn_array[0];
          scn_array[i] = tmp_log_scn_array[0];
          wait_lsn_until_flushed(leader.palf_handle_impl_->get_max_lsn(), leader);
          CLOG_LOG(INFO, "submit_log_private success", K(i), "scn", tmp_log_scn_array[0], K(ref_scn));
        }
      }
      return ret;
    };
    EXPECT_EQ(OB_SUCCESS, submit_log_private(leader, count, id, log_size, lsns, logts));

    max_scn_case3 = palf_handle_impl->get_max_scn();
    EXPECT_EQ(OB_SUCCESS, wait_until_has_committed(leader, palf_handle_impl->get_end_lsn()));
    EXPECT_EQ(OB_ITER_END, read_and_submit_group_log(leader, raw_write_leader));
    PalfHandleImpl *raw_write_palf_handle_impl = raw_write_leader.palf_handle_impl_;
    EXPECT_EQ(OB_SUCCESS, wait_until_has_committed(raw_write_leader, raw_write_palf_handle_impl->get_end_lsn()));


    PalfBufferIterator iterator;
    auto get_file_end_lsn = [&end_lsn]() -> LSN { return *end_lsn; };
    auto get_mode_version = [&mode_version, &mode_version_v]() -> int64_t {
      PALF_LOG(INFO, "runlin trace", K(*mode_version), K(mode_version_v));
      return *mode_version;
    };
    EXPECT_EQ(OB_SUCCESS,
        iterator.init(LSN(0), get_file_end_lsn, get_mode_version, &raw_write_palf_handle_impl->log_engine_.log_storage_));
    EXPECT_EQ(OB_SUCCESS, iterator.next(max_scn_case1)); count--;
    EXPECT_EQ(OB_ITER_END, iterator.next(SCN::base_scn()));

    // case0: 验证group iterator迭代日志功能
    EXPECT_EQ(OB_ITER_END, read_group_log(raw_write_leader, LSN(0)));

    LSN curr_lsn = iterator.get_curr_read_lsn();
    // case1:
    // - 验证mode_version变化后，cache是否清空
    // - replayable_point_scn是否生效
    // 当mode version发生变化时，预期case应该清空
    // raw模式下，当replayable_point_scn很小时，直接返回OB_ITER_END
    PALF_LOG(INFO, "runlin trace case1", K(mode_version_v), K(*mode_version), K(max_scn_case1));
    // mode_version_v 为无效值时，预期不清空
    mode_version_v = INVALID_PROPOSAL_ID;
    end_lsn_v = curr_lsn;
    EXPECT_FALSE(curr_lsn == iterator.iterator_storage_.end_lsn_);
    EXPECT_FALSE(curr_lsn == iterator.iterator_storage_.start_lsn_);
    EXPECT_EQ(OB_ITER_END, iterator.next(SCN::base_scn()));

    //  mode_version_v 比inital_mode_version小，预期不清空
    mode_version_v = -1;
    EXPECT_FALSE(curr_lsn == iterator.iterator_storage_.end_lsn_);
    EXPECT_FALSE(curr_lsn == iterator.iterator_storage_.start_lsn_);
    EXPECT_EQ(OB_ITER_END, iterator.next(SCN::base_scn()));

    // 合理的mode_version_v，清空cache
    mode_version_v = 100;
    end_lsn_v = curr_lsn;
    EXPECT_EQ(OB_ITER_END, iterator.next(SCN::base_scn()));
    // cache清空，依赖上一次next操作
    EXPECT_EQ(curr_lsn, iterator.iterator_storage_.start_lsn_);
    EXPECT_EQ(curr_lsn, iterator.iterator_storage_.end_lsn_);

    PALF_LOG(INFO, "runlin trace", K(iterator), K(max_scn_case1), K(curr_lsn));

    end_lsn_v = LSN(1000000000);
    // 当replayable_point_scn为max_log_ts，预期max_log_ts前的日志可以吐出5条日志
    EXPECT_EQ(OB_SUCCESS, iterator.next(max_scn_case1)); count--;
    while (count > 0) {
      EXPECT_EQ(OB_SUCCESS, iterator.next(max_scn_case1));
      count--;
    }
    EXPECT_EQ(OB_ITER_END, iterator.next(max_scn_case1));
    // case2: next 功能是否正常
    // 尝试读取后续的5条日志
    count = 5;

    PALF_LOG(INFO, "runlin trace case2", K(iterator), K(max_scn_case2));
    while (count > 0) {
      EXPECT_EQ(OB_SUCCESS, iterator.next(max_scn_case2));
      count--;
    }

    // 此时的curr_entry已经是第三次提交日志的第一条日志日志(first_log)
    // 由于该日志对应的时间戳比max_scn_case2大，因此不会吐出
    // NB: 这里测试时，遇到过以下情况:case3的第一次 next后的EXPECT_EQ：
    // curr_entry变为first_log后，在后续的测试中，尝试把file_end_lsn设置到
    // fisrt_log之前，然后出现了一种情况，此时调用next(fist_log_ts, next_log_min_scn)后,
    // next_log_min_scn被设置为first_scn+1，对外表现为：尽管存在first_log，但外部在
    // 没有看到first_log之前就已经next_log_min_scn一定大于first_scn
    //
    // 实际上，这种情况是不会出现的，因为file_end_lsn不会回退的
    EXPECT_EQ(OB_ITER_END, iterator.next(max_scn_case2));

    //case3: next(replayable_point_scn, &next_log_min_scn)
    PALF_LOG(INFO, "runlin trace case3", K(iterator), K(max_scn_case3), K(end_lsn_v), K(max_scn_case2));
    SCN first_scn = logts[0];
    // 在使用next(replayable_point_scn, &next_log_min_scn)接口时
    // 我们禁止使用LogEntry的头作为迭代器重点
    LSN first_log_start_lsn = lsns[0];
    LSN first_log_end_lsn = lsns[0]+log_size+sizeof(LogEntryHeader);
    SCN next_log_min_scn;
    bool iterate_end_by_replayable_point = false;
    count = 5;
    // 模拟提前达到文件终点, 此时curr_entry_size为0，因此next_log_min_scn为max_scn_case2+1
    end_lsn_v = first_log_start_lsn - 1;
    CLOG_LOG(INFO, "runlin trace 1", K(iterator), K(end_lsn_v), KPC(end_lsn), K(max_scn_case2), K(first_scn));
    EXPECT_EQ(OB_ITER_END, iterator.next(SCN::plus(first_scn, 10000), next_log_min_scn, iterate_end_by_replayable_point));
    // file_end_lsn尽管回退了，但curr_entry_已经没有被读取过, 因此next_log_min_scn依旧为first_scn
    EXPECT_EQ(SCN::plus(max_scn_case2, 1), next_log_min_scn);
    EXPECT_EQ(iterate_end_by_replayable_point, false);

    // 读取一条日志成功，next_log_min_scn会被重置
    // curr_entry为fisrt_log_ts对应的log
    end_lsn_v = first_log_end_lsn;
    CLOG_LOG(INFO, "runlin trace 2", K(iterator), K(end_lsn_v), KPC(end_lsn));
    EXPECT_EQ(OB_SUCCESS, iterator.next(first_scn, next_log_min_scn, iterate_end_by_replayable_point)); count--;
    // iterator 返回成功，next_log_min_scn应该为OB_INVALID_TIMESTAMP
    EXPECT_EQ(next_log_min_scn.is_valid(), false);

    CLOG_LOG(INFO, "runlin trace 3", K(iterator), K(end_lsn_v), KPC(end_lsn));
    {
      // 模拟提前达到文件终点, 此时文件终点为file_log_end_lsn预期next_log_min_scn为first_scn对应的日志+1
      SCN second_scn = logts[1];
      EXPECT_EQ(OB_ITER_END, iterator.next(second_scn, next_log_min_scn, iterate_end_by_replayable_point));
      // iterator返回OB_ITER_END，next_log_min_scn为first_scn+1
      EXPECT_EQ(next_log_min_scn, SCN::plus(first_scn, 1));
      EXPECT_EQ(iterate_end_by_replayable_point, false);
      CLOG_LOG(INFO, "runlin trace 3", K(iterator), K(end_lsn_v), KPC(end_lsn), K(first_scn), K(second_scn));
      // 再次调用next，预期next_log_min_scn依旧为first_scn+1
      EXPECT_EQ(OB_ITER_END, iterator.next(second_scn, next_log_min_scn, iterate_end_by_replayable_point));
      // iterator返回OB_ITER_END，next_log_min_scn为first_scn+1
      EXPECT_EQ(next_log_min_scn, SCN::plus(first_scn, 1));
    }

    CLOG_LOG(INFO, "runlin trace 4", K(iterator), K(end_lsn_v), KPC(end_lsn));
    SCN prev_next_success_scn;
    // 模拟到达replayable_point_scn，此时文件终点为second log, 预期next_log_min_scn为replayable_point_scn+1
    // 同时replayable_point_scn < 缓存的日志时间戳
    {
      SCN second_scn = logts[1];
      SCN replayable_point_scn = SCN::minus(second_scn, 1);
      end_lsn_v = lsns[1]+log_size+sizeof(LogEntryHeader);
      EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
      EXPECT_EQ(iterate_end_by_replayable_point, true);
      // iterator返回OB_ITER_END，next_log_min_scn为replayable_point_scn + 1
      EXPECT_EQ(next_log_min_scn, SCN::plus(replayable_point_scn, 1));
      // 再次调用next，预期next_log_min_scn还是replayable_point_scn+1
      EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
      // iterator返回OB_ITER_END，next_log_min_scn为replayable_point_scn+1
      EXPECT_EQ(next_log_min_scn, SCN::plus(replayable_point_scn, 1));
      EXPECT_EQ(iterate_end_by_replayable_point, true);
      EXPECT_EQ(OB_SUCCESS, iterator.next(second_scn, next_log_min_scn, iterate_end_by_replayable_point)); count--;
      EXPECT_EQ(next_log_min_scn.is_valid(), false);
      prev_next_success_scn = iterator.iterator_impl_.prev_entry_scn_;
      EXPECT_EQ(prev_next_success_scn, second_scn);
    }

    // 模拟file end lsn不是group entry的终点
    {

      // 设置终点为第三条日志LogEntry对应的起点
      end_lsn_v = lsns[2];
      SCN third_scn = logts[2];
      SCN replayable_point_scn = third_scn.minus(third_scn, 1);
      CLOG_LOG(INFO, "runlin trace 5.1", K(iterator), K(end_lsn_v), KPC(end_lsn), K(replayable_point_scn));
      // 此时内存中缓存的日志为第三条日志, 但该日志由于end_lsn的原因不可读，
      // 因此next_log_min_scn为replayable_point_scn+1
      EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
      EXPECT_EQ(next_log_min_scn, SCN::plus(replayable_point_scn, 1));
      EXPECT_EQ(iterate_end_by_replayable_point, false);
      // 由于replayable_point_scn与curr_entry_之间不可能有日志，同时replayable_point_scn<curr_entry_，
      // 因此prev_entry_scn_会推到到replayable_point_scn
      prev_next_success_scn = replayable_point_scn;
      EXPECT_EQ(replayable_point_scn, iterator.iterator_impl_.prev_entry_scn_);

      CLOG_LOG(INFO, "runlin trace 5.2", K(iterator), K(end_lsn_v), KPC(end_lsn));

      // 将replayable_point_scn变小，但由于在case4的最后一步迭代日成功，因此next_log_min_scn为
      // prev_next_success_scn + 1
      replayable_point_scn = SCN::minus(replayable_point_scn, 2);
      EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
      EXPECT_EQ(SCN::plus(prev_next_success_scn, 1), next_log_min_scn);
      EXPECT_EQ(SCN::plus(replayable_point_scn, 2), iterator.iterator_impl_.prev_entry_scn_);
      EXPECT_EQ(iterate_end_by_replayable_point, false);
    }

    end_lsn_v = LSN(1000000000);
    while (count > 0) {
      EXPECT_EQ(OB_SUCCESS, iterator.next(max_scn_case3, next_log_min_scn, iterate_end_by_replayable_point));
      prev_next_success_scn = iterator.iterator_impl_.prev_entry_scn_;
      EXPECT_EQ(false, next_log_min_scn.is_valid());
      count--;
    }
    CLOG_LOG(INFO, "runlin trace 6.1", K(iterator), K(end_lsn_v), K(max_scn_case3));
    // 磁盘上以及受控回放点之后没有可读日志，此时应该返回受控回放点+1
    EXPECT_EQ(OB_ITER_END, iterator.next(max_scn_case3, next_log_min_scn, iterate_end_by_replayable_point));
    EXPECT_EQ(SCN::plus(max_scn_case3, 1), next_log_min_scn);
    EXPECT_EQ(max_scn_case3, prev_next_success_scn);
    CLOG_LOG(INFO, "runlin trace 6.2", K(iterator), K(end_lsn_v), K(max_scn_case3), "end_lsn_of_leader",
        raw_write_leader.palf_handle_impl_->get_max_lsn());

    // raw write 变为 Append后，在写入一些日志
    // 测试raw write变apend后，迭代日志是否正常
    {
      std::vector<SCN> logts_append;
      std::vector<LSN> lsns_append;
      int count_append = 5;

      EXPECT_EQ(OB_SUCCESS, change_access_mode_to_append(raw_write_leader));
      PALF_LOG(INFO, "runlin trace 6.3", "raw_write_leader_lsn", raw_write_leader.palf_handle_impl_->get_max_lsn(),
          "new_leader_lsn", leader.palf_handle_impl_->get_max_lsn());
      EXPECT_EQ(OB_SUCCESS, submit_log_private(leader, count_append, id, log_size, lsns_append, logts_append));
      EXPECT_EQ(OB_SUCCESS, submit_log_private(raw_write_leader, count_append, id, log_size, lsns_append, logts_append));
      EXPECT_EQ(OB_SUCCESS, wait_lsn_until_flushed(leader.palf_handle_impl_->get_max_lsn(), leader));
      EXPECT_EQ(OB_SUCCESS, wait_lsn_until_flushed(raw_write_leader.palf_handle_impl_->get_max_lsn(), raw_write_leader));
      PALF_LOG(INFO, "runlin trace 6.4", "raw_write_leader_lsn", raw_write_leader.palf_handle_impl_->get_max_lsn(),
          "new_leader_lsn", leader.palf_handle_impl_->get_max_lsn());

      // case 7 end_lsn_v 为很大的之后，让内存中有2M数据, 预期iterator next会由于受控回放失败，prev_entry_scn_不变
      // replayable_point_scn 为第一条日志的时间戳-2, next_log_min_scn 为append第一条LogEntry的时间戳
      // NB: 如果不将数据读到内存中来，可能会出现读数据报错OB_NEED_RETRY的问题。
      end_lsn_v = LSN(1000000000);
      SCN replayable_point_scn = SCN::minus(logts_append[0], 2);
      EXPECT_EQ(OB_SUCCESS, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point)); count_append--;
      prev_next_success_scn = iterator.iterator_impl_.prev_entry_scn_;

      end_lsn_v = lsns_append[1]+2;

      // 此时curr_entry_为第二条日志, curr_entry有效但不可读
      // 模拟replayable_point_scn大于curr_entry_
      PALF_LOG(INFO, "runlin trace 7.1", K(iterator), K(replayable_point_scn), K(end_lsn_v), K(logts_append[1]));
      replayable_point_scn.convert_from_ts(ObTimeUtility::current_time() + 100000000);
      EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
      EXPECT_EQ(next_log_min_scn, logts_append[1]);
      EXPECT_EQ(prev_next_success_scn, iterator.iterator_impl_.prev_entry_scn_);
      EXPECT_EQ(iterate_end_by_replayable_point, false);

      PALF_LOG(INFO, "runlin trace 7.1.1", K(iterator), K(replayable_point_scn), K(end_lsn_v), K(logts_append[1]));
      EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
      EXPECT_EQ(next_log_min_scn, logts_append[1]);
      EXPECT_EQ(prev_next_success_scn, iterator.iterator_impl_.prev_entry_scn_);

      // replayable_point_scn回退是一个不可能出现的情况, 但从iterator视角不能依赖这个
      // 验证replayable_point_scn回退到一个很小的值，预期next_log_min_scn为prev_next_success_scn+1
      // 模拟replayable_point_scn小于prev_entry_
      replayable_point_scn.convert_for_tx(100);
      PALF_LOG(INFO, "runlin trace 7.2", K(iterator), K(replayable_point_scn), K(end_lsn_v), K(logts_append[0]));
      EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
      EXPECT_EQ(SCN::plus(prev_next_success_scn, 1), next_log_min_scn);
      EXPECT_EQ(prev_next_success_scn, iterator.iterator_impl_.prev_entry_scn_);
      EXPECT_EQ(iterate_end_by_replayable_point, false);

      // 在迭代一次，结果一样
      EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
      EXPECT_EQ(SCN::plus(prev_next_success_scn, 1), next_log_min_scn);
      EXPECT_EQ(prev_next_success_scn, iterator.iterator_impl_.prev_entry_scn_);

      // 验证replayable_point_scn的值为prev_next_success_scn和第二条append的日志之间，
      // 预期next_log_min_scn为replayable_point_scn+1
      // 模拟replayable_point_scn位于[prev_entry_, curr_entry_]
      replayable_point_scn = SCN::minus(logts_append[1], 4);
      PALF_LOG(INFO, "runlin trace 7.3", K(iterator), K(replayable_point_scn), K(end_lsn_v),
          K(logts_append[1]), K(prev_next_success_scn));
      EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
      EXPECT_EQ(next_log_min_scn, SCN::plus(replayable_point_scn, 1));
      // 由于replayable_point_scn到curr_entry_之间没有日志，因此prev_entry_scn_会被推到replayable_point_scn
      EXPECT_EQ(replayable_point_scn, iterator.iterator_impl_.prev_entry_scn_);

      // 在迭代一次
      EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
      EXPECT_EQ(next_log_min_scn, SCN::plus(replayable_point_scn, 1));
      // 由于replayable_point_scn到curr_entry_之间没有日志，因此prev_entry_scn_会被推到replayable_point_scn
      EXPECT_EQ(replayable_point_scn, iterator.iterator_impl_.prev_entry_scn_);

      // 验证迭代append日志成功,
      end_lsn_v = lsns_append[2]+2;
      replayable_point_scn = logts_append[0];
      EXPECT_EQ(OB_SUCCESS, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
      EXPECT_EQ(false, next_log_min_scn.is_valid());
      EXPECT_EQ(logts_append[1], iterator.iterator_impl_.prev_entry_scn_); count_append--;
      prev_next_success_scn = logts_append[1];

      // replayable_point_scn比较大，预期next_log_min_scn为logts_append[2]
      replayable_point_scn.convert_from_ts(ObTimeUtility::current_time() + 100000000);
      PALF_LOG(INFO, "runlin trace 7.4", K(iterator), K(replayable_point_scn), K(end_lsn_v),
          K(logts_append[2]), K(prev_next_success_scn));
      EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
      EXPECT_EQ(next_log_min_scn, logts_append[2]);
      // 在迭代一次，结果一样
      EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
      EXPECT_EQ(next_log_min_scn, logts_append[2]);
      EXPECT_EQ(iterate_end_by_replayable_point, false);

      // 回退replayable_point_scn，预期next_log_min_scn为prev_next_success_scn+1
      replayable_point_scn.convert_for_tx(100);
      EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
      EXPECT_EQ(SCN::plus(prev_next_success_scn, 1), next_log_min_scn);
      EXPECT_EQ(iterate_end_by_replayable_point, false);

      end_lsn_v = LSN(1000000000);
      replayable_point_scn.convert_from_ts(ObTimeUtility::current_time() + 100000000);
      // 留一条日志
      while (count_append > 1) {
        EXPECT_EQ(OB_SUCCESS, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
        EXPECT_EQ(false, next_log_min_scn.is_valid());
        prev_next_success_scn = iterator.iterator_impl_.prev_entry_scn_;
        count_append--;
      }

      // 验证append切回raw后是否正常工作
      {
        int64_t id3 = ATOMIC_AAF(&palf_id_, 1);
        std::vector<SCN> logts_append;
        std::vector<LSN> lsns_append;
        int count_append = 5;
        PALF_LOG(INFO, "runlin trace 8.1.0", "raw_write_leader_lsn", raw_write_leader.palf_handle_impl_->get_max_lsn(),
            "new_leader_lsn", leader.palf_handle_impl_->get_max_lsn());
        EXPECT_EQ(OB_SUCCESS, submit_log_private(leader, count_append, id, log_size, lsns_append, logts_append));
        SCN max_scn_case4 = leader.palf_handle_impl_->get_max_scn();
        EXPECT_EQ(OB_SUCCESS, wait_until_has_committed(leader, leader.palf_handle_impl_->get_max_lsn()));
        EXPECT_EQ(OB_SUCCESS, change_access_mode_to_raw_write(raw_write_leader));
        PALF_LOG(INFO, "runlin trace 8.1", "raw_write_leader_lsn", raw_write_leader.palf_handle_impl_->get_max_lsn(),
            "new_leader_lsn", leader.palf_handle_impl_->get_max_lsn());
        EXPECT_EQ(OB_ITER_END, read_and_submit_group_log(leader,
                                                         raw_write_leader,
                                                         raw_write_leader.palf_handle_impl_->get_max_lsn()));
        EXPECT_EQ(OB_SUCCESS, wait_lsn_until_flushed(raw_write_leader.palf_handle_impl_->get_max_lsn(), raw_write_leader));
        PALF_LOG(INFO, "runlin trace 8.2", "raw_write_leader_lsn", raw_write_leader.palf_handle_impl_->get_max_lsn(),
            "new_leader_lsn", leader.palf_handle_impl_->get_max_lsn());

        // replayable_point_scn偏小
        SCN replayable_point_scn;
        replayable_point_scn.convert_for_tx(100);
        PALF_LOG(INFO, "runlin trace 8.3", K(iterator), K(replayable_point_scn), K(end_lsn_v),
          K(logts_append[0]), K(prev_next_success_scn));
        // 迭代前一轮的日志，不需要递减count_append
        EXPECT_EQ(OB_SUCCESS, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
        prev_next_success_scn = iterator.iterator_impl_.prev_entry_scn_;
        // 由于受控回放点不可读, next_log_min_scn应该为prev_next_success_scn+1
        EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
        EXPECT_EQ(SCN::plus(prev_next_success_scn, 1), next_log_min_scn);

        EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
        EXPECT_EQ(SCN::plus(prev_next_success_scn, 1), next_log_min_scn);

        // 推大受控回放点到第一条日志，但end_lsn_v也变为第一条日志的起点，此时会由于end_lsn_v不可读
        // 预期next_log_min_scn为prev_next_success_scn+1, 由于prev_next_success_scn和replayable_point_scn
        // 之间可能存在日志，因此不能将next_Log_min_ts设置为replayable_point_scn+1
        //
        // 模拟prev_entry_后没有日志，replayable_point_scn大于prev_entry_scn_
        end_lsn_v = lsns_append[0];
        replayable_point_scn = logts_append[0];
        PALF_LOG(INFO, "runlin trace 8.4", K(iterator), K(replayable_point_scn), K(end_lsn_v),
          K(logts_append[0]), K(prev_next_success_scn));
        EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
        EXPECT_EQ(SCN::plus(prev_next_success_scn, 1), next_log_min_scn);

        EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
        EXPECT_EQ(SCN::plus(prev_next_success_scn, 1), next_log_min_scn);

        // 模拟prev_entry_后没有日志，replayable_point_scn小于prev_entry_scn_
        replayable_point_scn = SCN::minus(prev_next_success_scn, 100);
        EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
        EXPECT_EQ(SCN::plus(prev_next_success_scn, 1), next_log_min_scn);

        EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
        EXPECT_EQ(SCN::plus(prev_next_success_scn, 1), next_log_min_scn);

        // 模拟prev_entry后有日志
        // 推大end_lsn_v到第二条日志的起点
        end_lsn_v = lsns_append[1]+2;
        replayable_point_scn = logts_append[1];
        PALF_LOG(INFO, "runlin trace 8.5", K(iterator), K(replayable_point_scn), K(end_lsn_v),
          K(logts_append[1]), K(prev_next_success_scn));
        EXPECT_EQ(OB_SUCCESS, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
        EXPECT_EQ(next_log_min_scn.is_valid(), false);
        prev_next_success_scn = iterator.iterator_impl_.prev_entry_scn_;
        EXPECT_EQ(prev_next_success_scn, logts_append[0]);

        PALF_LOG(INFO, "runlin trace 8.6", K(iterator), K(replayable_point_scn), K(end_lsn_v),
          K(logts_append[1]), K(prev_next_success_scn));
        // 模拟prev_entry_后有日志, 但不可见的情况
        // 此时会由于replayable_point_scn不吐出第二条日志
        // 模拟replayable_point_scn在prev_entry_之前的情况
        replayable_point_scn.convert_for_tx(100);
        EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
        EXPECT_EQ(SCN::plus(prev_next_success_scn, 1), next_log_min_scn);

        EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
        EXPECT_EQ(SCN::plus(prev_next_success_scn, 1), next_log_min_scn);

        // 模拟replayable_point_scn在prev_entry_之后的情况, 由于prev_enty_后有日志，因此
        // prev_entry_到replayable_point_scn之间不可能有未读过的日志，
        // 因此next_log_min_scn为replayable_point_scn + 1.
        replayable_point_scn = SCN::plus(prev_next_success_scn , 2);
        PALF_LOG(INFO, "runlin trace 8.7", K(iterator), K(replayable_point_scn), K(end_lsn_v),
          K(logts_append[1]), K(prev_next_success_scn));
        EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
        EXPECT_EQ(SCN::plus(replayable_point_scn, 1), next_log_min_scn);

        EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
        EXPECT_EQ(SCN::plus(replayable_point_scn, 1), next_log_min_scn);

        // 模拟replayable_point_scn在curr_entry之后的情况
        replayable_point_scn.convert_from_ts(ObTimeUtility::current_time() + 100000000);
        PALF_LOG(INFO, "runlin trace 8.8", K(iterator), K(replayable_point_scn), K(end_lsn_v),
          K(logts_append[1]), K(prev_next_success_scn));
        EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
        EXPECT_EQ(logts_append[1], next_log_min_scn);

        EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
        EXPECT_EQ(logts_append[1], next_log_min_scn);

        end_lsn_v = LSN(1000000000);
        EXPECT_EQ(OB_SUCCESS, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
        EXPECT_EQ(next_log_min_scn.is_valid(), false);
        EXPECT_EQ(iterator.iterator_impl_.prev_entry_scn_, logts_append[1]);
        prev_next_success_scn = iterator.iterator_impl_.prev_entry_scn_;

        // 验证受控回放
        replayable_point_scn.convert_for_tx(100);
        EXPECT_EQ(OB_ITER_END, iterator.next(replayable_point_scn, next_log_min_scn, iterate_end_by_replayable_point));
        EXPECT_EQ(SCN::plus(prev_next_success_scn, 1), next_log_min_scn);
        EXPECT_EQ(true, iterate_end_by_replayable_point);
      }
    }
  }
  // 验证重启
  restart_paxos_groups();
  {
    PalfHandleImplGuard raw_write_leader;
    PalfBufferIterator iterator;
    EXPECT_EQ(OB_SUCCESS, get_leader(id, raw_write_leader, leader_idx));
    PalfHandleImpl *raw_write_palf_handle_impl = raw_write_leader.palf_handle_impl_;
    auto get_file_end_lsn = []() -> LSN { return LSN(1000000000); };
    auto get_mode_version = [&mode_version, &mode_version_v]() -> int64_t {
      PALF_LOG(INFO, "runlin trace", K(*mode_version), K(mode_version_v));
      return *mode_version;
    };
    EXPECT_EQ(OB_SUCCESS,
        iterator.init(LSN(0), get_file_end_lsn, get_mode_version, &raw_write_palf_handle_impl->log_engine_.log_storage_));
    SCN max_scn = raw_write_leader.palf_handle_impl_->get_max_scn();
    int64_t count = 5 + 5 + 5 + 5 + 5;
    while (count > 0) {
      EXPECT_EQ(OB_SUCCESS, iterator.next(max_scn));
      count--;
    }
    EXPECT_EQ(OB_ITER_END, iterator.next(max_scn));
    EXPECT_EQ(OB_ITER_END, read_log_from_memory(raw_write_leader));
  }
}

TEST_F(TestObSimpleLogClusterSingleReplica, test_gc_block)
{
  SET_CASE_LOG_FILE(TEST_NAME, "test_gc_block");
  OB_LOGGER.set_log_level("TRACE");
  int64_t id = ATOMIC_AAF(&palf_id_, 1);
  int64_t leader_idx = 0;
  LSN upper_aligned_log_tail;
  PalfHandleImplGuard leader;
  EXPECT_EQ(OB_SUCCESS, create_paxos_group(id, leader_idx, leader));
  LogEngine *log_engine = &leader.palf_handle_impl_->log_engine_;
  LogStorage *log_meta_storage = &log_engine->log_meta_storage_;
  block_id_t min_block_id;
  share::SCN min_block_scn;
  EXPECT_EQ(OB_ENTRY_NOT_EXIST, log_engine->get_min_block_info_for_gc(min_block_id, min_block_scn));
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 31, leader_idx, MAX_LOG_BODY_SIZE));
  EXPECT_EQ(OB_SUCCESS, wait_lsn_until_flushed(leader.palf_handle_impl_->get_max_lsn(), leader));
  EXPECT_EQ(OB_ERR_OUT_OF_UPPER_BOUND, log_engine->get_min_block_info_for_gc(min_block_id, min_block_scn));
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 1, leader_idx, MAX_LOG_BODY_SIZE));
  EXPECT_EQ(OB_SUCCESS, wait_lsn_until_flushed(leader.palf_handle_impl_->get_max_lsn(), leader));
  block_id_t expect_block_id = 1;
  share::SCN expect_scn;
  EXPECT_EQ(OB_SUCCESS, log_engine->get_min_block_info_for_gc(min_block_id, min_block_scn));
  EXPECT_EQ(OB_SUCCESS, log_engine->get_block_min_scn(expect_block_id, expect_scn));
  EXPECT_EQ(expect_scn, min_block_scn);
  EXPECT_EQ(OB_SUCCESS, log_engine->delete_block(0));
  EXPECT_EQ(false, log_engine->min_block_max_scn_.is_valid());

  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 100, leader_idx, MAX_LOG_BODY_SIZE));
  EXPECT_EQ(OB_SUCCESS, wait_lsn_until_flushed(leader.palf_handle_impl_->get_max_lsn(), leader));
  expect_block_id = 2;
  EXPECT_EQ(OB_SUCCESS, log_engine->get_min_block_info_for_gc(min_block_id, min_block_scn));
  EXPECT_EQ(OB_SUCCESS, log_engine->get_block_min_scn(expect_block_id, expect_scn));
  EXPECT_EQ(expect_scn, min_block_scn);
  EXPECT_EQ(OB_SUCCESS, log_engine->delete_block(1));
  expect_block_id = 3;
  EXPECT_EQ(OB_SUCCESS, log_engine->get_min_block_info_for_gc(min_block_id, min_block_scn));
  EXPECT_EQ(OB_SUCCESS, log_engine->get_block_min_scn(expect_block_id, expect_scn));
  EXPECT_EQ(expect_scn, min_block_scn);
}

TEST_F(TestObSimpleLogClusterSingleReplica, test_iterator_with_flashback)
{
  SET_CASE_LOG_FILE(TEST_NAME, "test_iterator_with_flashback");
  OB_LOGGER.set_log_level("TRACE");
  int64_t id = ATOMIC_AAF(&palf_id_, 1);
  int64_t leader_idx = 0;
  PalfHandleImplGuard leader;
  PalfHandleImplGuard raw_write_leader;
  EXPECT_EQ(OB_SUCCESS, create_paxos_group(id, leader_idx, leader));
  PalfHandleImpl *palf_handle_impl = leader.palf_handle_impl_;
  const int64_t id_raw_write = ATOMIC_AAF(&palf_id_, 1);
  EXPECT_EQ(OB_SUCCESS, create_paxos_group(id_raw_write, leader_idx, raw_write_leader));
  EXPECT_EQ(OB_SUCCESS, change_access_mode_to_raw_write(raw_write_leader));

  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 2, leader_idx, 200));
  SCN max_scn1 = leader.palf_handle_impl_->get_max_scn();
  sleep(2);
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 2, leader_idx, 200));
  SCN max_scn2 = leader.palf_handle_impl_->get_max_scn();
  EXPECT_EQ(OB_SUCCESS, wait_until_has_committed(leader, leader.palf_handle_impl_->get_max_lsn()));

  EXPECT_EQ(OB_ITER_END, read_and_submit_group_log(leader, raw_write_leader));
  EXPECT_EQ(OB_SUCCESS, wait_until_has_committed(raw_write_leader, raw_write_leader.palf_handle_impl_->get_max_lsn()));

  PalfBufferIterator iterator;
  EXPECT_EQ(OB_SUCCESS, raw_write_leader.palf_handle_impl_->alloc_palf_buffer_iterator(LSN(0), iterator));
  // 迭代flashacbk之前的日志成功
  EXPECT_EQ(OB_SUCCESS, iterator.next(max_scn1));
  EXPECT_EQ(OB_SUCCESS, iterator.next(max_scn1));
  PALF_LOG(INFO, "runlin trace case1", K(iterator));

  EXPECT_EQ(OB_SUCCESS, raw_write_leader.palf_handle_impl_->inner_flashback(max_scn1));

  EXPECT_EQ(max_scn1, raw_write_leader.palf_handle_impl_->get_max_scn());

  int64_t mode_version;
  switch_flashback_to_append(raw_write_leader, mode_version);

  EXPECT_EQ(OB_SUCCESS, submit_log(raw_write_leader, 2, leader_idx, 333));
  EXPECT_EQ(OB_SUCCESS, wait_until_has_committed(raw_write_leader, raw_write_leader.palf_handle_impl_->get_max_lsn()));

  SCN max_scn3 = raw_write_leader.palf_handle_impl_->get_max_scn();
  PALF_LOG(INFO, "runlin trace case2", K(iterator), K(max_scn3), "end_lsn:", raw_write_leader.palf_handle_impl_->get_end_lsn());

  LSN iterator_end_lsn = iterator.iterator_storage_.end_lsn_;
  // 内存中有两条日志，预期返回成功, 此时会清cache
  EXPECT_EQ(OB_SUCCESS, iterator.next(max_scn3));
  EXPECT_FALSE(iterator_end_lsn == iterator.iterator_storage_.end_lsn_);
  EXPECT_EQ(OB_SUCCESS, iterator.next(max_scn3));

  EXPECT_EQ(OB_ITER_END, iterator.next(max_scn3));
}


} // namespace unittest
} // namespace oceanbase

int main(int argc, char **argv)
{
  RUN_SIMPLE_LOG_CLUSTER_TEST(TEST_NAME);
}
