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

#include <gtest/gtest.h>

#define private public
#define protected public

#include "storage/memtable/ob_memtable.h"
#include "storage/memtable/mvcc/ob_mvcc_trans_ctx.h"
#include "storage/memtable/ob_memtable_context.h"
#include "lib/random/ob_random.h"

namespace oceanbase
{

namespace unittest
{
using namespace oceanbase::common;
using namespace oceanbase::memtable;

class ObMockTxCallback : public ObITransCallback
{
public:
  ObMockTxCallback(ObMemtable *mt,
                   bool need_submit_log = true,
                   bool need_fill_redo = true,
                   int64_t log_ts = INT64_MAX,
                   int64_t seq_no = INT64_MAX)
    : ObITransCallback(need_fill_redo, need_submit_log),
      mt_(mt), seq_no_(seq_no) { log_ts_ = log_ts; }

  virtual ObIMemtable* get_memtable() const override { return mt_; }
  virtual int64_t get_seq_no() const override { return seq_no_; }
  virtual int checkpoint_callback() override;
  virtual int rollback_callback() override;
  virtual int calc_checksum(const int64_t checksum_log_ts,
                            ObBatchChecksum *checksumer) override;

  ObMemtable *mt_;
  int64_t seq_no_;
};

class ObMockBitSet {
public:
  ObMockBitSet() { reset(); }
  void reset()
  {
    memset(set_, 0, sizeof(char) * 100000);
  }

  void add_bit(int64_t bit)
  {
    if (bit >= 100000) {
      ob_abort();
    } else if (1 == set_[bit]) {
      TRANS_LOG(ERROR, "set in unexpected", K(bit));
      ob_abort();
    } else {
      set_[bit] = 1;
      TRANS_LOG(INFO, "add bit", K(bit), K(lbt()));
    }
  }

  bool equal(ObMockBitSet &other)
  {
    bool bret = true;
    for (int i = 0; i < 100000; i++) {
      if (set_[i] != other.set_[i]) {
        if (set_[i] == 1) {
          TRANS_LOG(ERROR, "different bit set", K(i),
                    "set_[i]", "1",
                    "other.set_[i]", "0");
        } else {
          TRANS_LOG(ERROR, "different bit set", K(i),
                    "set_[i]", "0",
                    "other.set_[i]", "1");
        }
        bret = false;
      }
    }

    return bret;
  }

  char set_[100000];
};

class TestTxCallbackList : public ::testing::Test
{
public:
  TestTxCallbackList()
    : seq_counter_(0),
      mt_counter_(0),
      mt_ctx_(),
      cb_allocator_(),
      mgr_(mt_ctx_, cb_allocator_),
      callback_list_(mgr_) {}
  virtual void SetUp() override
  {
    mt_counter_ = 0;
    fast_commit_reserve_cnt_ = 0;
    checkpoint_cnt_ = 0;
    rollback_cnt_ = 0;
    checksum_.reset();
    callback_list_.reset();
    mgr_.reset();
    TRANS_LOG(INFO, "setup success");
  }

  virtual void TearDown() override
  {
    mt_counter_ = 0;
    fast_commit_reserve_cnt_ = 0;
    callback_list_.reset();
    mgr_.reset();
    TRANS_LOG(INFO, "teardown success");
  }

  static void SetUpTestCase()
  {
    TRANS_LOG(INFO, "SetUpTestCase");
  }
  static void TearDownTestCase()
  {
    TRANS_LOG(INFO, "TearDownTestCase");
  }

  ObMockTxCallback *create_callback(ObMemtable *mt,
                                    bool need_submit_log = true,
                                    bool need_fill_redo = true,
                                    int64_t log_ts = INT64_MAX)
  {
    int64_t seq_no = ++seq_counter_;
    ObMockTxCallback *cb = new ObMockTxCallback(mt,
                                                need_submit_log,
                                                need_fill_redo,
                                                log_ts,
                                                seq_no);
    return cb;
  }

