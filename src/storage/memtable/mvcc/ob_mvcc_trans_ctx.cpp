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

#include "ob_mvcc_trans_ctx.h"
#include "ob_mvcc_ctx.h"
#include "ob_mvcc_row.h"
#include "share/rc/ob_tenant_base.h"
#include "storage/memtable/ob_memtable.h"
#include "storage/memtable/ob_memtable_context.h"
#include "storage/memtable/ob_memtable_data.h"
#include "storage/memtable/ob_memtable_util.h"
#include "lib/atomic/atomic128.h"
#include "storage/memtable/ob_lock_wait_mgr.h"
#include "storage/tx/ob_trans_ctx.h"
#include "storage/tx/ob_trans_part_ctx.h"
#include "ob_mvcc_ctx.h"
#include "storage/memtable/ob_memtable_interface.h"

namespace oceanbase
{
using namespace common;
using namespace transaction;
namespace memtable
{

void RedoDataNode::set(const ObMemtableKey *key,
                       const ObRowData &old_row,
                       const ObRowData &new_row,
                       const blocksstable::ObDmlFlag dml_flag,
                       const uint32_t modify_count,
                       const uint32_t acc_checksum,
                       const int64_t version,
                       const int32_t flag,
                       const int64_t seq_no,
                       const common::ObTabletID &tablet_id)
{
  key_.encode(*key);
  old_row_ = old_row;
  new_row_ = new_row;
  dml_flag_ = dml_flag;
  modify_count_ = modify_count;
  acc_checksum_ = acc_checksum;
  version_ = version;
  flag_ = flag;
  seq_no_ = seq_no;
  callback_ = NULL;
  tablet_id_ = tablet_id;
}

void TableLockRedoDataNode::set(
    const ObMemtableKey *key,
    const oceanbase::transaction::tablelock::ObTableLockOp &lock_op,
    const common::ObTabletID &tablet_id,
    ObITransCallback *callback)
{
  key_.encode(*key);
  lock_id_ = lock_op.lock_id_;
  owner_id_ = lock_op.owner_id_;
  lock_mode_ = lock_op.lock_mode_;
  lock_op_type_ = lock_op.op_type_;
  seq_no_ = lock_op.lock_seq_no_;
  callback_ = callback;
  tablet_id_ = tablet_id;
  create_timestamp_ = lock_op.create_timestamp_;
  create_schema_version_ = lock_op.create_schema_version_;
}

void ObITransCallback::set_log_ts(const int64_t log_ts)
{
  if (INT64_MAX == log_ts_) {
    log_ts_ = log_ts;
  }
}

int64_t ObITransCallback::get_log_ts() const
{
  return log_ts_;
}

int ObITransCallback::before_append_cb(const bool is_replay)
{
  need_fill_redo_ = !is_replay;
  need_submit_log_ = !is_replay;
  return before_append(is_replay);
}

int ObITransCallback::after_append_cb(const bool is_replay, const int ret_code)
{
  if (OB_SUCCESS != ret_code) {
    need_fill_redo_ = true;
    need_submit_log_ = true;
  }
  return after_append(is_replay, ret_code);
}

int ObITransCallback::log_submitted_cb()
{
  int ret = OB_SUCCESS;
  if (need_submit_log_) {
    if (OB_SUCC(log_submitted())) {
      need_submit_log_ = false;
    }
  }
  return ret;
}

int ObITransCallback::undo_log_submitted_cb()
{
  int ret = OB_SUCCESS;
  if (need_submit_log_) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "log has not beed submitted", K(ret), K(*this));
  } else if (!need_fill_redo_) {
  } else if (OB_SUCC(undo_log_submitted())) {
    need_submit_log_ = true;
  }
  return ret;
}

int ObITransCallback::log_sync_cb(const int64_t log_ts)
{
  int ret = OB_SUCCESS;
  if (!need_fill_redo_) {
  } else if (OB_UNLIKELY(INT64_MAX == log_ts)) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "log ts should not be invalid", K(ret), K(log_ts), K(*this));
  } else if (OB_SUCC(log_sync(log_ts))) {
    need_fill_redo_ = false;
  }
  return ret;
}

int ObITransCallback::log_sync_fail_cb()
{
  int ret = OB_SUCCESS;
  if (need_fill_redo_) {
    if (OB_SUCC(log_sync_fail())) {
      need_fill_redo_ = false;
    }
  }
  return ret;
}

int ObITransCallback::append(ObITransCallback *node)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    node->set_prev(this);
    node->set_next(this->get_next());
    this->get_next()->set_prev(node);
    this->set_next(node);
  }
  return ret;
}

int ObITransCallback::remove()
{
  int ret = OB_SUCCESS;
  ObITransCallback *prev = this->get_prev();
  ObITransCallback *next = this->get_next();
  if (OB_ISNULL(prev) || OB_ISNULL(next)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    prev->set_next(next);
    next->set_prev(prev);
  }
  return ret;
}

ObTransCallbackMgr::WRLockGuard::WRLockGuard(const SpinRWLock &rwlock)
  : time_guard_(5 * 1000 * 1000), // 5 second
    lock_guard_(rwlock)
{
  time_guard_.click();
}

ObTransCallbackMgr::RDLockGuard::RDLockGuard(const SpinRWLock &rwlock)
  : time_guard_(5 * 1000 * 1000), // 5 second
    lock_guard_(rwlock)
{
  time_guard_.click();
}

