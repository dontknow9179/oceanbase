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

#include "storage/tx/ob_ctx_tx_data.h"
#include "storage/tx/ob_trans_ctx_mgr.h"
#include "storage/tx/ob_tx_data_define.h"
#include "storage/tx/ob_trans_define.h"

namespace oceanbase
{

using namespace storage;

namespace transaction
{

#define GET_TX_TABLE_(tx_table)                                                       \
  ObTxTableGuard table_guard;                                                         \
  if (OB_ISNULL(ctx_mgr_)) {                                                          \
    ret = OB_ERR_UNEXPECTED;                                                          \
    TRANS_LOG(WARN, "ctx_mgr is null when get_tx_table", K(ret));                     \
  } else if (OB_FAIL(ctx_mgr_->get_tx_table_guard(table_guard))) {                    \
    TRANS_LOG(WARN, "get tx table guard without check failed", KR(ret), K(*this));    \
  } else if (OB_ISNULL(tx_table = table_guard.get_tx_table())) {                      \
    ret = OB_ERR_UNEXPECTED;                                                          \
    TRANS_LOG(WARN, "tx table is null", KR(ret), K(ctx_mgr_->get_ls_id()), K(*this)); \
  }

int ObCtxTxData::init(ObLSTxCtxMgr *ctx_mgr, int64_t tx_id)
{
  int ret = OB_SUCCESS;
  ObTxTable *tx_table = nullptr;
  if (OB_ISNULL(ctx_mgr) || tx_id < 0) {
    ret = OB_INVALID_ARGUMENT;
    TRANS_LOG(WARN, "invalid argument", K(ret), KP(ctx_mgr), K(tx_id));
  } else {
    ctx_mgr_ = ctx_mgr;
    ObTxTable *tx_table = nullptr;
    if (OB_ISNULL(ctx_mgr_)) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(WARN, "ctx_mgr is null when get_tx_table", K(ret));
    } else if (OB_ISNULL(tx_table = ctx_mgr_->get_tx_table())) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(WARN, "tx table is null", KR(ret), K(ctx_mgr_->get_ls_id()), K(*this));
    } else if (OB_FAIL(tx_table->alloc_tx_data(tx_data_))) {
      TRANS_LOG(WARN, "get tx data failed", KR(ret), K(ctx_mgr_->get_ls_id()));
    } else if (OB_ISNULL(tx_data_)) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(WARN, "tx data is unexpected null", KR(ret), K(ctx_mgr_->get_ls_id()));
    } else {
      tx_data_->tx_id_ = tx_id;
    }
  }
  return ret;
}

void ObCtxTxData::reset()
{
  ctx_mgr_ = nullptr;
  tx_data_ = nullptr;
  read_only_ = false;
  tx_commit_data_.reset();
}

void ObCtxTxData::destroy()
{
  int ret = OB_SUCCESS;
  if (tx_data_ != nullptr) {
    ObTxTable *tx_table = nullptr;
    GET_TX_TABLE_(tx_table);
    if (OB_FAIL(ret)) {

    } else if (OB_FAIL(free_tx_data_(tx_table, tx_data_))) {
      TRANS_LOG(WARN, "free tx data failed", K(ret), K(*this));

    } else {
      tx_data_ = nullptr;
    }
  }
}

int ObCtxTxData::insert_into_tx_table()
{
  int ret = OB_SUCCESS;
  common::ObTimeGuard tg("part_ctx::insert_into_tx_table", 100 * 1000);
  WLockGuard guard(lock_);
  tg.click();

  if (OB_FAIL(check_tx_data_writable_())) {
    TRANS_LOG(WARN, "tx data is not writeable", K(ret));
  } else {
    tg.click();
    ObTxTable *tx_table = nullptr;
    GET_TX_TABLE_(tx_table)
    if (OB_FAIL(ret)) {
    } else {
      tg.click();
      tx_commit_data_ = *tx_data_;
      if (OB_FAIL(insert_tx_data_(tx_table, tx_data_))) {
        TRANS_LOG(WARN, "insert tx data failed", K(ret), K(*this));
      } else {
        tg.click();
        read_only_ = true;
        tx_data_ = NULL;
        tx_commit_data_.is_in_tx_data_table_ = true;
      }
    }
  }
  if (tg.get_diff() > 100000) {
    TRANS_LOG(INFO, "ObCtxData insert into tx table const too much time", K(tg));
  }

  return ret;
}

