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

#ifndef OCEANBASE_TRANSACTION_OB_TX_FUNCTOR
#define OCEANBASE_TRANSACTION_OB_TX_FUNCTOR

#include "common/ob_simple_iterator.h"
#include "ob_trans_ctx.h"
#include "ob_trans_part_ctx.h"
#include "ob_trans_stat.h"

#include "storage/tx/ob_ls_tx_ctx_mgr_stat.h"
#include "ob_trans_version_mgr.h"
#include "storage/blocksstable/ob_macro_block_writer.h"
#include "share/ob_force_print_log.h"
#include "storage/tx/ob_trans_ctx_mgr.h"
#include "sql/session/ob_sql_session_info.h"
#include "storage/tablelock/ob_table_lock_common.h"
#include "storage/tx_table/ob_tx_table.h"
#include "storage/tx_table/ob_tx_table_define.h"
#include "storage/tx/ob_tx_stat.h"
#include "storage/tx/ob_trans_service.h"
#include "storage/tx/ob_keep_alive_ls_handler.h"

namespace oceanbase
{

namespace transaction
{
class ObTransCtx;

class TxFunctorStat
{
public:
  TxFunctorStat()
      : single_begin_time_(0), iter_cnt_(0), begin_time_(0), finish_time_(0),
        single_expired_limit_(INT64_MAX), single_expired_cnt_(0), total_expired_limit_(INT64_MAX)
  {}
  void reset()
  {
    single_begin_time_ = 0;
    iter_cnt_ = 0;
    begin_time_ = 0;
    finish_time_ = 0;
    single_expired_limit_ = INT64_MAX;
    single_expired_cnt_ = 0;
    total_expired_limit_ = INT64_MAX;
  }
  void set_expired_limit(int64_t single_limit, int64_t total_limit)
  {
    single_expired_limit_ = single_limit;
    total_expired_limit_ = total_limit;
  }

  void begin_iter_single()
  {
    int64_t cur_time = -1;
    if (INT64_MAX != single_expired_limit_) {
      cur_time = ObTimeUtility::fast_current_time();
    }

    if (0 == iter_cnt_) {
      if (-1 == cur_time) {
        cur_time = ObTimeUtility::fast_current_time();
      }
      begin_time_ = cur_time;
    }
    single_begin_time_ = cur_time;
    iter_cnt_++;
  }

  void finish_iter_single(const char *func_name, const ObTransID &tx_id, const share::ObLSID &ls_id)
  {
    functor_name_ = func_name;
    ls_id_ = ls_id;
    if (INT64_MAX != single_expired_limit_) {
      int64_t cur_time = ObTimeUtility::fast_current_time();
      if (cur_time - single_begin_time_ > single_expired_limit_) {
        single_expired_cnt_++;
        TRANS_LOG(INFO, "single tx cost too much time", K_(functor_name), K(tx_id), K(ls_id),
                  "cost_time", cur_time - single_begin_time_, K(single_begin_time_));
      }
    }
  }
  void print_stat_(bool force_print = false)
  {
    if (finish_time_ == 0) {
      finish_time_ = ObTimeUtility::fast_current_time();
    }
    if (iter_cnt_ > 0) {
      if (force_print) {
        TRANS_LOG(INFO, "ls trans functor stat", K_(functor_name), K_(ls_id), KPC(this));
      } else if (total_expired_limit_ != INT_MAX
                 && finish_time_ - begin_time_ >= total_expired_limit_) {
        TRANS_LOG(INFO, "ls trans functor stat", K_(functor_name), K_(ls_id), KPC(this));
      }
    }
  }

public:
  TO_STRING_KV(K(iter_cnt_),
               K(begin_time_),
               K(finish_time_),
               "total_cost_time",
               finish_time_ - begin_time_,
               K(single_expired_limit_),
               K(single_expired_cnt_),
               K(total_expired_limit_));

private:
  const char *functor_name_;
  share::ObLSID ls_id_;

  int64_t single_begin_time_;