void ObTransCallbackMgr::reset()
{
  int64_t stat = ATOMIC_LOAD(&parallel_stat_);

  callback_list_.reset();
  if (PARALLEL_STMT == stat && NULL != callback_lists_) {
    for (int i = 0; i < MAX_CALLBACK_LIST_COUNT; ++i) {
      if (!callback_lists_[i].empty()) {
        ob_abort();
        TRANS_LOG(ERROR, "txn callback list is broken", K(stat), K(i), K(this));
      }
    }
  }
  if (OB_NOT_NULL(callback_lists_)) {
    cb_allocator_.free(callback_lists_);
    callback_lists_ = NULL;
  }
  parallel_stat_ = 0;
  leader_changed_ = false;
  callback_main_list_append_count_ = 0;
  callback_slave_list_append_count_ = 0;
  callback_slave_list_merge_count_ = 0;
  callback_remove_for_trans_end_count_ = 0;
  callback_remove_for_remove_memtable_count_ = 0;
  callback_remove_for_fast_commit_count_ = 0;
  callback_remove_for_rollback_to_count_ = 0;
  pending_log_size_ = 0;
  flushed_log_size_ = 0;
}

int ObTransCallbackMgr::append(ObITransCallback *node)
{
  int ret = OB_SUCCESS;
  const int64_t tid = get_itid() + 1;
  const int64_t slot = tid % MAX_CALLBACK_LIST_COUNT;
  int64_t stat = ATOMIC_LOAD(&parallel_stat_);

  if (OB_FAIL(before_append(node))) {
    TRANS_LOG(ERROR, "before_append failed", K(ret), K(node));
  } else {
    if (PARALLEL_STMT == stat) {
      if (NULL == callback_lists_) {
        WRLockGuard guard(rwlock_);
        if (NULL == callback_lists_) {
          ObTxCallbackList *tmp_callback_lists = NULL;
          if (NULL == (tmp_callback_lists = (ObTxCallbackList *)cb_allocator_.alloc(
                         sizeof(ObTxCallbackList) * MAX_CALLBACK_LIST_COUNT))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
            TRANS_LOG(WARN, "alloc cb lists fail", K(ret));
          } else {
            for (int i = 0; i < MAX_CALLBACK_LIST_COUNT; ++i) {
              UNUSED(new(tmp_callback_lists + i) ObTxCallbackList(*this));
            }
            callback_lists_ = tmp_callback_lists;
          }
        }
      }

      if (OB_SUCC(ret)) {
        if (NULL == callback_lists_) {
          ret = OB_ERR_UNEXPECTED;
          TRANS_LOG(WARN, "callback lists is not inited", K(ret));
        } else {
          ret = callback_lists_[slot].append_callback(node);
          add_slave_list_append_cnt();
        }
      }
    } else {
      ret = callback_list_.append_callback(node);
      add_main_list_append_cnt();
    }

    int tmp_ret = OB_SUCCESS;
    if (OB_TMP_FAIL(after_append(node, ret))) {
      TRANS_LOG(ERROR, "after_append failed", K(tmp_ret), K(node));
      if (OB_SUCC(ret)) {
        ret = tmp_ret;
      }
    }
  }

  return ret;
}

int ObTransCallbackMgr::before_append(ObITransCallback *node)
{
  int ret = OB_SUCCESS;
  int64_t size = node->get_data_size();

  int64_t new_size = inc_pending_log_size(size);
  try_merge_multi_callback_lists(new_size, size, node->is_logging_blocked());
  if (for_replay_) {
    inc_flushed_log_size(size);
  }
  if (OB_FAIL(node->before_append_cb(for_replay_))) {
    TRANS_LOG(ERROR, "before_append_cb failed", K(ret), K(node));
  }

  return ret;
}

int ObTransCallbackMgr::after_append(ObITransCallback *node, const int ret_code)
{
  int ret = OB_SUCCESS;

  if (OB_SUCCESS != ret_code) {
    int64_t size = node->get_data_size();
    inc_pending_log_size(-1 * size);
    if (for_replay_) {
      inc_flushed_log_size(-1 * size);
    }
  }

  if (OB_FAIL(node->after_append_cb(for_replay_, ret_code))) {
    TRANS_LOG(ERROR, "after_before_append_cb failed", K(ret), K(node));
  }

  return ret;
}

int ObTransCallbackMgr::rollback_to(const int64_t to_seq_no,
                                    const int64_t from_seq_no)
{
  int ret = OB_SUCCESS;
  if (0 > to_seq_no || 0 > from_seq_no) {
    ret = OB_INVALID_ARGUMENT;
    TRANS_LOG(WARN, "invalid argument", K(ret), K(from_seq_no), K(to_seq_no));
  } else if (OB_FAIL(callback_list_.remove_callbacks_for_rollback_to(to_seq_no))) {
    TRANS_LOG(WARN, "invalid argument", K(ret), K(from_seq_no), K(to_seq_no));
  }
  return ret;
}

