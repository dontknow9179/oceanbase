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

#define USING_LOG_PREFIX STORAGE

#include "storage/ob_storage_table_guard.h"
#include "share/allocator/ob_memstore_allocator_mgr.h"
#include "storage/memtable/ob_memtable.h"
#include "storage/ob_i_table.h"
#include "storage/ob_relative_table.h"
#include "storage/tablet/ob_table_store_util.h"
#include "storage/tablet/ob_tablet.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "storage/tx_storage/ob_ls_handle.h" //ObLSHandle
#include "storage/tx_storage/ob_tenant_freezer.h"   

namespace oceanbase
{
namespace storage
{
ObStorageTableGuard::ObStorageTableGuard(
    ObTablet *tablet,
    ObStoreCtx &store_ctx,
    const bool need_control_mem,
    const bool for_replay,
    const int64_t replay_log_ts,
    const bool for_multi_source_data)
  : tablet_(tablet),
    store_ctx_(store_ctx),
    need_control_mem_(need_control_mem),
    memtable_(nullptr),
    retry_count_(0),
    last_ts_(0),
    for_replay_(for_replay),
    replay_log_ts_(replay_log_ts),
    for_multi_source_data_(for_multi_source_data)
{
  get_writing_throttling_sleep_interval() = 0;
}

ObStorageTableGuard::~ObStorageTableGuard()
{
  uint32_t &interval = get_writing_throttling_sleep_interval();
  if (need_control_mem_ && interval > 0) {
    uint64_t timeout = 10000;//10s
    common::ObWaitEventGuard wait_guard(common::ObWaitEventIds::MEMSTORE_MEM_PAGE_ALLOC_WAIT, timeout, 0, 0, interval);
    bool need_sleep = true;
    const int32_t SLEEP_INTERVAL_PER_TIME = 100 * 1000;//100ms
    int64_t left_interval = interval;
    if (!for_replay_) {
      int64_t query_left_time = store_ctx_.timeout_ - ObTimeUtility::current_time();
      left_interval = common::min(left_interval, query_left_time);
    }
    if (NULL != memtable_) {
      need_sleep = memtable_->is_active_memtable();
    }

    reset();
    int tmp_ret = OB_SUCCESS;
    bool has_sleep = false;
    
    while ((left_interval > 0) && need_sleep) {
      //because left_interval and SLEEP_INTERVAL_PER_TIME both are greater than
      //zero, so it's safe to convert to uint32_t, be careful with comparation between int and uint
      uint32_t sleep_interval = static_cast<uint32_t>(min(left_interval, SLEEP_INTERVAL_PER_TIME));
      if (for_replay_) {
        if(MTL(ObTenantFreezer *)->exist_ls_freezing()) {
          break;
        }
      }
      ob_usleep<common::ObWaitEventIds::STORAGE_WRITING_THROTTLE_SLEEP>(sleep_interval);
      has_sleep = true;
      left_interval -= sleep_interval;
      if (store_ctx_.mvcc_acc_ctx_.is_write()) {
        ObGMemstoreAllocator* memstore_allocator = NULL;
        if (OB_SUCCESS != (tmp_ret = ObMemstoreAllocatorMgr::get_instance().get_tenant_memstore_allocator(
            MTL_ID(), memstore_allocator))) {
        } else if (OB_ISNULL(memstore_allocator)) {
          LOG_WARN("get_tenant_mutil_allocator failed", K(store_ctx_.tablet_id_), K(tmp_ret));
        } else {
          need_sleep = memstore_allocator->need_do_writing_throttle();
        }
      }
    }
    if (for_replay_ && has_sleep) {
      // avoid print replay_timeout
      get_replay_is_writing_throttling() = true;
    }
  }
  reset();
}

int ObStorageTableGuard::refresh_and_protect_table(ObRelativeTable &relative_table)
{
  int ret = OB_SUCCESS;
  ObTabletTableIterator &iter = relative_table.tablet_iter_;
  const share::ObLSID &ls_id = tablet_->get_tablet_meta().ls_id_;
  const common::ObTabletID &tablet_id = tablet_->get_tablet_meta().tablet_id_;
  if (OB_ISNULL(store_ctx_.ls_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls is null", K(ret), K(ls_id));
  }

  while (OB_SUCC(ret) && need_to_refresh_table(iter.table_iter_)) {
    if (OB_FAIL(store_ctx_.ls_->get_tablet_svr()->get_read_tables(
        tablet_id,
        store_ctx_.mvcc_acc_ctx_.get_snapshot_version(),
        iter,
        relative_table.allow_not_ready()))) {
      LOG_WARN("fail to get read tables", K(ret), K(ls_id), K(tablet_id),
           "table id", relative_table.get_table_id());
    } else {
      // no worry. iter will hold tablet reference and its life cycle is longer than guard
      tablet_ = iter.tablet_handle_.get_obj();
      // TODO: check if seesion is killed
      if (store_ctx_.timeout_ > 0) {
        const int64_t query_left_time = store_ctx_.timeout_ - ObTimeUtility::current_time();
        if (query_left_time <= 0) {
          ret = OB_TRANS_STMT_TIMEOUT;
        }
      }
    }
  }

  return ret;
}

int ObStorageTableGuard::refresh_and_protect_memtable()
{
  int ret = OB_SUCCESS;
  ObIMemtableMgr *memtable_mgr = tablet_->get_memtable_mgr();
  memtable::ObIMemtable *memtable = nullptr;
  ObTableHandleV2 handle;
  const share::ObLSID &ls_id = tablet_->get_tablet_meta().ls_id_;
  const common::ObTabletID &tablet_id = tablet_->get_tablet_meta().tablet_id_;
  int64_t clog_checkpoint_ts = ObTabletMeta::INIT_CLOG_CHECKPOINT_TS;
  bool bool_ret = true;
  const int64_t start = ObTimeUtility::current_time();

  if (OB_ISNULL(memtable_mgr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("memtable mgr is null", K(ret), KP(memtable_mgr));
  } else {
    do {
      if (OB_FAIL(memtable_mgr->get_boundary_memtable(handle))) {
        // if there is no memtable, create a new one
        if (OB_ENTRY_NOT_EXIST == ret) {
          LOG_DEBUG("there is no boundary memtable", K(ret), K(ls_id), K(tablet_id));
          if (OB_FAIL(memtable_mgr->get_newest_clog_checkpoint_ts(clog_checkpoint_ts))) {
            LOG_WARN("fail to get newest clog_checkpoint_ts", K(ret), K(ls_id), K(tablet_id));
          } else if (replay_log_ts_ > clog_checkpoint_ts) {
            // TODO: get the newest schema_version from tablet
            ObLSHandle ls_handle;
            if (OB_FAIL(MTL(ObLSService *)->get_ls(ls_id, ls_handle, ObLSGetMod::STORAGE_MOD))) {
              LOG_WARN("failed to get log stream", K(ret), K(ls_id), K(tablet_id));
            } else if (OB_UNLIKELY(!ls_handle.is_valid())) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("unexpected error, invalid ls handle", K(ret), K(ls_handle), K(ls_id), K(tablet_id));
            } else if (OB_FAIL(ls_handle.get_ls()->get_tablet_svr()->create_memtable(
                                                                                     tablet_id, 0/*schema version*/, for_replay_))) {
              LOG_WARN("fail to create a boundary memtable", K(ret), K(ls_id), K(tablet_id));
            }
          } else { // replay_log_ts_ <= clog_checkpoint_ts
            // no need to create a boundary memtable
            ret = OB_SUCCESS;
            break;
          }
        } else { // OB_ENTRY_NOT_EXIST != ret
          LOG_WARN("fail to get boundary memtable", K(ret), K(ls_id), K(tablet_id));
        }
      } else if (OB_FAIL(handle.get_memtable(memtable))) {
        LOG_WARN("fail to get memtable from ObTableHandle", K(ret), K(ls_id), K(tablet_id));
      } else if (OB_FAIL(check_freeze_to_inc_write_ref(memtable, bool_ret))) {
        if (OB_MINOR_FREEZE_NOT_ALLOW != ret) {
          LOG_WARN("fail to check_freeze", K(ret), K(tablet_id), K(bool_ret), KPC(memtable));
        }
      } else {
        // do nothing
      }
      const int64_t cost_time = ObTimeUtility::current_time() - start;
      if (cost_time > 10 * 1000) {
        if (TC_REACH_TIME_INTERVAL(10 * 1000)) {
          TRANS_LOG(WARN, "refresh replay table too much times", K(ret),
                    K(ls_id), K(tablet_id), K(cost_time));
        }
      }
    } while ((OB_SUCC(ret) || OB_ENTRY_NOT_EXIST == ret) && bool_ret);
  }

  return ret;
}

void ObStorageTableGuard::reset()
{
  if (NULL != memtable_) {
    memtable_->dec_write_ref();
    memtable_ = NULL;
  }
}

void ObStorageTableGuard::double_check_inc_write_ref(
    const uint32_t old_freeze_flag,
    const bool is_tablet_freeze,
    memtable::ObIMemtable *memtable,
    bool &bool_ret)
{
  if (OB_ISNULL(memtable)) {
    LOG_WARN("memtable is null when inc write ref");
  } else {
    memtable->inc_write_ref();
    const uint32 new_freeze_flag = memtable->get_freeze_flag();
    const bool new_is_tablet_freeze = memtable->get_is_tablet_freeze();
    // do double-check to prevent concurrency problems
    if (old_freeze_flag != new_freeze_flag || is_tablet_freeze != new_is_tablet_freeze) {
      memtable->dec_write_ref();
    } else {
      bool_ret = false;
      memtable_ = memtable;
    }
  }
}

int ObStorageTableGuard::get_memtable_for_replay(memtable::ObIMemtable *&memtable)
{
  int ret = OB_SUCCESS;
  const common::ObTabletID &tablet_id = tablet_->get_tablet_meta().tablet_id_;
  memtable = nullptr;

  if (OB_NOT_NULL(memtable_)) {
    memtable = memtable_;
  } else {
    ret = OB_NO_NEED_UPDATE;
  }

  return ret;
}

int ObStorageTableGuard::check_freeze_to_inc_write_ref(ObITable *table, bool &bool_ret)
{
  int ret = OB_SUCCESS;
  bool_ret = true;
  const share::ObLSID &ls_id = tablet_->get_tablet_meta().ls_id_;
  const common::ObTabletID &tablet_id = tablet_->get_tablet_meta().tablet_id_;
  ObIMemtableMgr *memtable_mgr = tablet_->get_memtable_mgr();
  // need to make sure the memtable is a right boundary memtable
  memtable::ObIMemtable *memtable = static_cast<memtable::ObIMemtable *>(table);
  memtable::ObIMemtable *old_memtable = memtable;
  // get freeze_flag before memtable->is_active_memtable() to double-check
  // prevent that the memtable transforms from active to frozen before inc_write_ref
  uint32_t old_freeze_flag = 0;
  bool is_tablet_freeze = false;

  if (OB_ISNULL(memtable_mgr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("memtable mgr is null", K(ret), K(bool_ret), K(ls_id), K(tablet_id), KP(memtable_mgr));
  } else if (OB_ISNULL(table)) {
    LOG_INFO("table is null, need to refresh", K(bool_ret), K(ls_id), K(tablet_id));
  } else if (FALSE_IT(old_freeze_flag = memtable->get_freeze_flag())) {
  } else if (FALSE_IT(is_tablet_freeze = memtable->get_is_tablet_freeze())) {
  } else if (memtable->is_active_memtable()) {
    // the most recent memtable is active
    // no need to create a new memtable
    if (for_replay_ || for_multi_source_data_) {
      // filter memtables for replay or multi_source_data according to log_ts
      ObTableHandleV2 handle;
      if (OB_FAIL(memtable_mgr->get_memtable_for_replay(replay_log_ts_, handle))) {
        if (OB_NO_NEED_UPDATE == ret) {
          // no need to replay the log
          bool_ret = false;
          ret = OB_SUCCESS;
        } else {
          LOG_WARN("fail to get memtable for replay", K(ret), K(bool_ret), K(ls_id), K(tablet_id));
        }
      } else if (OB_FAIL(handle.get_memtable(memtable))) {
        LOG_WARN("fail to get memtable from ObTableHandle", K(ret), K(bool_ret), K(ls_id), K(tablet_id));
      } else {
        if (memtable != old_memtable) {
          is_tablet_freeze = memtable->get_is_tablet_freeze();
        }
        double_check_inc_write_ref(old_freeze_flag, is_tablet_freeze, memtable, bool_ret);
      }
    } else {
      double_check_inc_write_ref(old_freeze_flag, is_tablet_freeze, memtable, bool_ret);
    }
  } else {
    // the most recent memtable is frozen
    // need to create a new memtable
    // TODO: allow to write frozen memtables except for the boundary one when replaying
    const int64_t write_ref = memtable->get_write_ref();
    if (write_ref == 0) {
      // create a new memtable if no write in the old memtable
      ObLSHandle ls_handle;
      if (OB_FAIL(MTL(ObLSService *)->get_ls(ls_id, ls_handle, ObLSGetMod::STORAGE_MOD))) {
        LOG_WARN("failed to get log stream", K(ret), K(bool_ret), K(ls_id), K(tablet_id));
      } else if (OB_UNLIKELY(!ls_handle.is_valid())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected error, invalid ls handle", K(ret), K(bool_ret), K(ls_handle), K(ls_id), K(tablet_id));
      } else if (OB_FAIL(ls_handle.get_ls()->get_tablet_svr()->create_memtable(tablet_id,
          memtable->get_max_schema_version()/*schema version*/, for_replay_))) {
        if (OB_MINOR_FREEZE_NOT_ALLOW != ret) {
          LOG_WARN("fail to create new memtable for freeze", K(ret), K(bool_ret), K(ls_id), K(tablet_id));
        }
      }
    }
  }

  return ret;
}

bool ObStorageTableGuard::need_to_refresh_table(ObTableStoreIterator &iter)
{
  int ret = OB_SUCCESS;
  bool bool_ret = false;
  int exit_flag = -1;

  ObITable *table = iter.get_boundary_table(true);
  if (NULL == table || !table->is_memtable()) {
    // TODO: get the newest schema_version from tablet
    const common::ObTabletID &tablet_id = tablet_->get_tablet_meta().tablet_id_;
    if (OB_FAIL(store_ctx_.ls_->get_tablet_svr()->create_memtable(tablet_id, 0/*schema version*/, for_replay_))) {
      LOG_WARN("fail to create a boundary memtable", K(ret), K(tablet_id));
    }
    bool_ret = true;
    exit_flag = 0;
  } else if (!table->is_data_memtable()) {
    bool_ret = false;
  } else if (iter.check_store_expire()) {
    bool_ret = true;
    exit_flag = 1;
  } else if (OB_FAIL(check_freeze_to_inc_write_ref(table, bool_ret))) {
    bool_ret = true;
    exit_flag = 2;
    if (OB_MINOR_FREEZE_NOT_ALLOW != ret) {
      LOG_WARN("fail to inc write ref", K(ret));
    }
  } else {
    exit_flag = 3;
  }

  if (bool_ret && check_if_need_log()) {
    const share::ObLSID &ls_id = tablet_->get_tablet_meta().ls_id_;
    const common::ObTabletID &tablet_id = tablet_->get_tablet_meta().tablet_id_;
    LOG_WARN("refresh table too much times", K(ret), K(exit_flag), K(ls_id), K(tablet_id), KP(table));
    if (0 == exit_flag) {
      LOG_WARN("table is null or not memtable", K(ret), K(ls_id), K(tablet_id), KP(table));
    } else if (1 == exit_flag) {
      LOG_WARN("iterator store is expired", K(ret), K(ls_id), K(tablet_id), K(iter.check_store_expire()), K(iter.count()), K(iter));
    } else if (2 == exit_flag) {
      LOG_WARN("failed to check_freeze_to_inc_write_ref", K(ret), K(ls_id), K(tablet_id), KPC(table));
    } else if (3 == exit_flag) {
      LOG_WARN("check_freeze_to_inc_write_ref costs too much time", K(ret), K(ls_id), K(tablet_id), KPC(table));
    } else {
      LOG_WARN("unexpect exit_flag", K(exit_flag), K(ret), K(ls_id), K(tablet_id));
    }
  }

  return bool_ret;
}

bool ObStorageTableGuard::check_if_need_log()
{
  bool need_log = false;
  if ((++retry_count_ % GET_TS_INTERVAL) == 0) {
    const int64_t cur_ts = common::ObTimeUtility::current_time();
    if (0 >= last_ts_) {
      last_ts_ = cur_ts;
    } else if (cur_ts - last_ts_ >= LOG_INTERVAL_US) {
      last_ts_ = cur_ts;
      need_log = true;
    } else {
      // do nothing
    }
  }
  return need_log;
}
} // namespace storage
} // namespace oceanbase