  int64_t iter_cnt_;
  int64_t begin_time_;
  int64_t finish_time_;
  int64_t single_expired_limit_;
  int64_t single_expired_cnt_;
  int64_t total_expired_limit_;
};

// XXX TMP_CODE
// In the future, ObTransCtx will no longer be stored in the hashmap, but ObPartTransCtx directly;
// there are too many changes in this commit, so they will be processed in the next commit; TODO senchen;
#define OPERATOR_V4(FUNC_NAME) \
  private: \
  TxFunctorStat func_stat_; \
  public: \
  bool operator()(ObTransCtx *tx_ctx_base) { \
    bool bool_ret = false; \
    ObPartTransCtx *tx_ctx = dynamic_cast<transaction::ObPartTransCtx*>(tx_ctx_base); \
    ObTransID tx_id = tx_ctx->get_trans_id(); \
    share::ObLSID ls_id = tx_ctx->get_ls_id(); \
    func_stat_.begin_iter_single(); \
    bool_ret = internal_operator(tx_id, tx_ctx); \
    func_stat_.finish_iter_single(#FUNC_NAME, tx_id, ls_id);\
    return bool_ret; \
  }; \
  bool internal_operator(const ObTransID &tx_id, ObPartTransCtx *tx_ctx)

#define SET_EXPIRED_LIMIT(SINGLE_LIMIT, TOTAL_LIMIT) \
  func_stat_.set_expired_limit(SINGLE_LIMIT, TOTAL_LIMIT);

#define PRINT_FUNC_STAT func_stat_.print_stat_();
#define FORCE_PRINT_FUNC_STAT func_stat_.print_stat_(true);

class SwitchToFollowerForcedlyFunctor
{
public:
  SwitchToFollowerForcedlyFunctor(ObIArray<ObTxCommitCallback> &cb_array) : cb_array_(cb_array)
  {
    SET_EXPIRED_LIMIT(100 * 1000 /*100ms*/, 3 * 1000 * 1000 /*3s*/)
  }
  ~SwitchToFollowerForcedlyFunctor() { PRINT_FUNC_STAT; }
  OPERATOR_V4(SwitchToFollowerForcedlyFunctor)
  {
    int tmp_ret = common::OB_SUCCESS;

    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      TRANS_LOG(WARN, "invalid argument", K(tx_id), "ctx", OB_P(tx_ctx));
      tmp_ret = common::OB_INVALID_ARGUMENT;
    } else if (common::OB_SUCCESS != (tmp_ret = tx_ctx->switch_to_follower_forcedly(cb_array_))) {
      TRANS_LOG(ERROR, "leader revoke failed", K(tx_id), K(*tx_ctx));
    }

    return true;
  }

private:
  ObIArray<ObTxCommitCallback> &cb_array_;
};

class SwitchToLeaderFunctor
{
public:
  explicit SwitchToLeaderFunctor(int64_t start_working_ts)
  {
    start_working_ts_ = start_working_ts;

    SET_EXPIRED_LIMIT(100 * 1000 /*100ms*/, 3 * 1000 * 1000 /*3s*/);
  }
  ~SwitchToLeaderFunctor() { PRINT_FUNC_STAT; }
  OPERATOR_V4(SwitchToLeaderFunctor)
  {
    bool bool_ret = false;
    int tmp_ret = common::OB_SUCCESS;

    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      TRANS_LOG(WARN, "invalid argument", K(tx_id), "ctx", OB_P(tx_ctx));
    } else if (OB_TMP_FAIL(tx_ctx->switch_to_leader(start_working_ts_))) {
      TRANS_LOG(WARN, "switch_to_leader error", "ret", tmp_ret, K(*tx_ctx));
    } else {
      bool_ret = true;
    }
    return bool_ret;
  }

private:
  int64_t start_working_ts_;
};

class SwitchToFollowerGracefullyFunctor
{
public:
  SwitchToFollowerGracefullyFunctor(const int64_t abs_expired_time,
                                    ObIArray<ObTxCommitCallback> &cb_array)
      : abs_expired_time_(abs_expired_time), count_(0), ret_(OB_SUCCESS), cb_array_(cb_array)
  {
    SET_EXPIRED_LIMIT(100 * 1000 /*100ms*/, 3 * 1000 * 1000 /*3s*/);
  }
  ~SwitchToFollowerGracefullyFunctor() { PRINT_FUNC_STAT; }
  OPERATOR_V4(SwitchToFollowerGracefullyFunctor)
  {
    bool bool_ret = false;
    int ret = OB_SUCCESS;
    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      ret_ = ret = OB_INVALID_ARGUMENT;
      TRANS_LOG(WARN, "invalid argument", K(tx_id), "ctx", OB_P(tx_ctx));
    } else {
      ++count_;
      if ((count_ % BATCH_CHECK_COUNT) == 0) {
        const int64_t now = ObTimeUtility::current_time();
        if (now >= abs_expired_time_) {
          ret_ = ret = OB_TIMEOUT;
          TRANS_LOG(WARN, "switch to follower gracefully timeout");
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(tx_ctx->switch_to_follower_gracefully(cb_array_))) {
        TRANS_LOG(WARN, "switch to follower gracefully failed", KR(ret), K(*tx_ctx));
        ret_ = ret;
      } else {
        bool_ret = true;
      }
    }
    return bool_ret;
  }
  int get_ret() const { return ret_; }
  int64_t get_count() const { return count_; }

private:
  static const int64_t BATCH_CHECK_COUNT = 100;
  int64_t abs_expired_time_;
  int64_t count_;
  int ret_;
  ObIArray<ObTxCommitCallback> &cb_array_;
};

class ResumeLeaderFunctor
{
public:
  ResumeLeaderFunctor(int64_t start_working_ts)
  {
    start_working_ts_ = start_working_ts;

    SET_EXPIRED_LIMIT(100 * 1000 /*100ms*/, 3 * 1000 * 1000 /*3s*/);
  }
  ~ResumeLeaderFunctor() { PRINT_FUNC_STAT; }
  OPERATOR_V4(ResumeLeaderFunctor)
  {
    bool bool_ret = false;
    int ret = OB_SUCCESS;
    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      TRANS_LOG(WARN, "invalid argument", K(tx_id), "ctx", OB_P(tx_ctx));
    } else if (OB_FAIL(tx_ctx->resume_leader(start_working_ts_))) {
      TRANS_LOG(WARN, "resume leader failed", KR(ret), K(*tx_ctx));
    } else {
      bool_ret = true;
    }
    return bool_ret;
  }

private:
  int64_t start_working_ts_;
};