void ObTransCallbackMgr::merge_multi_callback_lists()
{
  int64_t stat = ATOMIC_LOAD(&parallel_stat_);
  int64_t cnt = 0;
  if (PARALLEL_STMT == stat) {
    WRLockGuard guard(rwlock_);
    if (OB_NOT_NULL(callback_lists_)) {
      for (int64_t i = 0; i < MAX_CALLBACK_LIST_COUNT; ++i) {
        cnt = callback_list_.concat_callbacks(callback_lists_[i]);
        add_slave_list_merge_cnt(cnt);
      }
    }
#ifndef NDEBUG
    TRANS_LOG(INFO, "merge callback lists to callback list", K(stat), K(host_.get_tx_id()));
#endif
  }
}

void ObTransCallbackMgr::force_merge_multi_callback_lists()
{
  int64_t cnt = 0;
  WRLockGuard guard(rwlock_);
  if (OB_NOT_NULL(callback_lists_)) {
    for (int64_t i = 0; i < MAX_CALLBACK_LIST_COUNT; ++i) {
      cnt = callback_list_.concat_callbacks(callback_lists_[i]);
      add_slave_list_merge_cnt(cnt);
    }
  }
  TRANS_LOG(DEBUG, "force merge callback lists to callback list", K(host_.get_tx_id()));
}

transaction::ObPartTransCtx *ObTransCallbackMgr::get_trans_ctx() const
{
  return host_.get_trans_ctx();
}

void ObTransCallbackMgr::reset_pdml_stat()
{
  bool need_retry = true;
  while (need_retry) {
    WRLockGuard guard(rwlock_);
    int64_t stat = ATOMIC_LOAD(&parallel_stat_);
    if (!ATOMIC_BCAS(&parallel_stat_, stat, 0)) {
      TRANS_LOG(ERROR, "reset parallel stat when leader revoke encounter parallel",
                K(stat), K(parallel_stat_));
    } else {
      need_retry = false;
    }
  }

  force_merge_multi_callback_lists();
}

int ObTransCallbackMgr::remove_callbacks_for_fast_commit(bool &has_remove)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(callback_list_.remove_callbacks_for_fast_commit(has_remove))) {
    TRANS_LOG(WARN, "remove callbacks for fast commit fail", K(ret));
  }

  return ret;
}

int ObTransCallbackMgr::remove_callback_for_uncommited_txn(ObIMemtable *memtable)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(memtable)) {
    ret = OB_INVALID_ARGUMENT;
    TRANS_LOG(WARN, "memtable is null", K(ret));
  } else if (OB_FAIL(callback_list_.remove_callbacks_for_remove_memtable(memtable))) {
    TRANS_LOG(WARN, "fifo remove callback fail", K(ret), K(*memtable));
  }

  return ret;
}

int ObTransCallbackMgr::clean_unlog_callbacks(int64_t &removed_cnt)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(callback_list_.clean_unlog_callbacks(removed_cnt))) {
    TRANS_LOG(WARN, "clean unlog callbacks failed", K(ret));
  }

  return ret;
}

int ObTransCallbackMgr::calc_checksum_before_log_ts(const int64_t log_ts,
                                                    uint64_t &checksum,
                                                    int64_t &checksum_log_ts)
{
  int ret = OB_SUCCESS;

  if (INT64_MAX == log_ts) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "log ts is invalid", K(log_ts));
  } else if (OB_FAIL(callback_list_.tx_calc_checksum_before_log_ts(log_ts))) {
    TRANS_LOG(WARN, "calc checksum with minor freeze failed", K(ret), K(log_ts));
  } else {
    callback_list_.get_checksum_and_log_ts(checksum, checksum_log_ts);
  }

  return ret;
}

void ObTransCallbackMgr::update_checksum(const uint64_t checksum,
                                         const int64_t checksum_log_ts)
{
  callback_list_.update_checksum(checksum, checksum_log_ts);
}

int64_t ObTransCallbackMgr::inc_pending_log_size(const int64_t size)
{
  int64_t new_size = -1;
  if (!for_replay_) {
    int64_t old_size = ATOMIC_FAA(&pending_log_size_, size);
    new_size = ATOMIC_LOAD(&pending_log_size_);
    if (old_size < 0 || new_size < 0) {
      ObIMemtableCtx *mt_ctx = NULL;
      transaction::ObTransCtx *trans_ctx = NULL;
      if (NULL == (mt_ctx = static_cast<ObIMemtableCtx *>(&host_))) {
        TRANS_LOG(ERROR, "mt_ctx is null", K(size), K(old_size), K(new_size), K(host_));
      } else if (NULL == (trans_ctx = mt_ctx->get_trans_ctx())) {
        TRANS_LOG(ERROR, "trans ctx get failed", K(size), K(old_size), K(new_size), K(*mt_ctx));
      } else {
        TRANS_LOG(ERROR, "increase remaining data size less than 0!",
                  K(size), K(old_size), K(new_size), K(*trans_ctx));
      }
    }
  }
  return new_size;
}

void ObTransCallbackMgr::try_merge_multi_callback_lists(const int64_t new_size, const int64_t size, const bool is_logging_blocked)
{
  if (!for_replay_) {
    int64_t old_size = new_size - size;
    if (size < 0 || new_size < 0 || old_size < 0) {
    } else if ((0 != GCONF._private_buffer_size
                && old_size < GCONF._private_buffer_size
                && new_size >= GCONF._private_buffer_size)
               || is_logging_blocked) {
      // merge the multi callback lists once the immediate logging is satisfied.
      merge_multi_callback_lists();
    }
  }
}

