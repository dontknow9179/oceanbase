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

#include "ob_log_restore_handler.h"
#include "common/ob_member_list.h"
#include "common/ob_role.h"                  // ObRole
#include "lib/lock/ob_tc_rwlock.h"
#include "lib/net/ob_addr.h"                 // ObAddr
#include "lib/ob_define.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"         // K*
#include "lib/string/ob_string.h"            // ObString
#include "lib/time/ob_time_utility.h"        // ObTimeUtility
#include "lib/utility/ob_macro_utils.h"
#include "logservice/ob_log_service.h"       // ObLogService
#include "logservice/palf/log_define.h"
#include "logservice/palf/log_group_entry.h"
#include "logservice/palf/palf_env.h"        // PalfEnv
#include "logservice/palf/palf_iterator.h"
#include "logservice/replayservice/ob_log_replay_service.h"
#include "ob_log_archive_piece_mgr.h"        // ObLogArchivePieceContext
#include "ob_fetch_log_task.h"               // ObFetchLogTask
#include "ob_log_restore_rpc_define.h"       // RPC Request
#include "ob_remote_log_source.h"            // ObRemoteLogParent
#include "ob_remote_log_source_allocator.h"  // ObResSrcAlloctor
#include "ob_log_restore_define.h"           // ObRemoteFetchContext
#include "share/ob_define.h"
#include "share/ob_errno.h"
#include "share/restore/ob_log_restore_source.h"

namespace oceanbase
{
using namespace palf;
namespace logservice
{
using namespace oceanbase::share;
ObLogRestoreHandler::ObLogRestoreHandler() :
  parent_(NULL),
  context_(),
  restore_context_()
{}

ObLogRestoreHandler::~ObLogRestoreHandler()
{
  destroy();
}

int ObLogRestoreHandler::init(const int64_t id, PalfEnv *palf_env)
{
  int ret = OB_SUCCESS;
  palf::PalfHandle palf_handle;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else if (NULL == palf_env) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid arguments", K(ret), KP(palf_env));
  } else if (OB_FAIL(palf_env->open(id, palf_handle))) {
    CLOG_LOG(WARN, "get palf_handle failed", K(ret), K(id));
  } else {
    id_ = id;
    palf_handle_ = palf_handle;
    palf_env_ = palf_env;
    role_ = FOLLOWER;
    is_in_stop_state_ = false;
    is_inited_ = true;
    CLOG_LOG(INFO, "ObLogRestoreHandler init success", K(id), K(role_), K(palf_handle));
  }