class ReplayTxStartWorkingLogFunctor
{
public:
  ReplayTxStartWorkingLogFunctor(int64_t start_working_ts)
  {
    start_working_ts_ = start_working_ts;
    SET_EXPIRED_LIMIT(100 * 1000 /*100ms*/, 3 * 1000 * 1000 /*3s*/);
  }
  ~ReplayTxStartWorkingLogFunctor() { PRINT_FUNC_STAT; }
  OPERATOR_V4(ReplayTxStartWorkingLogFunctor)
  {
    int ret = OB_SUCCESS;
    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      ret = common::OB_INVALID_ARGUMENT;
      TRANS_LOG(WARN, "invalid argument", KR(ret), K(tx_id), "ctx", OB_P(tx_ctx));
    } else {
      if (OB_FAIL(tx_ctx->replay_start_working_log(start_working_ts_))) {
        TRANS_LOG(WARN, "replay start working log error", KR(ret), K(tx_id));
      }
    }
    return true;
  }

private:
  int64_t start_working_ts_;
};

class KillTxCtxFunctor
{
public:
  KillTxCtxFunctor(const KillTransArg &arg, ObIArray<ObTxCommitCallback> &cb_array)
      : arg_(arg), release_audit_mgr_lock_(false), cb_array(cb_array)
  {

    SET_EXPIRED_LIMIT(100 * 1000 /*100ms*/, 3 * 1000 * 1000 /*3s*/);
  }
  ~KillTxCtxFunctor() { PRINT_FUNC_STAT; }
  void set_release_audit_mgr_lock(const bool release_audit_mgr_lock)
  {
    release_audit_mgr_lock_ = release_audit_mgr_lock;
  }
  OPERATOR_V4(KillTxCtxFunctor)
  {
    int ret = OB_SUCCESS;
    int tmp_ret = common::OB_SUCCESS;

    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      TRANS_LOG(WARN, "invalid argument", K(tx_id), "ctx", OB_P(tx_ctx));
      tmp_ret = common::OB_INVALID_ARGUMENT;
    } else {
      if (OB_SUCC(tx_ctx->kill(arg_, cb_array))) {
        TRANS_LOG(INFO, "kill transaction success", K(tx_id), K_(arg));
      } else if (common::OB_TRANS_CANNOT_BE_KILLED == ret) {
        TRANS_LOG(INFO, "transaction can not be killed", K(tx_id), "context", *tx_ctx);
      } else {
        TRANS_LOG(WARN, "kill transaction error", "ret", ret, K(tx_id), "context", *tx_ctx);
      }
    }

    return OB_SUCCESS == ret;
  }

private:
  KillTransArg arg_;
  bool release_audit_mgr_lock_;
  ObIArray<ObTxCommitCallback> &cb_array;
};

class StopLSFunctor
{
public:
  StopLSFunctor() {}
  ~StopLSFunctor() {}
  bool operator()(const share::ObLSID &ls_id, ObLSTxCtxMgr *ls_tx_ctx_mgr)
  {
    int tmp_ret = common::OB_SUCCESS;
    bool bool_ret = false;
    const bool graceful = false;

    if (!ls_id.is_valid() || OB_ISNULL(ls_tx_ctx_mgr)) {
      TRANS_LOG(WARN, "invalid argument", K(ls_id), KP(ls_tx_ctx_mgr));
      tmp_ret = OB_INVALID_ARGUMENT;
    } else if (OB_TMP_FAIL(ls_tx_ctx_mgr->stop(graceful))) {
      TRANS_LOG(WARN, "ObLSTxCtxMgr stop error", K(tmp_ret), K(ls_id));
    } else {
      bool_ret = true;
    }

    return bool_ret;
  }
};

class WaitLSFunctor
{
public:
  explicit WaitLSFunctor(int64_t &retry_count) : retry_count_(retry_count) {}
  ~WaitLSFunctor() {}
  bool operator()(const share::ObLSID &ls_id, ObLSTxCtxMgr *ls_tx_ctx_mgr)
  {
    int tmp_ret = common::OB_SUCCESS;
    bool bool_ret = true;

    if (!ls_id.is_valid() || OB_ISNULL(ls_tx_ctx_mgr)) {
      TRANS_LOG(WARN, "invalid argument", K(ls_id), KP(ls_tx_ctx_mgr));
      tmp_ret = OB_INVALID_ARGUMENT;
    } else if (!ls_tx_ctx_mgr->is_stopped()) {
      TRANS_LOG(WARN, "ls_id has not been stopped", K(ls_id));
      tmp_ret = OB_PARTITION_IS_NOT_STOPPED;
    } else if (ls_tx_ctx_mgr->get_tx_ctx_count() > 0) {
      // if there are unfinished transactions at the ls_id,
      // increase retry_count by 1
      ++retry_count_;
    } else {
      // do nothing
    }

    if (common::OB_SUCCESS != tmp_ret) {
      bool_ret = false;
    }
    return bool_ret;
  }

private:
  int64_t &retry_count_;
};

class RemoveLSFunctor
{
public:
  RemoveLSFunctor() {}
  ~RemoveLSFunctor() {}
  bool operator()(const share::ObLSID &ls_id, ObLSTxCtxMgr *ls_tx_ctx_mgr)
  {
    int tmp_ret = common::OB_SUCCESS;
    bool bool_ret = false;

    if (!ls_id.is_valid() || OB_ISNULL(ls_tx_ctx_mgr)) {
      TRANS_LOG(WARN, "invalid argument", K(ls_id), KP(ls_tx_ctx_mgr));
      tmp_ret = OB_INVALID_ARGUMENT;
    } else if (!ls_tx_ctx_mgr->is_stopped()) {
      TRANS_LOG(WARN, "ls_tx_ctx_mgr has not been stopped", K(ls_id));
      tmp_ret = OB_PARTITION_IS_NOT_STOPPED;
    } else {
      // Release all ctx memory on the ls_id
      ls_tx_ctx_mgr->destroy();
      ls_tx_ctx_mgr = NULL;
      bool_ret = true;
    }
    UNUSED(tmp_ret);
    return bool_ret;
  }
};

