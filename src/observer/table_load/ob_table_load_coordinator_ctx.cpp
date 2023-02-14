// Copyright (c) 2022-present Oceanbase Inc. All Rights Reserved.
// Author:
//   suzhi.yt <suzhi.yt@oceanbase.com>

#define USING_LOG_PREFIX SERVER

#include "observer/table_load/ob_table_load_coordinator_ctx.h"
#include "observer/table_load/ob_table_load_coordinator_trans.h"
#include "observer/table_load/ob_table_load_table_ctx.h"
#include "observer/table_load/ob_table_load_task_scheduler.h"

namespace oceanbase
{
namespace observer
{
using namespace common;
using namespace common::hash;
using namespace lib;
using namespace table;
using namespace sql;
using namespace obrpc;

ObTableLoadCoordinatorCtx::ObTableLoadCoordinatorCtx(ObTableLoadTableCtx *ctx)
  : ctx_(ctx),
    allocator_("TLD_CoordCtx", OB_MALLOC_NORMAL_BLOCK_SIZE, ctx->param_.tenant_id_),
    task_scheduler_(nullptr),
    last_trans_gid_(1024),
    next_session_id_(0),
    status_(ObTableLoadStatusType::NONE),
    error_code_(OB_SUCCESS),
    redef_table_(),
    is_inited_(false)
{
}

ObTableLoadCoordinatorCtx::~ObTableLoadCoordinatorCtx()
{
  destroy();
}

int ObTableLoadCoordinatorCtx::init(ObSQLSessionInfo *session_info,
    const ObIArray<int64_t> &idx_array)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObTableLoadCoordinatorCtx init twice", KR(ret), KP(this));
  } else if (idx_array.count() != ctx_->param_.column_count_) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid idx_array", KR(ret), K(idx_array.count()), K_(ctx_->param_.column_count));
  } else {
    // init redef table
    if (OB_FAIL(redef_table_.init(ctx_, session_info))) {
      LOG_WARN("failed to init ddl processor", KR(ret));
    } else if (OB_FAIL(redef_table_.start())) {
      LOG_WARN("failed to create hidden table", KR(ret));
    } else if (OB_FAIL(target_schema_.init(ctx_->param_.tenant_id_,
                                            ctx_->param_.target_table_id_))) {
      LOG_WARN("fail to init table load schema", KR(ret), K(ctx_->param_.tenant_id_),
                K(ctx_->param_.target_table_id_));
    }
    // init idx array
    else if (OB_FAIL(idx_array_.assign(idx_array))) {
      LOG_WARN("failed to assign idx array", KR(ret), K(idx_array));
    }
    // init partition_location_
    else if (OB_FAIL(partition_location_.init(ctx_->param_.tenant_id_, ctx_->schema_.partition_ids_,
                                         allocator_))) {
      LOG_WARN("fail to init partition location", KR(ret));
    }
    else if (OB_FAIL(target_partition_location_.init(ctx_->param_.tenant_id_,
        target_schema_.partition_ids_, allocator_))) {
      LOG_WARN("fail to init origin partition location", KR(ret));
    }
    // init partition_calc_
    else if (OB_FAIL(
               partition_calc_.init(ctx_->param_.tenant_id_, ctx_->param_.table_id_))) {
      LOG_WARN("fail to init partition calc", KR(ret));
    }
    // init trans_allocator_
    else if (OB_FAIL(trans_allocator_.init("TLD_CTransPool", ctx_->param_.tenant_id_))) {
      LOG_WARN("fail to init trans allocator", KR(ret));
    }
    // init trans_map_
    else if (OB_FAIL(
               trans_map_.create(1024, "TLD_TransMap", "TLD_TransMap", ctx_->param_.tenant_id_))) {
      LOG_WARN("fail to create trans map", KR(ret));
    }
    // init trans_ctx_map_
    else if (OB_FAIL(trans_ctx_map_.create(1024, "TLD_TCtxMap", "TLD_TCtxMap",
                                           ctx_->param_.tenant_id_))) {
      LOG_WARN("fail to create trans ctx map", KR(ret));
    }
    // init segment_trans_ctx_map_
    else if (OB_FAIL(segment_ctx_map_.init("TLD_SegCtxMap", ctx_->param_.tenant_id_))) {
      LOG_WARN("fail to init segment ctx map", KR(ret));
    }
    // generate credential_
    else if (OB_FAIL(generate_credential(session_info->get_priv_user_id()))) {
      LOG_WARN("fail to generate credential", KR(ret));
    }
    // init task_scheduler_
    else if (OB_ISNULL(task_scheduler_ = OB_NEWx(ObTableLoadTaskThreadPoolScheduler, (&allocator_),
                                                 ctx_->param_.session_count_, allocator_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to new ObTableLoadTaskThreadPoolScheduler", KR(ret));
    } else if (OB_FAIL(task_scheduler_->init())) {
      LOG_WARN("fail to init task scheduler", KR(ret));
    } else if (OB_FAIL(task_scheduler_->start())) {
      LOG_WARN("fail to start task scheduler", KR(ret));
    }
    if (OB_SUCC(ret)) {
      is_inited_ = true;
    } else {
      destroy();
    }
  }
  return ret;
}

void ObTableLoadCoordinatorCtx::stop()
{
  if (nullptr != task_scheduler_) {
    task_scheduler_->stop();
    task_scheduler_->wait();
  }
  LOG_INFO("coordinator ctx stop succ");
}

void ObTableLoadCoordinatorCtx::destroy()
{
  if (nullptr != task_scheduler_) {
    task_scheduler_->stop();
    task_scheduler_->wait();
    task_scheduler_->~ObITableLoadTaskScheduler();
    allocator_.free(task_scheduler_);
    task_scheduler_ = nullptr;
  }
  for (TransMap::const_iterator iter = trans_map_.begin(); iter != trans_map_.end(); ++iter) {
    ObTableLoadCoordinatorTrans *trans = iter->second;
    abort_unless(0 == trans->get_ref_count());
    trans_allocator_.free(trans);
  }
  trans_map_.reuse();
  for (TransCtxMap::const_iterator iter = trans_ctx_map_.begin(); iter != trans_ctx_map_.end();
       ++iter) {
    ObTableLoadTransCtx *trans_ctx = iter->second;
    ctx_->free_trans_ctx(trans_ctx);
  }
  trans_ctx_map_.reuse();
  segment_ctx_map_.reset();
  commited_trans_ctx_array_.reset();
  redef_table_.reset();
}

int ObTableLoadCoordinatorCtx::generate_credential(uint64_t user_id)
{
  int ret = OB_SUCCESS;
  const int64_t expire_ts = 0;

  share::schema::ObSchemaGetterGuard schema_guard;
  const share::schema::ObUserInfo *user_info = NULL;
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid schema service", K(ret));
  } else if (OB_FAIL(GCTX.schema_service_->get_tenant_schema_guard(ctx_->param_.tenant_id_, schema_guard))) {
    LOG_WARN("fail to get schema guard", K(ret), "tenant_id", ctx_->param_.tenant_id_);
  } else if (OB_FAIL(schema_guard.get_user_info(ctx_->param_.tenant_id_, user_id, user_info))) {
    LOG_WARN("fail to get user info", K(ret), K(ctx_->param_.tenant_id_), K(user_id));
  } else if (OB_ISNULL(user_info)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("user info is null", K(ret), K(ctx_->param_.tenant_id_), K(user_id));
  } else if (OB_FAIL(ObTableLoadUtils::generate_credential(ctx_->param_.tenant_id_, user_id,
                                                    0, expire_ts,
                                                    user_info->get_passwd_str().hash(), allocator_, credential_))) {
    LOG_WARN("fail to generate credential", KR(ret));
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::advance_status(ObTableLoadStatusType status)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadCoordinatorCtx not init", KR(ret));
  } else if (OB_UNLIKELY(ObTableLoadStatusType::ERROR == status ||
                         ObTableLoadStatusType::ABORT == status)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", KR(ret), K(status));
  } else {
    obsys::ObWLockGuard guard(rwlock_);
    if (OB_UNLIKELY(ObTableLoadStatusType::ERROR == status_)) {
      ret = error_code_;
      LOG_WARN("coordinator has error", KR(ret));
    } else if (OB_UNLIKELY(ObTableLoadStatusType::ABORT == status_)) {
      ret = OB_TRANS_KILLED;
      LOG_WARN("coordinator is abort", KR(ret));
    }
    // 正常运行阶段, 状态是一步步推进的
    else if (OB_UNLIKELY(static_cast<int64_t>(status) != static_cast<int64_t>(status_) + 1)) {
      ret = OB_STATE_NOT_MATCH;
      LOG_WARN("unexpected status", KR(ret), K(status), K(status_));
    } else {
      status_ = status;
      table_load_status_to_string(status_, ctx_->job_stat_->coordinator.status_);
      LOG_INFO("LOAD DATA COORDINATOR advance status", K(status));
    }
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::set_status_error(int error_code)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadCoordinatorCtx not init", KR(ret));
  } else if (OB_UNLIKELY(OB_SUCCESS == error_code)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", KR(ret), K(error_code));
  } else {
    obsys::ObWLockGuard guard(rwlock_);
    if (OB_UNLIKELY(status_ == ObTableLoadStatusType::ABORT)) {
      ret = OB_TRANS_KILLED;
    } else if (status_ != ObTableLoadStatusType::ERROR) {
      status_ = ObTableLoadStatusType::ERROR;
      error_code_ = error_code;
      table_load_status_to_string(status_, ctx_->job_stat_->coordinator.status_);
      LOG_INFO("LOAD DATA COORDINATOR status error", KR(error_code));
    }
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::set_status_abort()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadCoordinatorCtx not init", KR(ret));
  } else {
    obsys::ObWLockGuard guard(rwlock_);
    if (OB_UNLIKELY(status_ != ObTableLoadStatusType::ABORT)) {
      status_ = ObTableLoadStatusType::ABORT;
      table_load_status_to_string(status_, ctx_->job_stat_->coordinator.status_);
      LOG_INFO("LOAD DATA COORDINATOR status abort");
    }
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::check_status_unlock(ObTableLoadStatusType status) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(status != status_)) {
    if (ObTableLoadStatusType::ERROR == status_) {
      ret = error_code_;
    } else if (ObTableLoadStatusType::ABORT == status_) {
      ret = OB_CANCELED;
    } else {
      ret = OB_STATE_NOT_MATCH;
    }
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::check_status(ObTableLoadStatusType status) const
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadCoordinatorCtx not init", KR(ret));
  } else {
    obsys::ObRLockGuard guard(rwlock_);
    ret = check_status_unlock(status);
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::alloc_trans_ctx(const ObTableLoadTransId &trans_id,
                                               ObTableLoadTransCtx *&trans_ctx)
{
  int ret = OB_SUCCESS;
  trans_ctx = nullptr;
  // 分配trans_ctx
  if (OB_ISNULL(trans_ctx = ctx_->alloc_trans_ctx(trans_id))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc trans ctx", KR(ret), K(trans_id));
  }
  // 把trans_ctx插入map
  else if (OB_FAIL(trans_ctx_map_.set_refactored(trans_ctx->trans_id_, trans_ctx))) {
    LOG_WARN("fail to set trans ctx", KR(ret), K(trans_ctx->trans_id_));
  }
  if (OB_FAIL(ret)) {
    if (nullptr != trans_ctx) {
      ctx_->free_trans_ctx(trans_ctx);
      trans_ctx = nullptr;
    }
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::alloc_trans(const ObTableLoadSegmentID &segment_id,
                                           ObTableLoadCoordinatorTrans *&trans)
{
  int ret = OB_SUCCESS;
  trans = nullptr;
  const uint64_t trans_gid = ATOMIC_AAF(&last_trans_gid_, 1);
  const int32_t default_session_id =
    (ATOMIC_FAA(&next_session_id_, 1) % ctx_->param_.session_count_) + 1;
  ObTableLoadTransId trans_id(segment_id, trans_gid);
  ObTableLoadTransCtx *trans_ctx = nullptr;
  // 分配trans_ctx
  if (OB_FAIL(alloc_trans_ctx(trans_id, trans_ctx))) {
    LOG_WARN("fail to alloc trans ctx", KR(ret), K(trans_id));
  }
  // 构造trans
  else if (OB_ISNULL(trans = trans_allocator_.alloc(trans_ctx, default_session_id))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc ObTableLoadCoordinatorTrans", KR(ret));
  } else if (OB_FAIL(trans->init())) {
    LOG_WARN("fail to init trans", KR(ret), K(trans_id));
  } else if (OB_FAIL(trans_map_.set_refactored(trans_id, trans))) {
    LOG_WARN("fail to set_refactored", KR(ret), K(trans_id));
  }
  if (OB_FAIL(ret)) {
    if (nullptr != trans) {
      trans_allocator_.free(trans);
      trans = nullptr;
    }
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::start_trans(const ObTableLoadSegmentID &segment_id,
                                           ObTableLoadCoordinatorTrans *&trans)
{
  int ret = OB_SUCCESS;
  trans = nullptr;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadCoordinatorCtx not init", KR(ret));
  } else {
    obsys::ObWLockGuard guard(rwlock_);
    if (OB_FAIL(check_status_unlock(ObTableLoadStatusType::LOADING))) {
      LOG_WARN("fail to check status", KR(ret), K_(status));
    } else {
      SegmentCtx *segment_ctx = nullptr;
      if (OB_FAIL(segment_ctx_map_.get(segment_id, segment_ctx))) {
        if (OB_UNLIKELY(OB_ENTRY_NOT_EXIST != ret)) {
          LOG_WARN("fail to get segment ctx", KR(ret));
        } else {
          if (OB_FAIL(segment_ctx_map_.create(segment_id, segment_ctx))) {
            LOG_WARN("fail to create", KR(ret));
          }
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_UNLIKELY(nullptr != segment_ctx->current_trans_ ||
            nullptr != segment_ctx->committed_trans_ctx_)) {
          ret = OB_ENTRY_EXIST;
          LOG_WARN("trans already exist", KR(ret));
        } else {
          if (OB_FAIL(alloc_trans(segment_id, trans))) {
            LOG_WARN("fail to alloc trans", KR(ret));
          } else {
            segment_ctx->current_trans_ = trans;
            trans->inc_ref_count();
          }
        }
      }
      if (OB_NOT_NULL(segment_ctx)) {
        segment_ctx_map_.revert(segment_ctx);
      }
    }
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::commit_trans(ObTableLoadCoordinatorTrans *trans)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadCoordinatorCtx not init", KR(ret));
  } else if (OB_ISNULL(trans)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", KR(ret), KP(trans));
  } else {
    obsys::ObWLockGuard guard(rwlock_);
    const ObTableLoadSegmentID &segment_id = trans->get_trans_id().segment_id_;
    SegmentCtx *segment_ctx = nullptr;
    if (OB_FAIL(segment_ctx_map_.get(segment_id, segment_ctx))) {
      if (OB_UNLIKELY(OB_ENTRY_NOT_EXIST != ret)) {
        LOG_WARN("fail to get segment ctx", KR(ret));
      } else {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected trans", KR(ret));
      }
    } else if (OB_UNLIKELY(segment_ctx->current_trans_ != trans)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected trans", KR(ret));
    } else if (OB_FAIL(trans->check_trans_status(ObTableLoadTransStatusType::COMMIT))) {
      LOG_WARN("fail to check trans status commit", KR(ret));
    } else if (OB_FAIL(commited_trans_ctx_array_.push_back(trans->get_trans_ctx()))) {
      LOG_WARN("fail to push back trans ctx", KR(ret));
    } else {
      segment_ctx->current_trans_ = nullptr;
      segment_ctx->committed_trans_ctx_ = trans->get_trans_ctx();
      trans->set_dirty();
    }
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::abort_trans(ObTableLoadCoordinatorTrans *trans)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadCoordinatorCtx not init", KR(ret));
  } else if (OB_ISNULL(trans)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", KR(ret), KP(trans));
  } else {
    obsys::ObWLockGuard guard(rwlock_);
    const ObTableLoadSegmentID &segment_id = trans->get_trans_id().segment_id_;
    SegmentCtx *segment_ctx = nullptr;
    if (OB_FAIL(segment_ctx_map_.get(segment_id, segment_ctx))) {
      if (OB_UNLIKELY(OB_ENTRY_NOT_EXIST != ret)) {
        LOG_WARN("fail to get segment ctx", KR(ret));
      } else {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected trans", KR(ret));
      }
    } else if (OB_UNLIKELY(segment_ctx->current_trans_ != trans)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected trans", KR(ret));
    } else if (OB_FAIL(trans->check_trans_status(ObTableLoadTransStatusType::ABORT))) {
      LOG_WARN("fail to check trans status abort", KR(ret));
    } else {
      segment_ctx->current_trans_ = nullptr;
      trans->set_dirty();
    }
  }
  return ret;
}

void ObTableLoadCoordinatorCtx::put_trans(ObTableLoadCoordinatorTrans *trans)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadCoordinatorCtx not init", KR(ret));
  } else if (OB_ISNULL(trans)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", KR(ret), KP(trans));
  } else {
    ObTableLoadTransCtx *trans_ctx = trans->get_trans_ctx();
    if (0 == trans->dec_ref_count() && trans->is_dirty()) {
      ObTableLoadTransStatusType trans_status = trans_ctx->get_trans_status();
      OB_ASSERT(ObTableLoadTransStatusType::COMMIT == trans_status ||
                ObTableLoadTransStatusType::ABORT == trans_status);
      obsys::ObWLockGuard guard(rwlock_);
      if (OB_FAIL(trans_map_.erase_refactored(trans->get_trans_id()))) {
        LOG_WARN("fail to erase_refactored", KR(ret));
      } else {
        trans_allocator_.free(trans);
        trans = nullptr;
      }
    }
  }
  if (OB_FAIL(ret)) {
    set_status_error(ret);
  }
}

int ObTableLoadCoordinatorCtx::get_trans(const ObTableLoadTransId &trans_id,
                                         ObTableLoadCoordinatorTrans *&trans) const
{
  int ret = OB_SUCCESS;
  trans = nullptr;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadCoordinatorCtx not init", KR(ret));
  } else {
    obsys::ObRLockGuard guard(rwlock_);
    if (OB_FAIL(trans_map_.get_refactored(trans_id, trans))) {
      if (OB_UNLIKELY(OB_HASH_NOT_EXIST != ret)) {
        LOG_WARN("fail to get_refactored", KR(ret), K(trans_id));
      } else {
        ret = OB_ENTRY_NOT_EXIST;
      }
    } else {
      trans->inc_ref_count();
    }
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::get_trans_ctx(const ObTableLoadTransId &trans_id,
                                             ObTableLoadTransCtx *&trans_ctx) const
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadCoordinatorCtx not init", KR(ret));
  } else {
    obsys::ObRLockGuard guard(rwlock_);
    if (OB_FAIL(trans_ctx_map_.get_refactored(trans_id, trans_ctx))) {
      if (OB_UNLIKELY(OB_HASH_NOT_EXIST != ret)) {
        LOG_WARN("fail to get trans ctx", KR(ret), K(trans_id));
      } else {
        ret = OB_ENTRY_NOT_EXIST;
      }
    }
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::get_segment_trans_ctx(const ObTableLoadSegmentID &segment_id,
                                                     ObTableLoadTransCtx *&trans_ctx)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadCoordinatorCtx not init", KR(ret));
  } else {
    obsys::ObRLockGuard guard(rwlock_);
    SegmentCtx *segment_ctx = nullptr;
    if (OB_FAIL(segment_ctx_map_.get(segment_id, segment_ctx))) {
      if (OB_UNLIKELY(OB_ENTRY_NOT_EXIST != ret)) {
        LOG_WARN("fail to get segment ctx", KR(ret));
      }
    } else if (nullptr != segment_ctx->current_trans_) {
      trans_ctx = segment_ctx->current_trans_->get_trans_ctx();
    } else if (nullptr != segment_ctx->committed_trans_ctx_) {
      trans_ctx = segment_ctx->committed_trans_ctx_;
    } else {
      ret = OB_ENTRY_NOT_EXIST;
    }
    if (OB_NOT_NULL(segment_ctx)) {
      segment_ctx_map_.revert(segment_ctx);
    }
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::get_active_trans_ids(
  ObIArray<ObTableLoadTransId> &trans_id_array) const
{
  int ret = OB_SUCCESS;
  trans_id_array.reset();
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadCoordinatorCtx not init", KR(ret));
  } else {
    obsys::ObRLockGuard guard(rwlock_);
    for (TransMap::const_iterator trans_iter = trans_map_.begin();
         OB_SUCC(ret) && trans_iter != trans_map_.end(); ++trans_iter) {
      if (OB_FAIL(trans_id_array.push_back(trans_iter->first))) {
        LOG_WARN("fail to push back", KR(ret));
      }
    }
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::get_committed_trans_ids(
  ObTableLoadArray<ObTableLoadTransId> &trans_id_array, ObIAllocator &allocator) const
{
  int ret = OB_SUCCESS;
  trans_id_array.reset();
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadCoordinatorCtx not init", KR(ret));
  } else {
    obsys::ObRLockGuard guard(rwlock_);
    if (OB_FAIL(trans_id_array.create(commited_trans_ctx_array_.count(), allocator))) {
      LOG_WARN("fail to create trans id array", KR(ret));
    } else {
      for (int64_t i = 0; i < commited_trans_ctx_array_.count(); ++i) {
        ObTableLoadTransCtx *trans_ctx = commited_trans_ctx_array_.at(i);
        trans_id_array[i] = trans_ctx->trans_id_;
      }
    }
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::check_exist_trans(bool &is_exist) const
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadCoordinatorCtx not init", KR(ret));
  } else {
    obsys::ObRLockGuard guard(rwlock_);
    is_exist = !trans_map_.empty();
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::check_exist_committed_trans(bool &is_exist) const
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadCoordinatorCtx not init", KR(ret));
  } else {
    obsys::ObRLockGuard guard(rwlock_);
    is_exist = !commited_trans_ctx_array_.empty();
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::commit()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadCoordinatorCtx not init", KR(ret));
  } else if (OB_FAIL(check_status(ObTableLoadStatusType::MERGED))) {
    LOG_WARN("fail to check status", KR(ret));
  } else if (OB_FAIL(redef_table_.finish())){
    LOG_WARN("failed to finish redef table", KR(ret));
  }
  return ret;
}

int ObTableLoadCoordinatorCtx::abort()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableLoadCoordinatorCtx not init", KR(ret));
  } else if (OB_FAIL(redef_table_.abort())){
    LOG_WARN("failed to abort redef table", KR(ret));
  }
  return ret;
}

int64_t ObTableLoadCoordinatorCtx::get_ddl_task_id() const
{
  return redef_table_.get_ddl_task_id();
}
} // namespace observer
} // namespace oceanbase