int ObTransCallbackMgr::get_memtable_key_arr(ObMemtableKeyArray &memtable_key_arr)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(callback_list_.get_memtable_key_arr_w_timeout(memtable_key_arr))) {
    if (OB_ITER_STOP == ret) {
      ret = OB_SUCCESS;
    } else {
      TRANS_LOG(WARN, "lifo callback get memtablekey fail", K(ret), K(memtable_key_arr));
    }
  } else {
    //do nothing
  }

  return ret;
}

void ObTransCallbackMgr::acquire_callback_list()
{
  int64_t stat = ATOMIC_LOAD(&parallel_stat_);
  int64_t tid = get_itid() + 1;
  if (0 == stat) {
    if (!ATOMIC_BCAS(&parallel_stat_, 0, tid << 32)) {
      ATOMIC_STORE(&parallel_stat_, PARALLEL_STMT);
    }
  } else if (tid == (stat >> 32)) {
    if (!ATOMIC_BCAS(&parallel_stat_, stat, stat + 1)) {
      TRANS_LOG(ERROR, "Unexpected status", K(this), K(tid_), K(ref_cnt_), K(tid));
    }
  } else {
    // https://yuque.antfin.com/ob/transaction/xzwarh
    ATOMIC_STORE(&parallel_stat_, PARALLEL_STMT);
  }
}

void ObTransCallbackMgr::revert_callback_list()
{
  int64_t stat = ATOMIC_LOAD(&parallel_stat_);
  const int64_t tid = get_itid() + 1;
  const int64_t slot = tid % MAX_CALLBACK_LIST_COUNT;
  int64_t cnt = 0;
  if (0 == stat) {
    WRLockGuard guard(rwlock_);
    // https://yuque.antfin.com/ob/transaction/xzwarh
    if (OB_NOT_NULL(callback_lists_)) {
      cnt = callback_list_.concat_callbacks(callback_lists_[slot]);
      add_slave_list_merge_cnt(cnt);
    }
  } else if (tid == (stat >> 32)) {
    if (0 == ref_cnt_) {
      UNUSED(ATOMIC_BCAS(&parallel_stat_, stat, 0));
    } else {
      UNUSED(ATOMIC_BCAS(&parallel_stat_, stat, stat - 1));
    }
  } else {
    // We need merge callback list for causality bwteen them
    WRLockGuard guard(rwlock_);
    if (OB_NOT_NULL(callback_lists_)) {
      cnt = callback_list_.concat_callbacks(callback_lists_[slot]);
      add_slave_list_merge_cnt(cnt);
    }
  }
}

void ObTransCallbackMgr::wakeup_waiting_txns_()
{
  if (OB_ISNULL(MTL(ObLockWaitMgr*))) {
    TRANS_LOG(WARN, "MTL(ObLockWaitMgr*) is null");
  } else {
    ObMemtableCtx &mem_ctx = static_cast<ObMemtableCtx&>(host_);
    MTL(ObLockWaitMgr*)->wakeup(mem_ctx.get_trans_ctx()->get_trans_id());
  }
}

void ObTransCallbackMgr::set_for_replay(const bool for_replay)
{
  ATOMIC_STORE(&for_replay_, for_replay);
  if (for_replay) {
    reset_pdml_stat();
  }
}

int ObTransCallbackMgr::replay_fail(const int64_t log_timestamp)
{
  return callback_list_.replay_fail(log_timestamp);
}

int ObTransCallbackMgr::replay_succ(const int64_t log_timestamp)
{
  return OB_SUCCESS;
}

int ObTransCallbackMgr::trans_end(const bool commit)
{
  int ret = common::OB_SUCCESS;
  // If the txn ends abnormally, there may still be tasks in execution. Our
  // solution is that before the txn resets, all callback_lists need be
  // cleaned up after blocking new writes (through end_code). So if PDML
  // exists and some data is cached in callback_lists, we need merge them into
  // main callback_list
  merge_multi_callback_lists();
  if (commit) {
    ret = callback_list_.tx_commit();
  } else {
    ret = callback_list_.tx_abort();
  }
  if (OB_SUCC(ret)) {
    wakeup_waiting_txns_();
  }
  return ret;
}

void ObTransCallbackMgr::calc_checksum_all()
{
  callback_list_.tx_calc_checksum_all();
}

void ObTransCallbackMgr::print_callbacks()
{
  callback_list_.tx_print_callback();
}

void ObTransCallbackMgr::elr_trans_preparing()
{
  callback_list_.tx_elr_preparing();
}

void ObTransCallbackMgr::trans_start()
{
  reset();
}

int ObMvccRowCallback::before_append(const bool is_replay)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(memtable_)) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "memtable is NULL", K(ret));
  } else if (!is_replay) {
    inc_unsubmitted_cnt_();
    inc_unsynced_cnt_();
  }

  return ret;
}

int ObMvccRowCallback::after_append(const bool is_replay, const int ret_code)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(memtable_)) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "memtable is NULL", K(ret));
  } else if (OB_SUCCESS != ret_code) {
    if (!is_replay) {
      dec_unsubmitted_cnt_();
      dec_unsynced_cnt_();
    }
  }

  return ret;
}

int ObMvccRowCallback::log_submitted()
{
  int ret = OB_SUCCESS;

  if (OB_NOT_NULL(memtable_)) {
    if (OB_FAIL(dec_unsubmitted_cnt_())) {
      TRANS_LOG(ERROR, "dec unsubmitted cnt failed", K(ret), K(*this));
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "memtable is NULL", K(ret));
  }

  return ret;
}