int ObCtxTxData::recover_tx_data(const ObTxData *tmp_tx_data)
{
  int ret = OB_SUCCESS;
  WLockGuard guard(lock_);

  if (OB_FAIL(check_tx_data_writable_())) {
    TRANS_LOG(WARN, "tx data is not writeable", K(ret), K(*this));
  } else if (OB_ISNULL(tmp_tx_data)) {
    ret = OB_INVALID_ARGUMENT;
    TRANS_LOG(WARN, "invalid argument", K(ret), KP(tmp_tx_data));
  } else {
    *tx_data_ = *tmp_tx_data;
  }

  return ret;
}

int ObCtxTxData::replace_tx_data(ObTxData *&tmp_tx_data)
{
  int ret = OB_SUCCESS;
  WLockGuard guard(lock_);

  ObTxTable *tx_table = nullptr;
  GET_TX_TABLE_(tx_table);

  if (OB_FAIL(check_tx_data_writable_())) {
    TRANS_LOG(WARN, "tx data is not writeable", K(ret), K(*this));
  } else if (OB_ISNULL(tmp_tx_data)) {
    ret = OB_INVALID_ARGUMENT;
    TRANS_LOG(WARN, "invalid argument", K(ret), KP(tmp_tx_data));
  } else if (OB_FAIL(free_tx_data_(tx_table, tx_data_))) {
    TRANS_LOG(WARN, "free tx data failed", K(ret), K(*this));
  } else {
    tx_data_ = tmp_tx_data;
    tmp_tx_data = nullptr;
  }

  return ret;
}

int ObCtxTxData::deep_copy_tx_data_out(ObTxData *&tmp_tx_data)
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);

  if (OB_FAIL(check_tx_data_writable_())) {
    TRANS_LOG(WARN, "tx data is not writeable", K(ret), K(*this));
  } else {
    ObTxTable *tx_table = nullptr;
    GET_TX_TABLE_(tx_table)
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(deep_copy_tx_data_(tx_table, tmp_tx_data))) {
      TRANS_LOG(WARN, "deep copy tx data failed", K(ret), KPC(tmp_tx_data), K(*this));
    } else if (OB_ISNULL(tmp_tx_data)) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(ERROR, "copied tmp tx data is null", KR(ret), K(*this));
    }
  }

  return ret;
}

int ObCtxTxData::alloc_tmp_tx_data(storage::ObTxData *&tmp_tx_data)
{

  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);

  if (OB_FAIL(check_tx_data_writable_())) {
    TRANS_LOG(WARN, "tx data is not writeable", K(ret), K(*this));
  } else {
    ObTxTable *tx_table = nullptr;
    GET_TX_TABLE_(tx_table)
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(tx_table->alloc_tx_data(tmp_tx_data))) {
      TRANS_LOG(WARN, "alloc tx data failed", K(ret));
    }
  }

  return ret;
}

int ObCtxTxData::free_tmp_tx_data(ObTxData *&tmp_tx_data)
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);

  if (OB_FAIL(check_tx_data_writable_())) {
    TRANS_LOG(WARN, "tx data is not writeable", K(ret), K(*this));
  } else {
    ObTxTable *tx_table = nullptr;
    GET_TX_TABLE_(tx_table)
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(free_tx_data_(tx_table, tmp_tx_data))) {
      TRANS_LOG(WARN, "free tx data failed", K(ret), KPC(tmp_tx_data), K(*this));
    } else {
      tmp_tx_data = nullptr;
    }
  }

  return ret;
}

int ObCtxTxData::insert_tmp_tx_data(ObTxData *&tmp_tx_data)
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);

  if (OB_FAIL(check_tx_data_writable_())) {
    TRANS_LOG(WARN, "tx data is not writeable", K(ret), K(*this));
  } else {
    ObTxTable *tx_table = nullptr;
    GET_TX_TABLE_(tx_table)
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(insert_tx_data_(tx_table, tmp_tx_data))) {
      TRANS_LOG(WARN, "insert tx data failed", K(ret), KPC(tmp_tx_data), K(*this));
    } else {
      tmp_tx_data = nullptr;
    }
  }

  return ret;
}

void ObCtxTxData::get_tx_table(storage::ObTxTable *&tx_table)
{
  int ret = OB_SUCCESS;

  GET_TX_TABLE_(tx_table);

  if (OB_FAIL(ret)) {
    tx_table = nullptr;
  }
}

int ObCtxTxData::set_state(int32_t state)
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);

  if (OB_FAIL(check_tx_data_writable_())) {
    TRANS_LOG(WARN, "tx data is not writeable", K(ret), K(*this));
  } else {
    ATOMIC_STORE(&tx_data_->state_, state);
  }

  return ret;
}