  void create_and_append_callback(ObMemtable *mt,
                                  bool need_submit_log = true,
                                  bool need_fill_redo = true,
                                  int64_t log_ts = INT64_MAX)
  {
    ObMockTxCallback *cb = create_callback(mt,
                                           need_submit_log,
                                           need_fill_redo,
                                           log_ts);
    EXPECT_NE(NULL, (long)cb);
    EXPECT_EQ(OB_SUCCESS, callback_list_.append_callback(cb));
  }

  ObMemtable *create_memtable()
  {
    mt_counter_++;
    return (ObMemtable *)(mt_counter_);
  }

  int64_t get_seq_no() const
  {
    return seq_counter_;
  }

  bool is_checksum_equal(int64_t no, ObMockBitSet &res)
  {
    ObMockBitSet other;
    other.reset();
    for (int64_t i = 1; i <= no; i++)
    {
      other.add_bit(i);
    }

    return res.equal(other);
  }

  static int64_t fast_commit_reserve_cnt_;
  static int64_t checkpoint_cnt_;
  static int64_t rollback_cnt_;
  static ObMockBitSet checksum_;

  int64_t seq_counter_;
  int64_t mt_counter_;
  ObMemtableCtx mt_ctx_;
  ObMemtableCtxCbAllocator cb_allocator_;
  ObTransCallbackMgr mgr_;
  ObTxCallbackList callback_list_;
};

int64_t TestTxCallbackList::fast_commit_reserve_cnt_;
int64_t TestTxCallbackList::checkpoint_cnt_;
int64_t TestTxCallbackList::rollback_cnt_;
ObMockBitSet TestTxCallbackList::checksum_;

int ObMockTxCallback::checkpoint_callback()
{
  TestTxCallbackList::checkpoint_cnt_++;
  return OB_SUCCESS;
}

int ObMockTxCallback::rollback_callback()
{
  TestTxCallbackList::rollback_cnt_++;
  return OB_SUCCESS;
}

int ObMockTxCallback::calc_checksum(const int64_t checksum_log_ts,
                                    ObBatchChecksum *)
{
  if (checksum_log_ts <= log_ts_) {
    TestTxCallbackList::checksum_.add_bit(seq_no_);
    TRANS_LOG(INFO, "need to calc checksum", K(checksum_log_ts), K(log_ts_), K(seq_no_));
  } else {
    TRANS_LOG(INFO, "no need to calc checksum", K(checksum_log_ts), K(log_ts_), K(seq_no_));
  }
  return OB_SUCCESS;
}

TEST_F(TestTxCallbackList, remove_callback_by_tx_commit)
{
  ObMemtable *memtable = create_memtable();
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);

  EXPECT_EQ(3, callback_list_.get_length());

  EXPECT_EQ(OB_SUCCESS, callback_list_.tx_commit());

  EXPECT_EQ(true, callback_list_.empty());
  EXPECT_EQ(3, mgr_.get_callback_remove_for_trans_end_count());
}

TEST_F(TestTxCallbackList, remove_callback_by_tx_abort)
{
  ObMemtable *memtable = create_memtable();
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             true   /*need_fill_redo*/);

  EXPECT_EQ(3, callback_list_.get_length());

  EXPECT_EQ(OB_SUCCESS, callback_list_.tx_abort());

  EXPECT_EQ(true, callback_list_.empty());
  EXPECT_EQ(3, mgr_.get_callback_remove_for_trans_end_count());
}

TEST_F(TestTxCallbackList, remove_callback_by_release_memtable)
{
  ObMemtable *memtable1 = create_memtable();
  ObMemtable *memtable2 = create_memtable();
  ObMemtable *memtable3 = create_memtable();
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);
  create_and_append_callback(memtable2,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);
  create_and_append_callback(memtable3,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);
  create_and_append_callback(memtable2,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2/*log_ts*/);
  create_and_append_callback(memtable3,
                             false, /*need_submit_log*/
                             true /*need_fill_redo*/);
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             true  /*need_fill_redo*/);
  create_and_append_callback(memtable3,
                             true, /*need_submit_log*/
                             true  /*need_fill_redo*/);
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false,  /*need_fill_redo*/
                             100     /*log_ts*/);

  EXPECT_EQ(9, callback_list_.get_length());

  EXPECT_EQ(OB_SUCCESS,
            callback_list_.remove_callbacks_for_remove_memtable(memtable2));

  EXPECT_EQ(7, callback_list_.get_length());
  EXPECT_EQ(2, mgr_.get_callback_remove_for_remove_memtable_count());

  EXPECT_EQ(OB_SUCCESS,
            callback_list_.remove_callbacks_for_remove_memtable(memtable1));

  EXPECT_EQ(5, callback_list_.get_length());
  EXPECT_EQ(4, mgr_.get_callback_remove_for_remove_memtable_count());

  EXPECT_EQ(OB_SUCCESS,
            callback_list_.remove_callbacks_for_remove_memtable(memtable3));

  EXPECT_EQ(4, callback_list_.get_length());
  EXPECT_EQ(5, mgr_.get_callback_remove_for_remove_memtable_count());

  EXPECT_EQ(5, checkpoint_cnt_);
}