class IterateLSIDFunctor
{
public:
  explicit IterateLSIDFunctor(ObLSIDIterator &ls_id_iter) : ls_id_iter_(ls_id_iter) {}
  bool operator()(const share::ObLSID &ls_id, ObLSTxCtxMgr *ls_tx_ctx_mgr)
  {
    int tmp_ret = common::OB_SUCCESS;
    bool bool_ret = false;

    if (!ls_id.is_valid() || OB_ISNULL(ls_tx_ctx_mgr)) {
      TRANS_LOG(WARN, "invalid argument", K(ls_id), KP(ls_tx_ctx_mgr));
      tmp_ret = OB_INVALID_ARGUMENT;
    } else if (OB_TMP_FAIL(ls_id_iter_.push(ls_id))) {
      TRANS_LOG(WARN, "ObLSIDIterator push ls_id error", K(tmp_ret), K(ls_id));
    } else {
      bool_ret = true;
    }
    return bool_ret;
  }
private:
  ObLSIDIterator &ls_id_iter_;
};

class IterateLSTxCtxMgrStatFunctor
{
public:
  IterateLSTxCtxMgrStatFunctor(const ObAddr &addr, ObTxCtxMgrStatIterator &tx_ctx_mgr_stat_iter)
      : tx_ctx_mgr_stat_iter_(tx_ctx_mgr_stat_iter), addr_(addr) {}
  bool operator()(const share::ObLSID &ls_id, ObLSTxCtxMgr *ls_tx_ctx_mgr)
  {
    int tmp_ret = common::OB_SUCCESS;
    bool bool_ret = false;
    ObLSTxCtxMgrStat ls_tx_ctx_mgr_stat;

    if (!ls_id.is_valid() || OB_ISNULL(ls_tx_ctx_mgr)) {
      TRANS_LOG(WARN, "invalid argument", K(ls_id), KP(ls_tx_ctx_mgr));
      tmp_ret = OB_INVALID_ARGUMENT;
    } else {
      uint64_t mgr_state = ls_tx_ctx_mgr->get_state_();
      tmp_ret = ls_tx_ctx_mgr_stat.init(addr_,
                                        ls_tx_ctx_mgr->ls_id_,
                                        ls_tx_ctx_mgr->is_master_(mgr_state),
                                        ls_tx_ctx_mgr->is_stopped_(mgr_state),
                                        mgr_state,
                                        ObLSTxCtxMgr::State::state_str(mgr_state),
                                        ls_tx_ctx_mgr->total_tx_ctx_count_,
                                        (int64_t)(&(*ls_tx_ctx_mgr)));
      if (OB_SUCCESS != tmp_ret) {
        TRANS_LOG(WARN, "ObLSTxCtxMgrStat init error", K_(addr), "ls_tx_ctx_mgr", *ls_tx_ctx_mgr);
      } else if (OB_TMP_FAIL(tx_ctx_mgr_stat_iter_.push(ls_tx_ctx_mgr_stat))) {
        TRANS_LOG(WARN, "ObTxCtxMgrStatIterator push error",
            K(tmp_ret), K(ls_id), "ls_tx_ctx_mgr", *ls_tx_ctx_mgr);
      } else {
        bool_ret = true;
      }
    }
    return bool_ret;
  }
private:
  ObTxCtxMgrStatIterator &tx_ctx_mgr_stat_iter_;
  const ObAddr &addr_;
};

class IterateCheckTabletModifySchema
{
public:
  explicit IterateCheckTabletModifySchema(const common::ObTabletID &tablet_id,
                                          const int64_t schema_version)
    : block_tx_id_(),
      tablet_id_(tablet_id),
      schema_version_(schema_version),
      ret_code_(common::OB_SUCCESS) {}
  OPERATOR_V4(IterateCheckTabletModifySchema)
  {
    int ret = common::OB_SUCCESS;
    bool bool_ret = false;

    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      TRANS_LOG(WARN, "invalid argument", K(tx_id), "ctx", OB_P(tx_ctx));
      ret = OB_INVALID_ARGUMENT;
    } else {
      if (OB_FAIL(tx_ctx->check_modify_schema_elapsed(tablet_id_,
                                                      schema_version_))) {
        if (OB_EAGAIN != ret) {
          TRANS_LOG(WARN, "tx_ctx check_modify_schema_elapsed error", K(ret),
                    "ctx", *tx_ctx);
        }
      } else {
        bool_ret = true;
      }
    }
    if (OB_FAIL(ret)) {
      ret_code_ = ret;
    }
    if (!bool_ret) {
      block_tx_id_ = tx_id;
    }
    return bool_ret;
  }
  int get_ret_code() const { return ret_code_; }
  ObTransID get_tx_id() const { return block_tx_id_; }
private:
  ObTransID block_tx_id_;
  common::ObTabletID tablet_id_;
  int64_t schema_version_;
  int ret_code_;
};