int ObCtxTxData::set_commit_version(int64_t commit_version)
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);

  if (OB_FAIL(check_tx_data_writable_())) {
    TRANS_LOG(WARN, "tx data is not writeable", K(ret), K(*this));
  } else {
    ATOMIC_STORE(&tx_data_->commit_version_, commit_version);
  }

  return ret;
}

int ObCtxTxData::set_start_log_ts(int64_t start_ts)
{
  int ret = OB_SUCCESS;
  const int64_t tmp_start_ts = (OB_INVALID_TIMESTAMP == start_ts ? INT64_MAX : start_ts);
  RLockGuard guard(lock_);

  if (OB_FAIL(check_tx_data_writable_())) {
    TRANS_LOG(WARN, "tx data is not writeable", K(ret), K(*this));
  } else {
    ATOMIC_STORE(&tx_data_->start_log_ts_, tmp_start_ts);
  }

  return ret;
}

int ObCtxTxData::set_end_log_ts(int64_t end_ts)
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);

  if (OB_FAIL(check_tx_data_writable_())) {
    TRANS_LOG(WARN, "tx data is not writeable", K(ret), K(*this));
  } else {
    ATOMIC_STORE(&tx_data_->end_log_ts_, end_ts);
  }

  return ret;
}

int32_t ObCtxTxData::get_state() const
{
  RLockGuard guard(lock_);
  return (NULL != tx_data_ ? ATOMIC_LOAD(&tx_data_->state_): ATOMIC_LOAD(&tx_commit_data_.state_));
}

int64_t ObCtxTxData::get_commit_version() const
{
  RLockGuard guard(lock_);
  return (NULL != tx_data_ ? ATOMIC_LOAD(&tx_data_->commit_version_) : ATOMIC_LOAD(&tx_commit_data_.commit_version_));
}

int64_t ObCtxTxData::get_start_log_ts() const
{
  RLockGuard guard(lock_);
  int64_t ctx_log_ts = (NULL != tx_data_ ? ATOMIC_LOAD(&tx_data_->start_log_ts_) : ATOMIC_LOAD(&tx_commit_data_.start_log_ts_));
  if (INT64_MAX == ctx_log_ts) {
    ctx_log_ts = OB_INVALID_TIMESTAMP;
  }
  return ctx_log_ts;
}

int64_t ObCtxTxData::get_end_log_ts() const
{
  RLockGuard guard(lock_);
  int64_t ctx_log_ts = (NULL != tx_data_ ? ATOMIC_LOAD(&tx_data_->end_log_ts_) : ATOMIC_LOAD(&tx_commit_data_.end_log_ts_));
  if (INT64_MAX == ctx_log_ts) {
    ctx_log_ts = OB_INVALID_TIMESTAMP;
  }
  return ctx_log_ts;
}

ObTransID ObCtxTxData::get_tx_id() const
{
  RLockGuard guard(lock_);
  return (NULL != tx_data_ ? tx_data_->tx_id_ : tx_commit_data_.tx_id_);
}

int ObCtxTxData::prepare_add_undo_action(ObUndoAction &undo_action,
                                         storage::ObTxData *&tmp_tx_data,
                                         storage::ObUndoStatusNode *&tmp_undo_status)
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);
  /*
   * alloc undo_status_node used on commit stage
   * alloc tx_data and add undo_action to it, which will be inserted
   *       into tx_data_table after RollbackSavepoint log sync success
   */
  if (OB_FAIL(check_tx_data_writable_())) {
    TRANS_LOG(WARN, "tx data is not writeable", K(ret), K(*this));
  } else {
    ObTxTable *tx_table = nullptr;
    GET_TX_TABLE_(tx_table);
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(tx_table->get_tx_data_table()->alloc_undo_status_node(tmp_undo_status))) {
      TRANS_LOG(WARN, "alloc undo status fail", K(ret), KPC(this));
    } else if (OB_ISNULL(tmp_undo_status)) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(ERROR, "undo status is null", KR(ret), KPC(this));
    } else if (OB_FAIL(tx_table->deep_copy_tx_data(tx_data_, tmp_tx_data))) {
      TRANS_LOG(WARN, "copy tx data fail", K(ret), KPC(this));
    } else if (OB_ISNULL(tmp_tx_data)) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(ERROR, "copied tx_data is null", KR(ret), KPC(this));
    } else if (OB_FAIL(tmp_tx_data->add_undo_action(tx_table, undo_action))) {
      TRANS_LOG(WARN, "add undo action fail", K(ret), KPC(this));
    }

    if (OB_FAIL(ret)) {
      if (tmp_undo_status) {
        tx_table->get_tx_data_table()->free_undo_status_node(tmp_undo_status);
      }
      if (tmp_tx_data) {
        tx_table->free_tx_data(tmp_tx_data);
      }
    }
  }
  return ret;
}