TEST_F(TestTxCallbackList, remove_callback_by_fast_commit)
{
  ObMemtable *memtable1 = create_memtable();
  ObMemtable *memtable2 = create_memtable();
  ObMemtable *memtable3 = create_memtable();

  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);
  create_and_append_callback(memtable2,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2/*log_ts*/);
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2/*log_ts*/);
  create_and_append_callback(memtable3,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             3/*log_ts*/);
  create_and_append_callback(memtable2,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             3/*log_ts*/);
  create_and_append_callback(memtable3,
                             false, /*need_submit_log*/
                             true /*need_fill_redo*/);
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             true  /*need_fill_redo*/);
  create_and_append_callback(memtable3,
                             true, /*need_submit_log*/
                             true  /*need_fill_redo*/);
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false,  /*need_fill_redo*/
                             100     /*log_ts*/);

  EXPECT_EQ(9, callback_list_.get_length());

  fast_commit_reserve_cnt_ = 16;
  bool has_remove = false;
  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_fast_commit(has_remove));
  EXPECT_EQ(8, callback_list_.get_length());
  EXPECT_EQ(1, mgr_.get_callback_remove_for_fast_commit_count());
  EXPECT_EQ(true, has_remove);

  fast_commit_reserve_cnt_ = 14;
  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_fast_commit(has_remove));
  EXPECT_EQ(6, callback_list_.get_length());
  EXPECT_EQ(3, mgr_.get_callback_remove_for_fast_commit_count());
  EXPECT_EQ(true, has_remove);

  fast_commit_reserve_cnt_ = 1;
  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_fast_commit(has_remove));
  EXPECT_EQ(4, callback_list_.get_length());
  EXPECT_EQ(5, mgr_.get_callback_remove_for_fast_commit_count());
  EXPECT_EQ(true, has_remove);

  fast_commit_reserve_cnt_ = 1;
  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_fast_commit(has_remove));
  EXPECT_EQ(4, callback_list_.get_length());
  EXPECT_EQ(5, mgr_.get_callback_remove_for_fast_commit_count());
  EXPECT_EQ(false, has_remove);

  EXPECT_EQ(5, checkpoint_cnt_);
}

TEST_F(TestTxCallbackList, remove_callback_by_rollback_to)
{
  ObMemtable *memtable1 = create_memtable();
  ObMemtable *memtable2 = create_memtable();
  ObMemtable *memtable3 = create_memtable();

  int64_t savepoint0 = get_seq_no();
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);
  create_and_append_callback(memtable2,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2/*log_ts*/);
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2/*log_ts*/);
  int64_t savepoint1 = get_seq_no();
  create_and_append_callback(memtable3,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             3/*log_ts*/);
  create_and_append_callback(memtable2,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             3/*log_ts*/);
  create_and_append_callback(memtable3,
                             true, /*need_submit_log*/
                             true /*need_fill_redo*/);
  create_and_append_callback(memtable1,
                             true, /*need_submit_log*/
                             true  /*need_fill_redo*/);
  int64_t savepoint2 = get_seq_no();
  create_and_append_callback(memtable3,
                             true, /*need_submit_log*/
                             true  /*need_fill_redo*/);
  int64_t savepoint3 = get_seq_no();
  create_and_append_callback(memtable1,
                             true, /*need_submit_log*/
                             true  /*need_fill_redo*/);

  EXPECT_EQ(9, callback_list_.get_length());

  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_rollback_to(savepoint3));
  EXPECT_EQ(8, callback_list_.get_length());
  EXPECT_EQ(1, mgr_.get_callback_remove_for_rollback_to_count());

  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_rollback_to(savepoint2));
  EXPECT_EQ(7, callback_list_.get_length());
  EXPECT_EQ(2, mgr_.get_callback_remove_for_rollback_to_count());

  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_rollback_to(savepoint1));
  EXPECT_EQ(3, callback_list_.get_length());
  EXPECT_EQ(6, mgr_.get_callback_remove_for_rollback_to_count());

  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_rollback_to(savepoint0));
  EXPECT_EQ(0, callback_list_.get_length());
  EXPECT_EQ(9, mgr_.get_callback_remove_for_rollback_to_count());

  EXPECT_EQ(9, rollback_cnt_);
}