class IterateCheckTabletModifyTimestamp
{
public:
  explicit IterateCheckTabletModifyTimestamp(const common::ObTabletID &tablet_id,
                                             const int64_t timestamp)
    : block_tx_id_(),
      tablet_id_(tablet_id),
      check_ts_(timestamp),
      ret_code_(common::OB_SUCCESS) {}
  OPERATOR_V4(IterateCheckTabletModifyTimestamp)
  {
    int ret = common::OB_SUCCESS;
    bool bool_ret = false;

    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      TRANS_LOG(WARN, "invalid argument", K(tx_id), "ctx", OB_P(tx_ctx));
      ret = OB_INVALID_ARGUMENT;
    } else {
      if (OB_FAIL(tx_ctx->check_modify_time_elapsed(tablet_id_,
                                                    check_ts_))) {
        if (OB_EAGAIN != ret) {
          TRANS_LOG(WARN, "tx_ctx check_modify_time_elapsed error", K(ret), "ctx", *tx_ctx);
        }
      } else {
        bool_ret = true;
      }
    }
    if (OB_FAIL(ret)) {
      ret_code_ = ret;
    }
    if (!bool_ret) {
      block_tx_id_ = tx_id;
    }
    return bool_ret;
  }
  int get_ret_code() const { return ret_code_; }
  ObTransID get_tx_id() const { return block_tx_id_; }
private:
  ObTransID block_tx_id_;
  common::ObTabletID tablet_id_;
  int64_t check_ts_;
  int ret_code_;
};

class IterateMinPrepareVersionFunctor
{
public:
  explicit IterateMinPrepareVersionFunctor() : min_prepare_version_(INT64_MAX) {}
  int64_t get_min_prepare_version() const { return min_prepare_version_; }
  OPERATOR_V4(IterateMinPrepareVersionFunctor)
  {
    int tmp_ret = common::OB_SUCCESS;

    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      tmp_ret = OB_INVALID_ARGUMENT;
      TRANS_LOG(WARN, "invalid argument", K(tmp_ret), K(tx_id), "ctx", OB_P(tx_ctx));
    } else {
      bool is_prepared = false;
      int64_t prepare_version = 0;
      if (OB_TMP_FAIL(tx_ctx->get_prepare_version_if_prepared(is_prepared, prepare_version))) {
        TRANS_LOG(WARN, "get prepare version if prepared failed", K(tmp_ret), K(*tx_ctx));
      } else if (!is_prepared || prepare_version >= min_prepare_version_) {
        // do nothing
      } else {
        min_prepare_version_ = prepare_version;
      }
    }
    return (OB_SUCCESS == tmp_ret);
  }
private:
  int64_t min_prepare_version_;
};

class ObGetMinUndecidedLogTsFunctor
{
public:
  ObGetMinUndecidedLogTsFunctor() : log_ts_(INT64_MAX) {}
  ~ObGetMinUndecidedLogTsFunctor() {}
  int64_t get_min_undecided_log_ts() const { return log_ts_; }
  OPERATOR_V4(ObGetMinUndecidedLogTsFunctor)
  {
    int ret = OB_SUCCESS;
    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      ret = OB_INVALID_ARGUMENT;
      TRANS_LOG(WARN, "invalid argument", K(tx_id), "ctx", OB_P(tx_ctx));
    } else {
      int64_t log_ts = tx_ctx->get_min_undecided_log_ts();
      if (log_ts_ > log_ts) {
        log_ts_ = log_ts;
      }
    }
    return OB_SUCC(ret);
  }
private:
  int64_t log_ts_;
};

class IterateAllLSTxStatFunctor
{
public:
  explicit IterateAllLSTxStatFunctor(ObTxStatIterator &tx_stat_iter): tx_stat_iter_(tx_stat_iter) {}
  bool operator()(const share::ObLSID &ls_id, ObLSTxCtxMgr *ls_tx_ctx_mgr)
  {
    int tmp_ret = common::OB_SUCCESS;
    bool bool_ret = false;

    if (!ls_id.is_valid() || OB_ISNULL(ls_tx_ctx_mgr)) {
      TRANS_LOG(WARN, "invalid argument", K(ls_id), KP(ls_tx_ctx_mgr));
      tmp_ret = OB_INVALID_ARGUMENT;
    } else if (OB_TMP_FAIL(ls_tx_ctx_mgr->iterate_tx_ctx_stat(tx_stat_iter_))) {
      TRANS_LOG(WARN, "iterate_tx_ctx_stat error", K(tmp_ret), K(ls_id));
    } else {
      bool_ret = true;
    }
    return bool_ret;
  }
private:
  ObTxStatIterator &tx_stat_iter_;
};

class IteratorTxIDFunctor
{
public:
  explicit IteratorTxIDFunctor(ObTxIDIterator &tx_id_iter) : tx_id_iter_(tx_id_iter) {}
  OPERATOR_V4(IteratorTxIDFunctor)
  {
    int tmp_ret = OB_SUCCESS;
    bool bool_ret = false;

    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      TRANS_LOG(WARN, "invalid argument", K(tx_id), "ctx", OB_P(tx_ctx));
      tmp_ret = OB_INVALID_ARGUMENT;
      // If you encounter a situation where tx_ctx has not been init yet,
      // skip it directly, there will be a background thread retry
    } else if (!tx_ctx->is_inited()) {
      // not inited, don't need to traverse
    } else {
      tx_id_iter_.push(tx_id);
    }
    if (OB_SUCCESS == tmp_ret) {
      bool_ret = true;
    }
    return bool_ret;
  }