  if (OB_FAIL(ret)) {
    if (true == palf_handle.is_valid() && false == is_valid()) {
      palf_env_->close(palf_handle);
    }
  }
  return ret;
}

int ObLogRestoreHandler::stop()
{
  int ret = OB_SUCCESS;
  WLockGuard guard(lock_);
  if (IS_INIT) {
    is_in_stop_state_ = true;
    if (palf_handle_.is_valid()) {
      palf_env_->close(palf_handle_);
    }
  }
  CLOG_LOG(INFO, "ObLogRestoreHandler stop", K(ret), KPC(this));
  return ret;
}

void ObLogRestoreHandler::destroy()
{
  WLockGuard guard(lock_);
  if (IS_INIT) {
    is_in_stop_state_ = true;
    is_inited_ = false;
    if (palf_handle_.is_valid()) {
      palf_env_->close(palf_handle_);
    }
    palf_env_ = NULL;
    id_ = -1;
    proposal_id_ = 0;
    role_ = ObRole::INVALID_ROLE;
    context_.reset();
  }
  if (NULL != parent_) {
    ObResSrcAlloctor::free(parent_);
    parent_ = NULL;
  }
}

// To support log restore in parallel, the FetchLogTask is introduced and each task covers a range of logs, as [Min_LSN, Max_LSN),
// which means logs wiil be pulled from the log restore source  and submitted to Palf with this task.
//
// Two scenarios should be handled:
// 1) Log restore is done within the leader of restore handler, which changes with the Palf
// 2) The restore end_scn maybe set to control the log pull and restore.
//
// For follower, log restore should terminate while all tasks should be freed and the restore context can be reset.
// The same as restore to_end.
//
// To generate nonoverlapping and continuous FetchLogTask, a max_submit_lsn_ in context is hold, which is the Max_LSN of the previous task
// and the Min_LSN of the current task.
//
// When role change of the restore handler, the context is reset but which is not reset when restore to_end is set.
//
// The role change is easy to distinguish with the proposal_id while restore_scn change is not,
// so reset restore context and advance issue_version, to reset start_fetch_log_lsn if restore_scn is advanced
// and free issued tasks before restore to_end(paralleled)
//
void ObLogRestoreHandler::switch_role(const common::ObRole &role, const int64_t proposal_id)
{
  WLockGuard guard(lock_);
  role_ = role;
  proposal_id_ = proposal_id;
  if (! is_strong_leader(role_)) {
    ObResSrcAlloctor::free(parent_);
    parent_ = NULL;
    context_.reset();
  }
}

int ObLogRestoreHandler::get_role(common::ObRole &role, int64_t &proposal_id) const
{
  return ObLogHandlerBase::get_role(role, proposal_id);
}

bool ObLogRestoreHandler::is_valid() const
{
  return true == is_inited_
         && false == is_in_stop_state_
         && true == palf_handle_.is_valid()
         && NULL != palf_env_;
}

int ObLogRestoreHandler::get_upper_limit_scn(SCN &scn) const
{
  RLockGuard guard(lock_);
  int ret = OB_SUCCESS;
  if (OB_ISNULL(parent_)) {
    ret = OB_EAGAIN;
  } else {
    parent_->get_upper_limit_scn(scn);
  }
  return ret;
}

int ObLogRestoreHandler::get_max_restore_scn(SCN &scn) const
{
  RLockGuard guard(lock_);
  int ret = OB_SUCCESS;
  if (OB_ISNULL(parent_) || ! parent_->to_end()) {
    ret = OB_EAGAIN;
  } else {
    parent_->get_end_scn(scn);
  }
  return ret;
}

int ObLogRestoreHandler::add_source(logservice::DirArray &array, const SCN &end_scn)
{
  int ret = OB_SUCCESS;
  WLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    CLOG_LOG(WARN, "ObLogRestoreHandler not init", K(ret), KPC(this));
    /*
  } else if (! is_strong_leader(role_)) {
    // not leader, just skip
    ret = OB_NOT_MASTER;
    */
  } else if (OB_UNLIKELY(array.empty() || !end_scn.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", K(ret), K(array), K(end_scn), KPC(this));
  } else if (FALSE_IT(alloc_source(share::ObLogRestoreSourceType::RAWPATH))) {
  } else if (NULL == parent_) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    ObRemoteRawPathParent *source = static_cast<ObRemoteRawPathParent *>(parent_);
    const bool source_exist = source->is_valid();
    if (OB_FAIL(source->set(array, end_scn))) {
      CLOG_LOG(WARN, "ObRemoteRawPathParent set failed", K(ret), K(array), K(end_scn));
      ObResSrcAlloctor::free(parent_);
      parent_ = NULL;
    } else if (! source_exist) {
      context_.set_issue_version();
    }
  }
  return ret;;
}

int ObLogRestoreHandler::add_source(share::ObBackupDest &dest, const SCN &end_scn)
{
  int ret = OB_SUCCESS;
  WLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    CLOG_LOG(WARN, "ObLogRestoreHandler not init", K(ret), KPC(this));
  } else if (! is_strong_leader(role_)) {
    // not leader, just skip
  } else if (OB_UNLIKELY(! dest.is_valid() || !end_scn.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
     CLOG_LOG(WARN, "invalid argument", K(ret), K(end_scn), K(dest), KPC(this));
  } else if (FALSE_IT(alloc_source(share::ObLogRestoreSourceType::LOCATION))) {
  } else if (NULL == parent_) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    ObRemoteLocationParent *source = static_cast<ObRemoteLocationParent *>(parent_);
    const bool source_exist = source->is_valid();
    if (OB_FAIL(source->set(dest, end_scn))) {
      CLOG_LOG(WARN, "ObRemoteLocationParent set failed", K(ret), K(end_scn), K(dest));
      ObResSrcAlloctor::free(parent_);
      parent_ = NULL;
    } else if (! source_exist) {
      context_.set_issue_version();
    }
  }
  return ret;
}