TEST_F(TestTxCallbackList, remove_callback_by_clean_unlog_callbacks)
{
  ObMemtable *memtable1 = create_memtable();
  ObMemtable *memtable2 = create_memtable();
  ObMemtable *memtable3 = create_memtable();

  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);
  create_and_append_callback(memtable2,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2/*log_ts*/);
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2/*log_ts*/);
  create_and_append_callback(memtable3,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             3/*log_ts*/);
  create_and_append_callback(memtable2,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             3/*log_ts*/);
  create_and_append_callback(memtable3,
                             true, /*need_submit_log*/
                             true /*need_fill_redo*/);
  create_and_append_callback(memtable1,
                             true, /*need_submit_log*/
                             true  /*need_fill_redo*/);
  create_and_append_callback(memtable3,
                             true, /*need_submit_log*/
                             true  /*need_fill_redo*/);
  create_and_append_callback(memtable1,
                             true, /*need_submit_log*/
                             true  /*need_fill_redo*/);

  EXPECT_EQ(9, callback_list_.get_length());
  int64_t removed_cnt = 0;
  EXPECT_EQ(OB_SUCCESS, callback_list_.clean_unlog_callbacks(removed_cnt));
  EXPECT_EQ(5, callback_list_.get_length());

  EXPECT_EQ(4, rollback_cnt_);
}

TEST_F(TestTxCallbackList, remove_callback_by_replay_fail)
{
  TRANS_LOG(INFO, "CASE: remove_callback_by_replay_fail");
  ObMemtable *memtable1 = create_memtable();
  ObMemtable *memtable2 = create_memtable();
  ObMemtable *memtable3 = create_memtable();

  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);
  create_and_append_callback(memtable2,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2/*log_ts*/);
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2/*log_ts*/);
  create_and_append_callback(memtable3,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             3/*log_ts*/);
  create_and_append_callback(memtable2,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             3/*log_ts*/);
  create_and_append_callback(memtable3,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             3/*log_ts*/);
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             4/*log_ts*/);
  create_and_append_callback(memtable3,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             4/*log_ts*/);
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             4/*log_ts*/);

  EXPECT_EQ(9, callback_list_.get_length());

  EXPECT_EQ(OB_SUCCESS, callback_list_.replay_fail(4 /*log_timestamp*/));
  EXPECT_EQ(6, callback_list_.get_length());

  EXPECT_EQ(3, rollback_cnt_);
}

TEST_F(TestTxCallbackList, checksum_leader_tx_end_basic)
{
  TRANS_LOG(INFO, "CASE: checksum_leader_tx_end_basic");
  ObMemtable *memtable = create_memtable();
  create_and_append_callback(memtable,
                             true, /*need_submit_log*/
                             true /*need_fill_redo*/);
  create_and_append_callback(memtable,
                             true, /*need_submit_log*/
                             true /*need_fill_redo*/);
  create_and_append_callback(memtable,
                             true, /*need_submit_log*/
                             true /*need_fill_redo*/);

  EXPECT_EQ(3, callback_list_.get_length());

  EXPECT_EQ(OB_SUCCESS, callback_list_.tx_calc_checksum_all());

  EXPECT_EQ(true, is_checksum_equal(3, checksum_));
  EXPECT_EQ(INT64_MAX, callback_list_.checksum_log_ts_);
}