int ObCtxTxData::cancel_add_undo_action(storage::ObTxData *tmp_tx_data, storage::ObUndoStatusNode *tmp_undo_status)
{
  int ret = OB_SUCCESS;
  ObTxTable *tx_table = nullptr;
  GET_TX_TABLE_(tx_table);
  if (OB_SUCC(ret)) {
    tx_table->free_tx_data(tmp_tx_data);
    ret = tx_table->get_tx_data_table()->free_undo_status_node(tmp_undo_status);
  }
  return ret;
}

int ObCtxTxData::commit_add_undo_action(ObUndoAction &undo_action, storage::ObUndoStatusNode &tmp_undo_status)
{
  return add_undo_action(undo_action, &tmp_undo_status);
}

int ObCtxTxData::add_undo_action(ObUndoAction &undo_action, storage::ObUndoStatusNode *tmp_undo_status)
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);

  if (OB_FAIL(check_tx_data_writable_())) {
    TRANS_LOG(WARN, "tx data is not writeable", K(ret), K(*this));
  } else {
    ObTxTable *tx_table = nullptr;
    GET_TX_TABLE_(tx_table);
    if (OB_FAIL(ret)) {
      // do nothing
    } else if (OB_FAIL(tx_data_->add_undo_action(tx_table, undo_action, tmp_undo_status))) {
      TRANS_LOG(WARN, "add undo action failed", K(ret), K(undo_action), KP(tmp_undo_status), K(*this));
    };
  }

  return ret;
}

int ObCtxTxData::Guard::get_tx_data(const ObTxData *&tx_data) const
{
  int ret = OB_SUCCESS;
  auto tmp_tx_data = host_.tx_data_;
  if (NULL == tmp_tx_data) {
    ret = OB_TRANS_CTX_NOT_EXIST;
  } else {
    tx_data = tmp_tx_data;
  }
  return ret;
}

int ObCtxTxData::get_tx_commit_data(const ObTxCommitData *&tx_commit_data) const
{
  int ret = OB_SUCCESS;
  tx_commit_data = &tx_commit_data_;
  return ret;
}

int ObCtxTxData::check_tx_data_writable_()
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(tx_data_)) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "tx_data_ is not valid", K(this));
  } else if (tx_data_->is_in_tx_data_table_) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "tx_data has inserted into tx table", K(ret), K(this));
  } else if (read_only_) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "try to write a read-only tx_data", K(ret), K(this));
  }
  return ret;
}

int ObCtxTxData::insert_tx_data_(ObTxTable *tx_table, ObTxData *&tx_data)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(tx_table)) {
    ret = OB_INVALID_ARGUMENT;
    TRANS_LOG(WARN, "invalid argument", K(ret), KP(tx_table));
  } else if (OB_ISNULL(tx_data)) {
    TRANS_LOG(INFO, "tx_data is nullptr, no need to insert", KP(tx_data), K(*this));
    // no need to insert, do nothing
  } else if (OB_FAIL(tx_table->insert(tx_data))) {
    TRANS_LOG(WARN, "insert into tx_table failed", K(ret), KPC(tx_data));
  } else {
    tx_data = nullptr;
  }

  return ret;
}

int ObCtxTxData::free_tx_data_(ObTxTable *tx_table, ObTxData *&tx_data)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(tx_table)) {
    ret = OB_INVALID_ARGUMENT;
    TRANS_LOG(WARN, "invalid argument", K(ret), KP(tx_table));
  } else if (OB_ISNULL(tx_data)) {
    TRANS_LOG(INFO, "tx_data is nullptr, no need to free", KP(tx_data), K(*this));
    // no need to free, do nothing
  } else {
    tx_table->free_tx_data(tx_data);
    tx_data = nullptr;
  }
  return ret;
}

int ObCtxTxData::deep_copy_tx_data_(ObTxTable *tx_table, storage::ObTxData *&tx_data)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(tx_table)) {
    ret = OB_INVALID_ARGUMENT;
    TRANS_LOG(WARN, "invalid argument", K(ret), KP(tx_table));
  } else if (OB_ISNULL(tx_data_)) {
    TRANS_LOG(INFO, "tx_data_ is nullptr, no need to deep copy tx data", K(*this));
    // no need to free, do nothing
  } else if (OB_FAIL(tx_table->deep_copy_tx_data(tx_data_, tx_data))) {
    TRANS_LOG(WARN, "deep copy tx data failed", K(ret), KPC(tx_data), K(*this));
  }
  return ret;
}

} // namespace transaction

} // namespace oceanbase