int ObLogRestoreHandler::add_source(const common::ObAddr &addr, const SCN &end_scn)
{
  int ret = OB_SUCCESS;
  WLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    CLOG_LOG(WARN, "ObLogRestoreHandler not init", K(ret), KPC(this));
  } else if (! is_strong_leader(role_)) {
    // not leader, just skip
  } else if (OB_UNLIKELY(!addr.is_valid() || !end_scn.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", K(ret), K(end_scn), K(addr), KPC(this));
  } else if (FALSE_IT(alloc_source(ObLogRestoreSourceType::SERVICE))) {
  } else if (NULL == parent_) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    ObRemoteSerivceParent *source = static_cast<ObRemoteSerivceParent *>(parent_);
    const bool source_exist = source->is_valid();
    if (OB_FAIL(source->set(addr, end_scn))) {
      CLOG_LOG(WARN, "ObRemoteSerivceParent set failed",
          K(ret), K(end_scn), K(addr), KPC(this));
      ObResSrcAlloctor::free(parent_);
      parent_ = NULL;
    } else if (! source_exist) {
      context_.set_issue_version();
    }
  }
  return ret;
}

int ObLogRestoreHandler::clean_source()
{
  int ret = OB_SUCCESS;
  WLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    CLOG_LOG(WARN, "ObLogRestoreHandler not init", K(ret), KPC(this));
  } else if (NULL == parent_) {
    // just skip
  } else {
    CLOG_LOG(INFO, "log_restore_source is empty, clean source", KPC(parent_));
    ObResSrcAlloctor::free(parent_);
    parent_ = NULL;
    context_.reset();
  }
  return ret;
}

int ObLogRestoreHandler::raw_write(const int64_t proposal_id,
                                   const palf::LSN &lsn,
                                   const SCN &scn,
                                   const char *buf,
                                   const int64_t buf_size)
{
  int ret = OB_SUCCESS;
  int64_t wait_times = 0;
  const bool need_nonblock = true;
  palf::PalfAppendOptions opts;
  opts.need_nonblock = need_nonblock;
  opts.need_check_proposal_id = true;
  while (wait_times < MAX_RAW_WRITE_RETRY_TIMES) {
    do {
      ret = OB_SUCCESS;
      WLockGuard guard(lock_);
      if (IS_NOT_INIT) {
        ret = OB_NOT_INIT;
      } else if (is_in_stop_state_) {
        ret = OB_IN_STOP_STATE;
      } else if (LEADER != role_) {
        ret = OB_NOT_MASTER;
      } else if (OB_UNLIKELY(!lsn.is_valid()
            || NULL == buf
            || 0 >= buf_size
            || 0 >= proposal_id)) {
        ret = OB_INVALID_ARGUMENT;
        CLOG_LOG(WARN, "invalid argument", K(ret), K(proposal_id), K(lsn), K(buf), K(buf_size));
      } else if (proposal_id != proposal_id_) {
        CLOG_LOG(INFO, "stale task, just skip", K(proposal_id), K(proposal_id_), K(lsn), K(id_));
      } else if (NULL == parent_ || parent_->to_end()) {
        ret = OB_RESTORE_LOG_TO_END;
        CLOG_LOG(INFO, "submit log to end, just skip", K(ret), K(lsn), KPC(this));
      } else {
        opts.proposal_id = proposal_id_;
        ret = palf_handle_.raw_write(opts, lsn, buf, buf_size);
        if (OB_SUCC(ret)) {
          context_.max_fetch_lsn_ = lsn + buf_size;
          context_.max_fetch_scn_ = scn;
          context_.last_fetch_ts_ = ObTimeUtility::fast_current_time();
          if (parent_->set_to_end(scn)) {
            // To stop and clear all restore log tasks and restore context, reset context and advance issue version
            CLOG_LOG(INFO, "restore log to_end succ", KPC(this), KPC(parent_));
            context_.reset();
            context_.set_issue_version();
          }
        }
      }
    } while (0);

    if (OB_EAGAIN == ret) {
      ++wait_times;
      int64_t sleep_us = wait_times * 10;
      if (sleep_us > MAX_RETRY_SLEEP_US) {
        sleep_us = MAX_RETRY_SLEEP_US;
      }
      ob_usleep(sleep_us);
    } else {
      // other ret code, end loop
      break;
    }
  }
  return ret;
}