TEST_F(TestTxCallbackList, checksum_follower_tx_end)
{
  ObMemtable *memtable = create_memtable();
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1 /*log_ts*/);
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2 /*log_ts*/);
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             3 /*log_ts*/);

  EXPECT_EQ(3, callback_list_.get_length());

  EXPECT_EQ(OB_SUCCESS, callback_list_.tx_calc_checksum_all());

  EXPECT_EQ(true, is_checksum_equal(3, checksum_));
  EXPECT_EQ(INT64_MAX, callback_list_.checksum_log_ts_);
}

TEST_F(TestTxCallbackList, checksum_leader_tx_end_harder)
{
  TRANS_LOG(INFO, "CASE: checksum_leader_tx_end_harder");
  ObMemtable *memtable = create_memtable();
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1 /*log_ts*/);
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1 /*log_ts*/);
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2 /*log_ts*/);
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             true /*need_fill_redo*/);
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             false /*need_fill_redo*/);

  EXPECT_EQ(5, callback_list_.get_length());

  EXPECT_EQ(OB_SUCCESS, callback_list_.tx_calc_checksum_all());

  EXPECT_EQ(true, is_checksum_equal(5, checksum_));
  EXPECT_EQ(INT64_MAX, callback_list_.checksum_log_ts_);
}

TEST_F(TestTxCallbackList, checksum_leader_tx_end_harderer)
{
  TRANS_LOG(INFO, "CASE: checksum_leader_tx_end_harderer");
  ObMemtable *memtable = create_memtable();
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1 /*log_ts*/);
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1 /*log_ts*/);
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2 /*log_ts*/);
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1 /*log_ts*/);
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             true /*need_fill_redo*/);
  create_and_append_callback(memtable,
                             false, /*need_submit_log*/
                             false /*need_fill_redo*/);

  EXPECT_EQ(6, callback_list_.get_length());

  EXPECT_EQ(OB_SUCCESS, callback_list_.tx_calc_checksum_all());

  EXPECT_EQ(true, is_checksum_equal(6, checksum_));
  EXPECT_EQ(INT64_MAX, callback_list_.checksum_log_ts_);
}

TEST_F(TestTxCallbackList, checksum_remove_memtable_and_tx_end)
{
  TRANS_LOG(INFO, "CASE: checksum_remove_memtable_and_tx_end");
  ObMemtable *memtable1 = create_memtable();
  ObMemtable *memtable2 = create_memtable();
  ObMemtable *memtable3 = create_memtable();
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);
  create_and_append_callback(memtable2,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);
  create_and_append_callback(memtable3,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2/*log_ts*/);
  create_and_append_callback(memtable2,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2/*log_ts*/);
  create_and_append_callback(memtable3,
                             false, /*need_submit_log*/
                             true /*need_fill_redo*/);
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             true  /*need_fill_redo*/);
  create_and_append_callback(memtable3,
                             true, /*need_submit_log*/
                             true  /*need_fill_redo*/);
  create_and_append_callback(memtable1,
                             true, /*need_submit_log*/
                             true  /*need_fill_redo*/);


  EXPECT_EQ(9, callback_list_.get_length());

  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_remove_memtable(memtable2));
  EXPECT_EQ(true, is_checksum_equal(5, checksum_));
  EXPECT_EQ(3, callback_list_.checksum_log_ts_);

  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_remove_memtable(memtable3));
  EXPECT_EQ(true, is_checksum_equal(5, checksum_));
  EXPECT_EQ(3, callback_list_.checksum_log_ts_);


  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_remove_memtable(memtable1));
  EXPECT_EQ(true, is_checksum_equal(5, checksum_));
  EXPECT_EQ(3, callback_list_.checksum_log_ts_);

  EXPECT_EQ(OB_SUCCESS, callback_list_.tx_calc_checksum_all());
  EXPECT_EQ(true, is_checksum_equal(9, checksum_));
  EXPECT_EQ(INT64_MAX, callback_list_.checksum_log_ts_);
}