private:
  ObTxIDIterator &tx_id_iter_;
};

class IterateTxStatFunctor
{
public:
  explicit IterateTxStatFunctor(ObTxStatIterator &tx_stat_iter) : tx_stat_iter_(tx_stat_iter) {}
  OPERATOR_V4(IterateTxStatFunctor)
  {
    int tmp_ret = common::OB_SUCCESS;
    bool bool_ret = false;

    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      TRANS_LOG(WARN, "invalid argument", K(tx_id), "ctx", OB_P(tx_ctx));
      tmp_ret = OB_INVALID_ARGUMENT;
      // If you encounter a situation where tx_ctx has not been init yet,
      // skip it directly, there will be a background thread retry
    } else if (!tx_ctx->is_inited()) {
      // not inited, don't need to traverse
    } else {
      ObTxStat tx_stat;
      // Judge whether the transaction has been decided by state
      bool has_decided = false;
      if (ObTxState::INIT < tx_ctx->exec_info_.state_) {
        has_decided = true;
      }
      if (tx_ctx->is_too_long_transaction()) {
        // If the transaction has not completed in 600 seconds, print its trace log
        tx_ctx->print_trace_log();
      }
      if (OB_SUCCESS == tmp_ret) {
        share::ObLSArray participants_arr;
        if (OB_TMP_FAIL(tx_ctx->get_2pc_participants_copy(participants_arr))) {
          TRANS_LOG(WARN, "ObTxStat get participants copy error", K(tmp_ret));
        } else if (OB_TMP_FAIL(tx_stat.init(tx_ctx->addr_,
                                            tx_id,
                                            tx_ctx->tenant_id_,
                                            has_decided,
                                            tx_ctx->ls_id_,
                                            participants_arr,
                                            tx_ctx->ctx_create_time_,
                                            tx_ctx->trans_expired_time_,
                                            tx_ctx->ref_,
                                            tx_ctx->last_op_sn_,
                                            tx_ctx->pending_write_,
                                            (int64_t)tx_ctx->exec_info_.state_,
                                            tx_ctx->exec_info_.trans_type_,
                                            tx_ctx->part_trans_action_,
                                            tx_ctx,
                                            tx_ctx->get_pending_log_size(),
                                            tx_ctx->get_flushed_log_size(),
                                            tx_ctx->role_state_,
                                            tx_ctx->session_id_,
                                            tx_ctx->exec_info_.scheduler_,
                                            tx_ctx->is_exiting_))) {
          TRANS_LOG(WARN, "ObTxStat init error", K(tmp_ret), KPC(tx_ctx));
        } else if (OB_TMP_FAIL(tx_stat_iter_.push(tx_stat))) {
          TRANS_LOG(WARN, "ObTxStatIterator push trans stat error", K(tmp_ret));
        } else {
          // do nothing
        }
      }
    }
    if (OB_SUCCESS == tmp_ret) {
      bool_ret = true;
    }

    return bool_ret;
  }
private:
  ObTxStatIterator &tx_stat_iter_;
};

class GetRecLogTSFunctor
{
public:
  explicit GetRecLogTSFunctor() : rec_log_ts_(INT64_MAX) {}
  int init()
  {
    rec_log_ts_ = INT64_MAX;
    return OB_SUCCESS;
  }
  OPERATOR_V4(GetRecLogTSFunctor)
  {
    int ret = common::OB_SUCCESS;
    bool bool_ret = false;

    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      TRANS_LOG(WARN, "invalid argument", K(tx_id), "ctx", OB_P(tx_ctx));
      ret = OB_INVALID_ARGUMENT;
    } else {
      ObTxCtxTableInfo ctx_info;
      rec_log_ts_ = MIN(rec_log_ts_, tx_ctx->get_rec_log_ts());
    }
    if (OB_SUCCESS == ret) {
      bool_ret = true;
    }
    return bool_ret;
  }
  int64_t get_rec_log_ts() { return rec_log_ts_; }
private:
  int64_t rec_log_ts_;
};

class OnTxCtxTableFlushedFunctor
{
public:
  explicit OnTxCtxTableFlushedFunctor() {}
  int init() { return OB_SUCCESS; }
  OPERATOR_V4(OnTxCtxTableFlushedFunctor)
  {
    int ret = common::OB_SUCCESS;
    bool bool_ret = false;

    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      TRANS_LOG(WARN, "invalid argument", K(tx_id), "ctx", OB_P(tx_ctx));
      ret = OB_INVALID_ARGUMENT;
    } else {
      if (OB_FAIL(tx_ctx->on_tx_ctx_table_flushed())) {
        TRANS_LOG(WARN, "fail to callback flushed", K(ret));
      }
    }
    if (OB_SUCCESS == ret) {
      bool_ret = true;
    }

    return bool_ret;
  }
};

class IterateTxObjLockOpFunctor
{
public:
  explicit IterateTxObjLockOpFunctor(tablelock::ObLockOpIterator &iter)
    : iter_(iter) {}
  OPERATOR_V4(IterateTxObjLockOpFunctor)
  {
    int ret = OB_SUCCESS;
    bool bool_ret = false;

    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      ret = OB_INVALID_ARGUMENT;
      TRANS_LOG(WARN, "invalid argument", KR(ret), K(tx_id), "ctx", OB_P(tx_ctx));
    } else if (OB_FAIL(tx_ctx->iterate_tx_obj_lock_op(iter_))) {
      TRANS_LOG(WARN, "iterate tx obj lock op fail", KR(ret), K(tx_id));
    } else {
      // do nothing
    }