void ObLogRestoreHandler::deep_copy_source(ObRemoteSourceGuard &source_guard)
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);
  ObRemoteLogParent *source = NULL;
  if (OB_ISNULL(parent_)) {
    CLOG_LOG(WARN, "parent_ is NULL", K(ret));
  } else if (OB_ISNULL(source = ObResSrcAlloctor::alloc(parent_->get_source_type(), share::ObLSID(id_)))) {
  } else if (FALSE_IT(parent_->deep_copy_to(*source))) {
  } else if (FALSE_IT(source_guard.set_source(source))) {
  }
}

int ObLogRestoreHandler::schedule(const int64_t id,
    const int64_t proposal_id,
    const int64_t version,
    const LSN &lsn,
    bool &scheduled)
{
  int ret = OB_SUCCESS;
  scheduled = false;
  WLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(! lsn.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (id != id_ || proposal_id != proposal_id_ || version != context_.issue_version_) {
    // stale task
  } else {
    scheduled = true;
    context_.max_submit_lsn_ = lsn;
    context_.issue_task_num_++;
  }
  return ret;
}

int ObLogRestoreHandler::try_retire_task(ObFetchLogTask &task, bool &done)
{
  done = false;
  int ret = OB_SUCCESS;
  WLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(!task.is_valid() || task.id_.id() != id_)) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", K(ret), K(task));
  } else if (! is_strong_leader(role_) || NULL == parent_) {
    done = true;
    CLOG_LOG(INFO, "ls not leader or source is NULL, stale task, just skip it", K(task), K(role_));
  } else if (OB_UNLIKELY(task.proposal_id_ != proposal_id_
        || task.version_ != context_.issue_version_)) {
    done = true;
    CLOG_LOG(INFO, "stale task, just skip it", K(task), KPC(this));
  } else if (context_.max_fetch_lsn_.is_valid() && context_.max_fetch_lsn_ >= task.end_lsn_) {
    CLOG_LOG(INFO, "restore max_lsn bigger than task end_lsn, just skip it", K(task), KPC(this));
    done = true;
    context_.issue_task_num_--;
  } else if (parent_->to_end()) {
    // when restore is set to_end, issue_version_ is advanced, and all issued tasks before are stale tasks as stale issue_version_
    CLOG_LOG(ERROR, "error unexpected, log restored to_end, just skip it", K(task), KPC(this), K(parent_));
    done = true;
    context_.issue_task_num_--;
  }
  return ret;
}

int ObLogRestoreHandler::need_schedule(bool &need_schedule,
    int64_t &proposal_id,
    ObRemoteFetchContext &context) const
{
  int ret = OB_SUCCESS;
  need_schedule = false;
  RLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(parent_)) {
    // do nothing
  } else if (OB_SUCCESS != context_.error_context_.ret_code_) {
    // error exist, no need schedule
  } else {
    need_schedule = is_strong_leader(role_) && ! parent_->to_end();
    proposal_id = proposal_id_;
    context = context_;
  }
  if (! need_schedule) {
    if (REACH_TIME_INTERVAL(60 * 1000 * 1000L)) {
      CLOG_LOG(INFO, "restore not schedule", KPC(this), KPC(parent_));
    }
  }
  return ret;
}

bool ObLogRestoreHandler::need_update_source() const
{
  RLockGuard guard(lock_);
  return is_strong_leader(role_);
}