TEST_F(TestTxCallbackList, checksum_fast_commit_and_tx_end)
{
  TRANS_LOG(INFO, "CASE: checksum_fast_commit_and_tx_end");
  ObMemtable *memtable1 = create_memtable();
  ObMemtable *memtable2 = create_memtable();
  ObMemtable *memtable3 = create_memtable();

  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);
  create_and_append_callback(memtable2,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2/*log_ts*/);
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2/*log_ts*/);
  create_and_append_callback(memtable3,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             3/*log_ts*/);
  create_and_append_callback(memtable2,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             3/*log_ts*/);
  create_and_append_callback(memtable3,
                             false, /*need_submit_log*/
                             true /*need_fill_redo*/);
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             true  /*need_fill_redo*/);
  create_and_append_callback(memtable3,
                             true, /*need_submit_log*/
                             true  /*need_fill_redo*/);
  create_and_append_callback(memtable1,
                             true, /*need_submit_log*/
                             true  /*need_fill_redo*/);

  EXPECT_EQ(9, callback_list_.get_length());

  fast_commit_reserve_cnt_ = 16;
  bool has_remove = false;
  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_fast_commit(has_remove));
  EXPECT_EQ(true, is_checksum_equal(1, checksum_));
  EXPECT_EQ(2, callback_list_.checksum_log_ts_);

  fast_commit_reserve_cnt_ = 14;
  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_fast_commit(has_remove));
  EXPECT_EQ(true, is_checksum_equal(3, checksum_));
  EXPECT_EQ(3, callback_list_.checksum_log_ts_);

  fast_commit_reserve_cnt_ = 1;
  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_fast_commit(has_remove));
  EXPECT_EQ(true, is_checksum_equal(5, checksum_));
  EXPECT_EQ(4, callback_list_.checksum_log_ts_);

  fast_commit_reserve_cnt_ = 1;
  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_fast_commit(has_remove));
  EXPECT_EQ(true, is_checksum_equal(5, checksum_));
  EXPECT_EQ(4, callback_list_.checksum_log_ts_);

  EXPECT_EQ(OB_SUCCESS, callback_list_.tx_calc_checksum_all());
  EXPECT_EQ(true, is_checksum_equal(9, checksum_));
  EXPECT_EQ(INT64_MAX, callback_list_.checksum_log_ts_);
}

TEST_F(TestTxCallbackList, checksum_rollback_to_and_tx_end)
{
  TRANS_LOG(INFO, "CASE: checksum_rollback_to_and_tx_end");
  ObMemtable *memtable1 = create_memtable();
  ObMemtable *memtable2 = create_memtable();
  ObMemtable *memtable3 = create_memtable();

  int64_t savepoint0 = get_seq_no();
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             1/*log_ts*/);
  create_and_append_callback(memtable2,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2/*log_ts*/);
  create_and_append_callback(memtable1,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             2/*log_ts*/);
  int64_t savepoint1 = get_seq_no();
  create_and_append_callback(memtable3,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             3/*log_ts*/);
  create_and_append_callback(memtable2,
                             false, /*need_submit_log*/
                             false, /*need_fill_redo*/
                             3/*log_ts*/);
  create_and_append_callback(memtable3,
                             true, /*need_submit_log*/
                             true /*need_fill_redo*/);
  create_and_append_callback(memtable1,
                             true, /*need_submit_log*/
                             true  /*need_fill_redo*/);
  int64_t savepoint2 = get_seq_no();
  create_and_append_callback(memtable3,
                             true, /*need_submit_log*/
                             true  /*need_fill_redo*/);
  int64_t savepoint3 = get_seq_no();
  create_and_append_callback(memtable1,
                             true, /*need_submit_log*/
                             true  /*need_fill_redo*/);

  EXPECT_EQ(9, callback_list_.get_length());

  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_rollback_to(savepoint3));
  EXPECT_EQ(true, is_checksum_equal(5, checksum_));
  EXPECT_EQ(4, callback_list_.checksum_log_ts_);

  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_rollback_to(savepoint2));
  EXPECT_EQ(true, is_checksum_equal(5, checksum_));
  EXPECT_EQ(4, callback_list_.checksum_log_ts_);

  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_rollback_to(savepoint1));
  EXPECT_EQ(true, is_checksum_equal(5, checksum_));
  EXPECT_EQ(4, callback_list_.checksum_log_ts_);

  EXPECT_EQ(OB_SUCCESS, callback_list_.remove_callbacks_for_rollback_to(savepoint0));
  EXPECT_EQ(true, is_checksum_equal(5, checksum_));
  EXPECT_EQ(4, callback_list_.checksum_log_ts_);

  EXPECT_EQ(OB_SUCCESS, callback_list_.tx_calc_checksum_all());
  EXPECT_EQ(true, is_checksum_equal(5, checksum_));
  EXPECT_EQ(INT64_MAX, callback_list_.checksum_log_ts_);
}