    if (OB_SUCCESS == ret) {
      bool_ret = true;
    }

    return bool_ret;
  }

private:
  ObLockOpIterator &iter_;
};

class IterateTxLockStatFunctor
{
public:
  explicit IterateTxLockStatFunctor(ObTxLockStatIterator &tx_lock_stat_iter)
    : tx_lock_stat_iter_(tx_lock_stat_iter) {}
  OPERATOR_V4(IterateTxLockStatFunctor)
  {
    int ret = OB_SUCCESS;
    bool bool_ret = false;

    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      ret = OB_INVALID_ARGUMENT;
      TRANS_LOG(WARN, "invalid argument", KR(ret), K(tx_id), "ctx", OB_P(tx_ctx));
    } else {
      ObMemtableKeyArray memtable_key_info_arr;
      if (OB_ISNULL(tx_ctx)) {
        ret = OB_INVALID_ARGUMENT;
        TRANS_LOG(WARN, "tx_ctx is null", KR(ret));
      } else if (OB_FAIL(tx_ctx->get_memtable_key_arr(memtable_key_info_arr))) {
        TRANS_LOG(WARN, "get memtable key arr fail", KR(ret), K(memtable_key_info_arr));
      } else {
        int64_t count = memtable_key_info_arr.count();
        for (int i = 0; OB_SUCC(ret) && i < count; i++) {
          ObTxLockStat tx_lock_stat;
          if (OB_FAIL(tx_lock_stat.init(tx_ctx->get_addr(),
                                        tx_ctx->get_tenant_id(),
                                        tx_ctx->get_ls_id(),
                                        memtable_key_info_arr.at(i),
                                        tx_ctx->get_session_id(),
                                        0,
                                        tx_id,
                                        tx_ctx->get_ctx_create_time(),
                                        tx_ctx->get_trans_expired_time()))) {
            TRANS_LOG(WARN, "trans lock stat init fail", KR(ret),
                      "tx_ctx", *(tx_ctx), K(tx_id), "memtable key info", memtable_key_info_arr.at(i));
          } else if (OB_FAIL(tx_lock_stat_iter_.push(tx_lock_stat))) {
            TRANS_LOG(WARN, "tx_lock_stat_iter push item fail", KR(ret), K(tx_lock_stat));
          } else {
            //do nothing
          }
        }
      }
    }

    if (OB_SUCCESS == ret) {
      bool_ret = true;
    }

    return bool_ret;
  }

private:
  ObTxLockStatIterator &tx_lock_stat_iter_;
};

class PrintFunctor
{
public:
  PrintFunctor(const int64_t max_print_count, const bool verbose)
      : max_print_count_(max_print_count), print_count_(0), verbose_(verbose)
  {
  //  TRANS_LOG(INFO, "begin print hashmap item", K(max_print_count));
  }
  ~PrintFunctor() {}
  // just print, no need to check parameters
  OPERATOR_V4(PrintFunctor)
  {
    bool bool_ret = false;
    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      TRANS_LOG(WARN, "invalid argument", K(tx_id), KP(tx_ctx));
    } else if (print_count_++ < max_print_count_) {
      TRANS_LOG(INFO, "hashmap item", K(tx_id), "context", *tx_ctx);
      bool_ret = true;
      if (verbose_) {
        tx_ctx->print_trace_log();
      }
    } else {
      // do nothing
    }
    return bool_ret;
  }
private:
  int64_t max_print_count_;
  int64_t print_count_;
  bool verbose_;
};

class PrintAllLSTxCtxFunctor
{
public:
  PrintAllLSTxCtxFunctor() {}
  ~PrintAllLSTxCtxFunctor() {}
  bool operator()(const share::ObLSID &ls_id, ObLSTxCtxMgr *ls_tx_ctx_mgr)
  {
    int tmp_ret = common::OB_SUCCESS;
    bool bool_ret = false;
    const bool verbose = true;

    if (!ls_id.is_valid() || OB_ISNULL(ls_tx_ctx_mgr)) {
      TRANS_LOG(WARN, "invalid argument", K(ls_id), KP(ls_tx_ctx_mgr));
      tmp_ret = OB_INVALID_ARGUMENT;
    } else {
      ls_tx_ctx_mgr->print_all_tx_ctx(ObLSTxCtxMgr::MAX_HASH_ITEM_PRINT, verbose);
      bool_ret = true;
    }
    UNUSED(tmp_ret);
    return bool_ret;
  }
};

class ObRemoveAllTxCtxFunctor
{
public:
  explicit ObRemoveAllTxCtxFunctor() {}
  ~ObRemoveAllTxCtxFunctor() {}
  OPERATOR_V4(ObRemoveAllTxCtxFunctor)
  {
    bool bool_ret = false;
    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      TRANS_LOG(WARN, "invalid argument", K(tx_id), KP(tx_ctx));
    } else {
      bool_ret = true;
    }
    return bool_ret;
  }
};