void ObLogRestoreHandler::mark_error(share::ObTaskId &trace_id, const int ret_code)
{
  int ret = OB_SUCCESS;
  WLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (! is_strong_leader(role_)) {
    CLOG_LOG(INFO, "not leader, no need record error", K(id_), K(ret_code));
  } else if (OB_SUCCESS == context_.error_context_.ret_code_) {
    context_.error_context_.ret_code_ = ret_code;
    context_.error_context_.trace_id_.set(trace_id);
  }
}

int ObLogRestoreHandler::get_restore_error(share::ObTaskId &trace_id, int &ret_code, bool &error_exist)
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);
  error_exist = false;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (! is_strong_leader(role_)) {
    // not leader, not report
  } else if (OB_SUCCESS == context_.error_context_.ret_code_) {
    // no error, not report
  } else {
    trace_id.set(context_.error_context_.trace_id_);
    ret_code = context_.error_context_.ret_code_;
    error_exist = true;
  }
  return ret;
}
int ObLogRestoreHandler::update_location_info(ObRemoteLogParent *source)
{
  int ret = OB_SUCCESS;
  WLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(source)) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "source is NULL", K(ret), K(source));
  } else if (OB_ISNULL(parent_) || ! is_strong_leader(role_)) {
    // stale task, just skip
  } else if (source->get_source_type() != parent_->get_source_type()) {
    CLOG_LOG(INFO, "source type not consistent with parent, just skip", KPC(source), KPC(this));
  } else if (OB_FAIL(parent_->update_locate_info(*source))) {
    CLOG_LOG(WARN, "update location info failed", K(ret), KPC(source), KPC(this));
  }
  return ret;
}

void ObLogRestoreHandler::alloc_source(const ObLogRestoreSourceType &type)
{
  if (parent_ != NULL && parent_->get_source_type() != type) {
    ObResSrcAlloctor::free(parent_);
    parent_ = NULL;
  }

  if (NULL == parent_) {
    parent_ = ObResSrcAlloctor::alloc(type, share::ObLSID(id_));
  }
}

int ObLogRestoreHandler::check_restore_done(const SCN &recovery_end_scn, bool &done)
{
  int ret = OB_SUCCESS;
  palf::PalfGroupBufferIterator iter;
  SCN end_scn;
  palf::LogGroupEntry entry;
  SCN entry_scn;
  palf::LSN end_lsn;
  int64_t id = 0;
  done = false;
  {
    RLockGuard guard(lock_);
    if (IS_NOT_INIT) {
      ret = OB_NOT_INIT;
      CLOG_LOG(WARN, "ObLogRestoreHandler not init", K(ret), KPC(this));
    } else if (OB_UNLIKELY(!recovery_end_scn.is_valid())) {
      ret = OB_INVALID_ARGUMENT;
      CLOG_LOG(WARN, "invalid argument", K(ret), K(recovery_end_scn));
    } else if (! is_strong_leader(role_)) {
      ret = OB_NOT_MASTER;
    } else if (restore_context_.seek_done_) {
      end_lsn = restore_context_.lsn_;
    } else if (OB_FAIL(palf_handle_.get_end_scn(end_scn))) {
      CLOG_LOG(WARN, "get end scn failed", K(ret), K_(id));
    } else if (end_scn < recovery_end_scn) {
      ret = OB_EAGAIN;
      CLOG_LOG(WARN, "log restore not finish", K(ret), K_(id), K(end_scn), K(recovery_end_scn));
    } else if (OB_FAIL(palf_handle_.seek(recovery_end_scn, iter))) {
      CLOG_LOG(WARN, "palf seek failed", K(ret), K_(id));
    } else if (OB_FAIL(iter.next())) {
      CLOG_LOG(WARN, "next entry failed", K(ret));
    } else if (OB_FAIL(iter.get_entry(entry, end_lsn))) {
      CLOG_LOG(WARN, "gen entry failed", K(ret), K_(id), K(iter));
    } else if (entry.get_scn() == recovery_end_scn) {
      // if the max log scn equals to recovery_end_scn, the max log should be replayed
      // otherwise the max log should not be replayed
      end_lsn = end_lsn + entry.get_serialize_size();
    }
    id = ATOMIC_LOAD(&id_);
  }

  // update restore context
  {
    WLockGuard guard(lock_);
    if (OB_SUCC(ret) && ! restore_context_.seek_done_) {
      restore_context_.lsn_ = end_lsn;
      restore_context_.seek_done_ = true;
      CLOG_LOG(INFO, "update restore context", K(id), K_(restore_context));
    }
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(MTL(ObLogService*)->get_log_replay_service()->is_replay_done(
            ObLSID(id), end_lsn, done))) {
      CLOG_LOG(WARN, "is_replay_done failed", K(ret), K(id), K(end_lsn));
    } else if (! done) {
      ret = OB_EAGAIN;
      CLOG_LOG(WARN, "log restore finish, and log replay not finish",
          K(ret), K(id), K(end_lsn), K(end_scn), K(recovery_end_scn));
    } else {
      CLOG_LOG(TRACE, "check restore done succ", K(ret), K(id));
    }
  }
  return ret;
}