int ObMvccRowCallback::undo_log_submitted()
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(memtable_)) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "memtable is NULL", K(ret));
  } else {
    inc_unsubmitted_cnt_();
  }

  return ret;
}

bool ObMvccRowCallback::is_logging_blocked() const
{
  const bool is_blocked = memtable_->get_logging_blocked();
  if (is_blocked) {
    TRANS_LOG(WARN, "block logging", K(is_blocked), KP(memtable_),
              K(memtable_->get_key().get_tablet_id()));
  }
  return is_blocked;
}

int ObMvccRowCallback::clean()
{
  unlink_trans_node();
  return OB_SUCCESS;
}

int ObMvccRowCallback::del()
{
  int ret = OB_SUCCESS;

  if (NULL != old_row_.data_) {
    ctx_.old_row_free((void *)(old_row_.data_));
    old_row_.data_ = NULL;
  }

  if (need_submit_log_ && need_fill_redo_) {
    log_submitted();
  }
  if (need_fill_redo_) {
    dec_unsynced_cnt_();
  }

  ret = remove();
  return ret;
}

int ObMvccRowCallback::get_memtable_key(uint64_t &table_id, ObStoreRowkey &rowkey) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(key_.decode(rowkey))) {
    TRANS_LOG(WARN, "memtable key decode failed", K(ret));
  }
  return ret;
}

const common::ObTabletID &ObMvccRowCallback::get_tablet_id() const
{
  return memtable_->get_key().get_tablet_id();
}

bool ObMvccRowCallback::on_memtable(const ObIMemtable * const memtable)
{
  return memtable == memtable_;
}

ObIMemtable *ObMvccRowCallback::get_memtable() const
{
  return memtable_;
};

int ObMvccRowCallback::print_callback()
{
  ObRowLatchGuard guard(value_.latch_);

  TRANS_LOG(INFO, "print callback", K(*this));
  return OB_SUCCESS;
}

int ObMvccRowCallback::merge_memtable_key(ObMemtableKeyArray &memtable_key_arr,
    ObMemtableKey &memtable_key, const common::ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;

  int64_t count = memtable_key_arr.count();
  int64_t i = 0;
  for (; i < count; i++) {
    // XXX maybe
    if (memtable_key_arr.at(i).get_hash_val() == memtable_key.hash()) {
      break;
    }
  }
  if (i == count) {
    ObMemtableKeyInfo memtable_key_info;
    if (OB_FAIL(memtable_key_info.init(memtable_key.hash()))) {
      TRANS_LOG(WARN, "memtable key info init fail", K(ret));
    } else {
      memtable_key_info.set_tablet_id(tablet_id);
      memtable_key.to_string(memtable_key_info.get_buf(), ObMemtableKeyInfo::MEMTABLE_KEY_INFO_BUF_SIZE);
      if (OB_FAIL(memtable_key_arr.push_back(memtable_key_info))) {
        TRANS_LOG(WARN, "memtable_key_arr push item fail", K(ret), K(memtable_key_arr), K(memtable_key_info));
      }
    }
  }

  return ret;
}

int ObMvccRowCallback::merge_memtable_key(ObMemtableKeyArray &memtable_key_arr)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(key_.get_rowkey())) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "static_cast key to ObMemtableKey* error", K(ret), "context", *this);
  } else if (OB_ISNULL(memtable_)) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "ObMvccRowCallback's memtable_ ptr is NULL", K(ret), "context", *this);
  } else if (OB_FAIL(merge_memtable_key(memtable_key_arr, key_, memtable_->get_key().get_tablet_id()))) {
    TRANS_LOG(WARN, "memtable_key_arr push item fail", K(ret), K(key_));
  } else {
    //do nothing
  }

  return ret;
}

int ObMvccRowCallback::elr_trans_preparing()
{
  ObRowLatchGuard guard(value_.latch_);

  ObMemtableCtx *mem_ctx = static_cast<ObMemtableCtx*>(&ctx_);
  if (NULL != tnode_) {
    value_.elr(mem_ctx->get_trans_ctx()->get_trans_id(),
               ctx_.get_commit_version(),
               get_tablet_id(),
               (ObMemtableKey*)&key_);
  }
  return OB_SUCCESS;
}

int ObMvccRowCallback::get_trans_id(ObTransID &trans_id) const
{
  int ret = OB_SUCCESS;
  ObMemtableCtx *mem_ctx = static_cast<ObMemtableCtx*>(&ctx_);
  ObTransCtx *trans_ctx = NULL;

  if (OB_ISNULL(mem_ctx)) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "unexpected mem ctx", K(ret));
  } else if (OB_ISNULL(trans_ctx = mem_ctx->get_trans_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "unexpected trans ctx", K(ret), K(ctx_));
  } else {
    trans_id = trans_ctx->get_trans_id();
  }

  return ret;
}

int ObMvccRowCallback::get_cluster_version(uint64_t &cluster_version) const
{
  int ret = OB_SUCCESS;
  ObMemtableCtx *mem_ctx = static_cast<ObMemtableCtx*>(&ctx_);
  ObTransCtx *trans_ctx = NULL;
  if (OB_ISNULL(mem_ctx)) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "unexpected mem ctx", K(ret));
  } else if (OB_ISNULL(trans_ctx = mem_ctx->get_trans_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "unexpected trans ctx", K(ret), K(ctx_));
  } else {
    cluster_version = trans_ctx->get_cluster_version();
  }
  return ret;
}