TEST_F(TestTxCallbackList, checksum_all_and_tx_end_test) {
  TRANS_LOG(INFO, "CASE: checksum_all_and_tx_end_test");
  int64_t length = 0;
  int64_t mt_cnt = 5;
  int64_t cur_log = 0;
  ObMemtable *mts[mt_cnt];
  ObITransCallback *need_submit_head = &(callback_list_.head_);
  ObMockBitSet my_calculate;
  my_calculate.reset();
  for (int i = 0; i < mt_cnt; i++) {
    mts[i] = create_memtable();
  }

  ObFunction<bool()> append_callback_op =
    [&]() -> bool {
      for (int i = 0; i < 10; i++) {
        ObMemtable *mt = mts[ObRandom::rand(0, mt_cnt - 1)];
        create_and_append_callback(mt,
                                   true, /*need_submit_log*/
                                   true  /*need_fill_redo*/);

      }
      return true;
    };

  ObFunction<bool()> on_success_op =
    [&]() -> bool {
      cur_log++;
      int i = 0;
      bool enable = false;
      int64_t max_cnt = ObRandom::rand(1, 5);
      for (ObITransCallback* it = need_submit_head->next_;
           it != &(callback_list_.head_) && i < max_cnt;
           it = it->next_) {
        i++;
        it->need_submit_log_ = false;
        it->need_fill_redo_ = false;
        it->log_ts_ = cur_log;
        enable = true;
        need_submit_head = it;
        my_calculate.add_bit(it->get_seq_no());
      }

      if (!enable) {
        cur_log--;
      }

      return enable;
    };

  ObFunction<bool()> remove_memtable_op =
    [&]() -> bool {
      ObMemtable *mt = mts[ObRandom::rand(0, mt_cnt-1)];
      bool enable = false;
      for (ObITransCallback* it = need_submit_head->next_;
           it != &(callback_list_.head_);
           it = it->next_) {
        if (it->need_submit_log_ || it->need_fill_redo_) {
          break;
        } else if (it->get_memtable() == mt) {
          enable = true;
          break;
        }
      }

      if (enable) {
        EXPECT_EQ(OB_SUCCESS,
                  callback_list_.remove_callbacks_for_remove_memtable(mt));
      }

      return enable;
    };

  ObFunction<bool()> fast_commit_op =
    [&]() -> bool{
      bool enable = false;

      fast_commit_reserve_cnt_ = 30;
      if (callback_list_.length_ > fast_commit_reserve_cnt_ / 2
          && !callback_list_.head_.next_->need_submit_log_) {
        enable = true;
      }

      bool has_remove = false;
      if (enable) {
        EXPECT_EQ(OB_SUCCESS,
                  callback_list_.remove_callbacks_for_fast_commit(has_remove));
        EXPECT_EQ(true, has_remove);
      }

      return enable;
    };

  ObFunction<bool()> rollback_to_op =
    [&]() -> bool{
      bool enable = false;
      if (!callback_list_.empty() &&
          callback_list_.head_.next_->get_seq_no() + 1 < seq_counter_ - 1) {
        int64_t seq = ObRandom::rand(callback_list_.head_.next_->get_seq_no() + 1,
                                       seq_counter_ - 1);
        enable = true;
        if (enable) {
          if (need_submit_head->get_seq_no() > seq) {
            need_submit_head = callback_list_.head_.prev_;
          }
          EXPECT_EQ(OB_SUCCESS,
                    callback_list_.remove_callbacks_for_rollback_to(seq));
          EXPECT_EQ(false, callback_list_.empty());
        }
      }

      return enable;
    };


  for (int c = 0; c < 10000; ) {
    int choice = ObRandom::rand(1, 10);
    ObFunction<bool()> *op = NULL;
    if (choice >= 1 && choice <= 3) {
      op = &append_callback_op;
    } else if (choice <= 5) {
      op = &on_success_op;
    } else if (choice <= 7) {
      op = &remove_memtable_op;
    } else if (choice <= 9) {
      op = &fast_commit_op;
    } else if (choice <= 10) {
      op = &rollback_to_op;
    }

    if ((*op)()) {
      TRANS_LOG(INFO, "choice on success", K(callback_list_), K(choice), K(c));
      c++;
    } else {
      TRANS_LOG(INFO, "choice on skip", K(callback_list_), K(choice), K(c));
    }
  }

  for (ObITransCallback* it = need_submit_head->next_;
       it != &(callback_list_.head_);
       it = it->next_) {
    EXPECT_EQ(it->need_submit_log_, true);
    EXPECT_EQ(it->need_fill_redo_, true);
    my_calculate.add_bit(it->get_seq_no());
  }

  EXPECT_EQ(OB_SUCCESS, callback_list_.tx_commit());
  EXPECT_EQ(true, my_calculate.equal(checksum_));

}

} // namespace unittest