class ObRemoveCallbackFunctor
{
public:
  explicit ObRemoveCallbackFunctor(memtable::ObMemtable *mt) : mt_(mt) {}
  ~ObRemoveCallbackFunctor() {}
  OPERATOR_V4(ObRemoveCallbackFunctor)
  {
    bool bool_ret = true;
    int tmp_ret = OB_SUCCESS;

    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx) || OB_ISNULL(mt_)) {
      tmp_ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(WARN, "invalid argument", K(tx_id));
    } else if (OB_TMP_FAIL(tx_ctx->remove_callback_for_uncommited_txn(mt_))) {
      TRANS_LOG(WARN, "remove callback for unncommitted tx failed",
        K(tmp_ret), K(tx_id), KP(tx_ctx));
    }

    if (OB_SUCCESS != tmp_ret) {
      bool_ret = false;
    }

    return bool_ret;
  }
private:
  memtable::ObMemtable *mt_;
};

class ObTxSubmitLogFunctor
{
public:
  explicit ObTxSubmitLogFunctor(const int action)
    : action_(action), result_(common::OB_SUCCESS), fail_tx_id_()
  {
    SET_EXPIRED_LIMIT(100 * 1000 /*100ms*/, 3 * 1000 * 1000 /*3s*/);
  }
  ~ObTxSubmitLogFunctor() { PRINT_FUNC_STAT; }
  enum
  {
    SUBMIT_REDO_LOG = 0,
    SUBMIT_NEXT_LOG = 1
  };

  OPERATOR_V4(ObTxSubmitLogFunctor)
  {
    int ret = OB_SUCCESS;

    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      ret = OB_INVALID_ARGUMENT;
      TRANS_LOG(WARN, "invalid argument", K(ret), K(tx_id), "ctx", OB_P(tx_ctx));
    } else if (ObTxSubmitLogFunctor::SUBMIT_REDO_LOG == action_) {
      if (OB_FAIL(tx_ctx->submit_redo_log(true))) {
        TRANS_LOG(WARN, "failed to submit redo log", K(ret), K(tx_id));
      }
    } else if (ObTxSubmitLogFunctor::SUBMIT_NEXT_LOG == action_) {
      if (OB_FAIL(tx_ctx->try_submit_next_log())) {
        TRANS_LOG(WARN, "failed to submit next log", K(ret), K(tx_id));
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(ERROR, "unexpected submit action", K(ret), K(tx_id));
    }

    if (OB_FAIL(ret)) {
      result_ = ret;
      fail_tx_id_ = tx_id;
    }

    return OB_SUCC(ret);
  }

  ObTransID get_fail_tx_id() { return fail_tx_id_; }
  int get_result() const { return result_; }

private:
  int action_;
  int result_;
  ObTransID fail_tx_id_;
};

class GetMinStartLogTsFunctor
{
public:
  GetMinStartLogTsFunctor() : min_start_log_ts_(INT64_MAX)
  {
  }
  ~GetMinStartLogTsFunctor() {}

  OPERATOR_V4(GetMinStartLogTsFunctor)
  {
    bool bool_ret = false;
    if (!tx_id.is_valid() || OB_ISNULL(tx_ctx)) {
      TRANS_LOG(WARN, "invalid argument", K(tx_id), KP(tx_ctx));
    } else {
      int64_t start_log_ts = tx_ctx->get_start_log_ts();
      if (start_log_ts < min_start_log_ts_) {
        min_start_log_ts_ = start_log_ts;
      }
      bool_ret = true;
    }
    return bool_ret;
  }

  int64_t get_min_start_log_ts() { return min_start_log_ts_; }

private:
  int64_t min_start_log_ts_;
};

class IteratePartCtxAskSchedulerStatusFunctor
{
public:
  IteratePartCtxAskSchedulerStatusFunctor()
  {
    SET_EXPIRED_LIMIT(100 * 1000 /*100ms*/, 3 * 1000 * 1000 /*3s*/);
    min_start_scn_ = INT64_MAX;
  }

  ~IteratePartCtxAskSchedulerStatusFunctor() { PRINT_FUNC_STAT; }
  OPERATOR_V4(IteratePartCtxAskSchedulerStatusFunctor)
  {
    int ret = OB_SUCCESS;

    if (OB_UNLIKELY(!tx_id.is_valid() || OB_ISNULL(tx_ctx))) {
      ret = OB_INVALID_ARGUMENT;
      TRANS_LOG(WARN, "invalid argument", KR(ret), K(tx_id), "ctx", OB_P(tx_ctx));
    } else {
      int64_t ctx_start_scn = tx_ctx->get_start_log_ts();
      if (ctx_start_scn < 0) {
        ctx_start_scn = INT64_MAX;
      }
      if (OB_FALSE_IT(min_start_scn_ = MIN(min_start_scn_, ctx_start_scn))) {
        // do nothing
      } else if (OB_FAIL(tx_ctx->check_scheduler_status())) {
        TRANS_LOG(WARN, "check scheduler status error", KR(ret), "ctx", *tx_ctx);
      } else {
        // do nothing
      }
    }

    if (OB_FAIL(ret)) {
      min_start_scn_ = OB_INVALID_TIMESTAMP;
    }

    return true;
  }

  int64_t get_min_start_scn() { return min_start_scn_; }

  MinStartScnStatus get_min_start_status()
  {
    MinStartScnStatus start_status = MinStartScnStatus::HAS_CTX;

    if (OB_INVALID_TIMESTAMP == min_start_scn_) {
      start_status = MinStartScnStatus::UNKOWN;
    } else if (INT64_MAX == min_start_scn_) {
      start_status = MinStartScnStatus::NO_CTX;
    }
    return start_status;
  }

private:
  int64_t min_start_scn_;
};

} // transaction
} // oceanbase

#endif // OCEANBASE_TRANSACTION_OB_TX_FUNCTOR