ObTransCtx *ObMvccRowCallback::get_trans_ctx() const
{
  int ret = OB_SUCCESS;
  ObMemtableCtx *mem_ctx = static_cast<ObMemtableCtx*>(&ctx_);
  ObTransCtx *trans_ctx = NULL;

  if (OB_ISNULL(mem_ctx)) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "unexpected mem ctx", K(ret));
  } else if (OB_ISNULL(trans_ctx = mem_ctx->get_trans_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "unexpected trans ctx", K(ret), K(ctx_));
  } else {
    // do nothing
  }

  return trans_ctx;
}

int ObMvccRowCallback::calc_checksum(const int64_t checksum_log_ts,
                                     ObBatchChecksum *checksumer)
{
  ObRowLatchGuard guard(value_.latch_);

  if (NULL != tnode_) {
    if (not_calc_checksum_) {
      // verification
      if (blocksstable::ObDmlFlag::DF_LOCK != get_dml_flag()) {
        TRANS_LOG(ERROR, "only LOCK node can not calc checksum",
                  K(*this), K(get_dml_flag()));
      }
    } else if (checksum_log_ts <= log_ts_) {
      tnode_->checksum(*checksumer);
      ((ObMemtableDataHeader *)tnode_->buf_)->checksum(*checksumer);
    }
  }

  return OB_SUCCESS;
}

int ObMvccRowCallback::checkpoint_callback()
{
  int ret = OB_SUCCESS;

  ObRowLatchGuard guard(value_.latch_);

  if (need_submit_log_ || need_fill_redo_) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "checkpoint never called on unsynced callback", KPC(this));
  } else if (OB_FAIL(value_.remove_callback(*this))) {
    TRANS_LOG(ERROR, "remove callback from trans node failed", K(ret), K(*this));
  }

  return ret;
}

static blocksstable::ObDmlFlag get_dml_flag(ObMvccTransNode *node)
{
  return NULL == node ? blocksstable::ObDmlFlag::DF_NOT_EXIST : reinterpret_cast<ObMemtableDataHeader *>(node->buf_)->dml_flag_;
}

blocksstable::ObDmlFlag ObMvccRowCallback::get_dml_flag() const
{
  return memtable::get_dml_flag(tnode_);
}

int ObMvccRowCallback::trans_commit()
{
  int ret = OB_SUCCESS;
  ObMvccTransNode *prev = NULL;
  ObMvccTransNode *next = NULL;
  const bool for_read = false;

  ObRowLatchGuard guard(value_.latch_);

  if (NULL != tnode_) {
    if (OB_FAIL(link_and_get_next_node(next))) {
      TRANS_LOG(WARN, "link trans node failed", K(ret));
    } else {
      // if (ctx_.is_for_replay()) {
      //   // verify current node checksum by previous node
      //   prev = tnode_->prev_;
      //   if (not_calc_checksum_) {
      //     // to fix the case of replay self written log
      //     // do nothing
      //   } else if (NULL == prev) {
      //     // do nothing
      //   } else if (prev->is_committed() &&
      //       prev->version_ == tnode_->version_ &&
      //       prev->modify_count_ + 1 == tnode_->modify_count_) {
      //     if (OB_FAIL(tnode_->verify_acc_checksum(prev->acc_checksum_))) {
      //       TRANS_LOG(ERROR, "current row checksum error", K(ret), K(value_), K(*prev), K(*tnode_));
      //       if (ObServerConfig::get_instance().ignore_replay_checksum_error) {
      //         // rewrite ret
      //         ret = OB_SUCCESS;
      //       }
      //     }
      //   } else {
      //     // do nothing
      //   }
      //   if (OB_SUCC(ret)) {
      //     // verify next node checksum by current node
      //     if (not_calc_checksum_) {
      //       // to fix the case of replay self log
      //       // do thing
      //     } else if (NULL == next) {
      //       // do nothing
      //     } else if (next->is_committed() &&
      //         tnode_->version_ == next->version_ &&
      //         tnode_->modify_count_ + 1 == next->modify_count_) {
      //       if (OB_FAIL(next->verify_acc_checksum(tnode_->acc_checksum_))) {
      //         TRANS_LOG(ERROR, "next row checksum error", K(ret), K(value_), K(*tnode_), K(*next));
      //         if (ObServerConfig::get_instance().ignore_replay_checksum_error) {
      //           // rewrite ret
      //           ret = OB_SUCCESS;
      //         }
      //       }
      //     } else {
      //       // do nothing
      //     }
      //   }
      // }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(value_.trans_commit(ctx_.get_commit_version(), *tnode_))) {
          TRANS_LOG(WARN, "mvcc trans ctx trans commit error", K(ret), K_(ctx), K_(value));
        } else if (FALSE_IT(tnode_->trans_commit(ctx_.get_commit_version(), ctx_.get_tx_end_log_ts()))) {
        } else if (!ctx_.is_for_replay() && FALSE_IT(wakeup_row_waiter_if_need_())) {
        } else if (blocksstable::ObDmlFlag::DF_LOCK == get_dml_flag()) {
          unlink_trans_node();
        } else {
          const int64_t MAX_TRANS_NODE_CNT = 2 * GCONF._ob_elr_fast_freeze_threshold;
          if (value_.total_trans_node_cnt_ > MAX_TRANS_NODE_CNT
              && NULL != memtable_
              && !memtable_->has_hotspot_row()) {
            memtable_->set_contain_hotspot_row();
            TRANS_LOG(INFO, "[FF] trans commit and set hotspot row success", K_(*memtable), K_(value), K_(ctx), K(*this));
          }
          (void)ATOMIC_FAA(&value_.update_since_compact_, 1);
          if (value_.need_compact(for_read, ctx_.is_for_replay())) {
            if (ctx_.is_for_replay()) {
              if (0 != ctx_.get_replay_compact_version() && INT64_MAX != ctx_.get_replay_compact_version()) {
                memtable_->row_compact(&value_, ctx_.is_for_replay(), ctx_.get_replay_compact_version());
              }
            } else {
              memtable_->row_compact(&value_, ctx_.is_for_replay(), INT64_MAX - 100);
            }
          }
        }
      }
    }
  }
  return ret;
}