namespace memtable
{
int ObTxCallbackList::remove_callbacks_for_fast_commit(bool &has_remove)
{
  int ret = OB_SUCCESS;
  has_remove = false;
  SpinLockGuard guard(latch_);

  const int64_t fast_commit_callback_count = unittest::TestTxCallbackList::fast_commit_reserve_cnt_;
  const int64_t recommand_reserve_count = (fast_commit_callback_count + 1) / 2;
  const int64_t need_remove_count = length_ - recommand_reserve_count;

  ObRemoveCallbacksForFastCommitFunctor functor(need_remove_count);
  functor.set_checksumer(checksum_log_ts_, &batch_checksum_);

  if (OB_FAIL(callback_(functor))) {
    TRANS_LOG(ERROR, "remove callbacks for fast commit wont report error", K(ret), K(functor));
  } else {
    callback_mgr_.add_fast_commit_callback_remove_cnt(functor.get_remove_cnt());
    ensure_checksum_(functor.get_checksum_last_log_ts());
    has_remove = OB_INVALID_TIMESTAMP != functor.get_checksum_last_log_ts();
    if (has_remove) {
      TRANS_LOG(INFO, "remove callbacks for fast commit", K(functor), K(*this));
    }
  }

  return ret;
}

int ObTxCallbackList::remove_callbacks_for_remove_memtable(ObIMemtable *memtable_for_remove)
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(latch_);

  ObRemoveSyncCallbacksWCondFunctor functor(
    // condition for remove
    [memtable_for_remove](ObITransCallback *callback) -> bool {
      if (callback->get_memtable() == memtable_for_remove) {
        return true;
      } else {
        return false;
      }
    }, // condition for stop
    [](ObITransCallback *) -> bool {
      return false;
    },
    false /*need_remove_data*/);
  functor.set_checksumer(checksum_log_ts_, &batch_checksum_);

  if (OB_FAIL(callback_(functor))) {
    TRANS_LOG(ERROR, "remove callbacks for remove memtable wont report error", K(ret), K(functor));
  } else {
    callback_mgr_.add_release_memtable_callback_remove_cnt(functor.get_remove_cnt());
    ensure_checksum_(functor.get_checksum_last_log_ts());
    if (functor.get_remove_cnt() > 0) {
      TRANS_LOG(INFO, "remove callbacks for remove memtable", KP(memtable_for_remove),
                K(functor), K(*this));
    }
  }

  return ret;
}
} // namespace memtable

} // namespace oceanbase

int main(int argc, char **argv)
{
  system("rm -rf test_tx_callback_list.log*");
  oceanbase::common::ObLogger::get_logger().set_file_name("test_tx_callback_list.log", true);
  oceanbase::common::ObLogger::get_logger().set_log_level("INFO");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