int ObLogRestoreHandler::check_restore_to_newest(share::SCN &end_scn, share::SCN &archive_scn)
{
  int ret = OB_SUCCESS;
  ObRemoteSourceGuard guard;
  ObRemoteLogParent *source = NULL;
  ObLogArchivePieceContext *piece_context = NULL;
  share::ObBackupDest *dest = NULL;
  end_scn = SCN::min_scn();
  archive_scn = SCN::min_scn();
  SCN restore_scn = SCN::min_scn();
  CLOG_LOG(INFO, "start check restore to newest", K(id_));
  {
    if (IS_NOT_INIT) {
      ret = OB_NOT_INIT;
      CLOG_LOG(WARN, "ObLogRestoreHandler not init", K(ret), KPC(this));
    } else if (! is_strong_leader(role_)) {
      ret = OB_NOT_MASTER;
      CLOG_LOG(WARN, "not leader", K(ret), KPC(this));
    } else if (FALSE_IT(deep_copy_source(guard))) {
    } else if (OB_ISNULL(source = guard.get_source())
        || !share::is_location_log_source_type(source->get_source_type())) {
      ret = OB_EAGAIN;
      CLOG_LOG(WARN, "invalid source", K(ret), KPC(this), KPC(source));
    } else if (! source->is_valid()) {
      ret = OB_EAGAIN;
      CLOG_LOG(WARN, "source is invalid", K(ret), KPC(this), KPC(source));
    } else {
      ObRemoteLocationParent *location_source = dynamic_cast<ObRemoteLocationParent *>(source);
      location_source->get(dest, piece_context, restore_scn);
    }
  }

  if (OB_SUCC(ret) && NULL != piece_context) {
    palf::LSN archive_lsn;
    palf::LSN end_lsn;
    if (OB_FAIL(palf_handle_.get_end_lsn(end_lsn))) {
      CLOG_LOG(WARN, "get end lsn failed", K(id_));
    } else if (OB_FAIL(palf_handle_.get_end_scn(end_scn))) {
      CLOG_LOG(WARN, "get end scn failed", K(id_));
    } else if (OB_FAIL(piece_context->get_max_archive_log(archive_lsn, archive_scn))) {
      CLOG_LOG(WARN, "get max archive log failed", K(id_));
    } else {
      if (archive_lsn == end_lsn && archive_scn == SCN::min_scn()) {
        archive_scn = end_scn;
        CLOG_LOG(INFO, "rewrite archive_scn while end_lsn equals to archive_lsn and archive_scn not got",
            K(id_), K(archive_lsn), K(archive_scn), K(end_lsn), K(end_scn));
      }
      CLOG_LOG(INFO, "check_restore_to_newest succ", K(id_), K(archive_scn), K(end_scn));
    }
  }
  return ret;
}