/*
 * wakeup_row_waiter_if_need_ - wakeup txn waiting to acquire row ownership
 *
 * The 'Row-Lock' is imply by active txn's dirty write
 * if current active txn aborted, committed or rollback to savepoint, its dirty
 * write maybe discard or be invalid, in these situations we should wakeup
 * any waiters who wait on the row
 *
 * to verify the current active txn on current row 'released ownership' actually,
 * we use these conditions:
 * 1) TxNode is in a determinated state: COMMITTED or ABORTED
 * 2) precedure of this TxNode is not owned by current txn
 *
 * however, this may cause false positive (which means the lock was not release
 * by this txn actually). but it is better to accept such ratio in order to
 * keep simple and fast
 */
int ObMvccRowCallback::wakeup_row_waiter_if_need_()
{
  int ret = OB_SUCCESS;
  if (NULL != tnode_ &&
      (tnode_->is_committed() || tnode_->is_aborted()) &&
      (tnode_->prev_ == NULL || tnode_->prev_->tx_id_ != tnode_->tx_id_)) {
    ret = value_.wakeup_waiter(get_tablet_id(), key_);
    /*****[for deadlock]*****/
    ObLockWaitMgr *p_lwm = MTL(ObLockWaitMgr *);
    if (OB_ISNULL(p_lwm)) {
      TRANS_LOG(WARN, "lock wait mgr is nullptr", K(*this));
    } else {
      p_lwm->reset_hash_holder(get_tablet_id(), key_, ctx_.get_tx_id());
    }
    /************************/
  }
  return ret;
}

int ObMvccRowCallback::trans_abort()
{
  ObRowLatchGuard guard(value_.latch_);

  if (NULL != tnode_) {
    if (!(tnode_->is_committed() || tnode_->is_aborted())) {
      tnode_->trans_abort(ctx_.get_tx_end_log_ts());
      wakeup_row_waiter_if_need_();
      unlink_trans_node();
    } else if (tnode_->is_committed()) {
      TRANS_LOG(ERROR, "abort on a committed node", K(*this));
    }
  }
  return OB_SUCCESS;
}

int ObMvccRowCallback::rollback_callback()
{
  ObRowLatchGuard guard(value_.latch_);

  if (NULL != tnode_) {
    tnode_->set_aborted();
    wakeup_row_waiter_if_need_();
    unlink_trans_node();
  }

  if (need_submit_log_
      && need_fill_redo_
      && INT64_MAX == log_ts_) {
    ctx_.inc_pending_log_size(-1 * data_size_);
  }

  return OB_SUCCESS;
}

MutatorType ObMvccRowCallback::get_mutator_type() const
{
  return MutatorType::MUTATOR_ROW;
}

int ObMvccRowCallback::get_redo(RedoDataNode &redo_node)
{
  int ret = OB_SUCCESS;
  if (NULL == key_.get_rowkey() || NULL == tnode_) {
    ret = OB_ENTRY_NOT_EXIST;
  } else if (!is_link_) {
    ret = OB_STATE_NOT_MATCH;
    TRANS_LOG(ERROR, "get_redo: trans_nod not link", K(ret), K(*this));
  } else {
    uint32_t last_acc_checksum = 0;
    if (NULL != tnode_->prev_) {
      last_acc_checksum = tnode_->prev_->acc_checksum_;
    } else {
      last_acc_checksum = 0;
    }
    tnode_->cal_acc_checksum(last_acc_checksum);
    const ObMemtableDataHeader *mtd = reinterpret_cast<const ObMemtableDataHeader *>(tnode_->buf_);
    ObRowData new_row;
    new_row.set(mtd->buf_, (int32_t)(data_size_ - sizeof(*mtd)));
    redo_node.set(&key_,
                  old_row_,
                  new_row,
                  mtd->dml_flag_,
                  tnode_->modify_count_,
                  tnode_->acc_checksum_,
                  tnode_->version_,
                  0,
                  seq_no_,
                  this->get_tablet_id());
    redo_node.set_callback(this);
  }
  return ret;
}

int ObMvccRowCallback::link_and_get_next_node(ObMvccTransNode *&next)
{
  int ret = OB_SUCCESS;
  if (NULL == tnode_) {
    // pass
  } else if (is_link_) {
    // pass
  } else {
    if (OB_ISNULL(memtable_)) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(ERROR, "memtable_ is NULL", K(ret), K_(ctx));
    } else if (OB_FAIL(value_.insert_trans_node(ctx_, *tnode_, memtable_->get_allocator(), next))) {
      TRANS_LOG(ERROR, "insert trans node failed", K(ret), K_(ctx));
    } else {
      is_link_ = true;
    }
  }
  return ret;
}

int ObMvccRowCallback::link_trans_node()
{
  bool ret = OB_SUCCESS;
  ObMvccTransNode *unused = NULL;
  ret = link_and_get_next_node(unused);
  return ret;
}

void ObMvccRowCallback::unlink_trans_node()
{
  int ret = OB_SUCCESS;
  if (is_link_) {
    if (NULL == tnode_) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(ERROR, "trans node is NULL", K(ret), K_(ctx), K_(value));
      // trans node of LOCK type is allowed to be unlinked even after committed
    } else if (tnode_->is_committed() && blocksstable::ObDmlFlag::DF_LOCK != get_dml_flag()) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(ERROR, "unlink committed trans node", K(ret), K_(ctx), K_(value), K(*tnode_));
    } else if (OB_FAIL(value_.unlink_trans_node(*tnode_))) {
      // TODO(handora.qc): temproary remove it
      // TRANS_LOG(ERROR, "unlink trans node failed", K(ret), K_(ctx), K_(value), K(*tnode_));
    } else {
      is_link_ = false;
    }
  }
}

int ObMvccRowCallback::row_delete()
{
  return del();
}

int64_t ObMvccRowCallback::to_string(char *buf, const int64_t buf_len) const
{
  int64_t pos = 0;
  databuff_printf(buf, buf_len, pos,
      "[this=%p, ctx=%s, is_link=%d, need_fill_redo=%d, "
      "value=%s, tnode=(%s), "
      "seq_no=%ld, memtable=%p, log_ts=%lu",
      this, to_cstring(ctx_), is_link_, need_fill_redo_,
      to_cstring(value_), NULL == tnode_ ? "null" : to_cstring(*tnode_),
      seq_no_, memtable_, log_ts_);
  return pos;
}

int ObMvccRowCallback::log_sync(const int64_t log_ts)
{
  int ret = OB_SUCCESS;

  memtable_->set_rec_log_ts(log_ts);
  memtable_->set_max_end_log_ts(log_ts);
  (void)tnode_->fill_log_timestamp(log_ts);
  ctx_.update_max_submitted_seq_no(seq_no_);
  if (OB_FAIL(dec_unsynced_cnt_())) {
    TRANS_LOG(ERROR, "memtable dec unsynced cnt error", K(ret), K(log_ts),
              K(memtable_->get_unsynced_cnt()));
  } else {
    // do nothing
  }

  return ret;
}

int ObMvccRowCallback::log_sync_fail()
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dec_unsynced_cnt_())) {
    TRANS_LOG(ERROR, "memtable dec unsynced cnt error", K(ret),
              K(memtable_->get_unsynced_cnt()));
  } else {
    unlink_trans_node();
  }

  return ret;
}

int ObMvccRowCallback::clean_unlog_cb()
{
  int ret = OB_SUCCESS;
  // NB: we should pay attention to the logic that leader switch(whether forcely
  // or gracefully) will ensure the invokation of all callbacks(whether succeed
  // or fail). So we add defensive code here for safety.

  if (need_fill_redo_ && !need_submit_log_) {
    TRANS_LOG(ERROR, "all callbacks must be invoked before leader switch", K(*this));
  } else if (!need_fill_redo_ && need_submit_log_) {
    TRANS_LOG(ERROR, "It will never on success before submit log", K(*this));
  } else if (need_fill_redo_ && need_submit_log_) {
    unlink_trans_node();
    need_submit_log_ = false;
    need_fill_redo_ = false;
    dec_unsubmitted_cnt_();
    dec_unsynced_cnt_();
  }
  return ret;
}

void ObMvccRowCallback::inc_unsubmitted_cnt_()
{
  if (OB_NOT_NULL(memtable_)) {
    memtable_->inc_unsubmitted_cnt();
    ctx_.inc_unsubmitted_cnt();
  }
}

void ObMvccRowCallback::inc_unsynced_cnt_()
{
  if (OB_NOT_NULL(memtable_)) {
    memtable_->inc_unsynced_cnt();
    ctx_.inc_unsynced_cnt();
  }
}

int ObMvccRowCallback::dec_unsubmitted_cnt_()
{
  int ret = OB_SUCCESS;

  if (OB_NOT_NULL(memtable_)) {
    ret = memtable_->dec_unsubmitted_cnt();
    ctx_.dec_unsubmitted_cnt();
  }

  return ret;
}

int ObMvccRowCallback::dec_unsynced_cnt_()
{
  int ret = OB_SUCCESS;

  if (OB_NOT_NULL(memtable_)) {
    ret = memtable_->dec_unsynced_cnt();
    ctx_.dec_unsynced_cnt();
  }

  return ret;
}

MutatorType ObITransCallback::get_mutator_type() const
{
  return MutatorType::MUTATOR_ROW;
}

}; // end namespace mvcc
}; // end namespace oceanbase