int ObLogRestoreHandler::submit_sorted_task(ObFetchLogTask &task)
{
  WLockGuard guard(lock_);
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (! is_strong_leader(role_)) {
    ret = OB_NOT_MASTER;
    CLOG_LOG(WARN, "restore_handler not master", K(ret), KPC(this));
  } else if (OB_UNLIKELY(! task.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", K(ret), K(task));
  } else if (OB_UNLIKELY(task.iter_.is_empty())) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "task iterator is empty", K(ret), K(task));
  } else if (OB_FAIL(context_.submit_array_.push_back(&task))) {
    CLOG_LOG(WARN, "push back failed", K(ret), K(task));
  } else {
    std::sort(context_.submit_array_.begin(), context_.submit_array_.end(), FetchLogTaskCompare());
  }
  return ret;
}

int ObLogRestoreHandler::get_next_sorted_task(ObFetchLogTask *&task)
{
  int ret = OB_SUCCESS;
  palf::LSN max_lsn;
  ObFetchLogTask *first = NULL;
  task = NULL;
  WLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (! is_strong_leader(role_)) {
    ret = OB_NOT_MASTER;
  } else if (NULL != parent_ && parent_->to_end()) {
    // if restore to end, free all cached tasks
    ret = context_.reset_sorted_tasks();
  } else if (context_.submit_array_.empty()) {
    // sorted_array is empty, do nothing
  } else if (FALSE_IT(first = context_.submit_array_.at(0))) {
    // get the first one
  } else if (OB_FAIL(palf_handle_.get_max_lsn(max_lsn))) {
    CLOG_LOG(WARN, "get max lsn failed", K(ret), K_(id));
  } else if (max_lsn < first->start_lsn_) {
    // check the first task if is in turn, skip it if not
    CLOG_LOG(TRACE, "task not in turn", KPC(first), K(max_lsn));
  } else if (context_.submit_array_.count() == 1) {
    // only one task in array, just pop it
    context_.submit_array_.pop_back();
    task = first;
  } else {
    // more than one task, replace first and end, and sort again
    ObFetchLogTask *tmp_task = NULL;
    context_.submit_array_.pop_back(tmp_task);
    context_.submit_array_.at(0) = tmp_task;
    std::sort(context_.submit_array_.begin(), context_.submit_array_.end(), FetchLogTaskCompare());
    task = first;
  }
  return ret;
}

int ObLogRestoreHandler::diagnose(RestoreDiagnoseInfo &diagnose_info)
{
  int ret = OB_SUCCESS;
  diagnose_info.restore_context_info_.reset();
  diagnose_info.restore_context_info_.reset();
  const int64_t MAX_TRACE_ID_LENGTH = 64;
  char trace_id[MAX_TRACE_ID_LENGTH];
  RLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (FALSE_IT(diagnose_info.restore_role_ = role_)) {
  } else if (FALSE_IT(diagnose_info.restore_proposal_id_ = proposal_id_)) {
  } else if (OB_FAIL(diagnose_info.restore_context_info_.append_fmt("issue_task_num:%ld; "
                                                                    "last_fetch_ts:%ld; "
                                                                    "max_submit_lsn:%ld; "
                                                                    "max_fetch_lsn:%ld; "
                                                                    "max_fetch_scn:%ld; ",
                                                                    context_.issue_task_num_,
                                                                    context_.last_fetch_ts_,
                                                                    context_.max_submit_lsn_.val_,
                                                                    context_.max_fetch_lsn_.val_,
                                                                    context_.max_fetch_scn_.convert_to_ts()))) {
    CLOG_LOG(WARN, "append restore_context_info failed", K(ret), K(context_));
  } else if (FALSE_IT(context_.error_context_.trace_id_.to_string(trace_id, sizeof(trace_id)))) {
  } else if (OB_FAIL(diagnose_info.restore_err_context_info_.append_fmt("ret_code:%d; "
                                                                        "trace_id:%s; ",
                                                                        context_.error_context_.ret_code_,
                                                                        trace_id))) {
    CLOG_LOG(WARN, "append restore_context_info failed", K(ret), K(context_));
  }
  return ret;
}

} // namespace logservice
} // namespace oceanbase
