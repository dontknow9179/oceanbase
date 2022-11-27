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

#include "ob_ddl_redo_log_writer.h"
#include "logservice/archiveservice/ob_archive_service.h"
#include "logservice/ob_log_handler.h"
#include "logservice/ob_log_service.h"
#include "storage/blocksstable/ob_block_manager.h"
#include "storage/blocksstable/ob_index_block_builder.h"
#include "storage/blocksstable/ob_macro_block_struct.h"
#include "storage/ls/ob_ls.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "storage/tx/ob_ts_mgr.h"
#include "storage/ddl/ob_tablet_ddl_kv_mgr.h"
#include "observer/ob_server_event_history_table_operator.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;
using namespace oceanbase::archive;
using namespace oceanbase::blocksstable;
using namespace oceanbase::logservice;
using namespace oceanbase::share;

int ObDDLCtrlSpeedItem::init(const share::ObLSID &ls_id)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("inited twice", K(ret));
  } else if (OB_UNLIKELY(!ls_id.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ls id is invalid", K(ret), K(ls_id));
  } else {
    ls_id_ = ls_id;
    next_available_write_ts_ = ObTimeUtility::current_time();
    if (OB_FAIL(refresh())) {
      LOG_WARN("fail to init write speed and clog disk used threshold", K(ret));
    } else {
      is_inited_ = true;
      LOG_INFO("succeed to init ObDDLCtrlSpeedItem", K(ret), K(is_inited_), K(ls_id_), 
        K(next_available_write_ts_), K(write_speed_), K(disk_used_stop_write_threshold_));
    }
  }
  return ret;
}

// refrese ddl clog write speed and disk used threshold on tenant level.
int ObDDLCtrlSpeedItem::refresh()
{
  int ret = OB_SUCCESS;
  int64_t archive_speed = 0;
  int64_t refresh_speed = 0;
  bool ignore = false;
  bool force_wait = false;
  int64_t total_used_space = 0; // for current tenant, used bytes.
  int64_t total_disk_space = 0; // for current tenant, limit used bytes.
  ObLSHandle ls_handle;
  palf::PalfDiskOptions disk_opt;
  logservice::ObLogService *log_service = MTL(logservice::ObLogService*);
  ObArchiveService *archive_service = MTL(ObArchiveService*);
  if (OB_ISNULL(log_service) || OB_ISNULL(archive_service)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("error unexpected, nullptr found", K(ret), KP(log_service), KP(archive_service));
  } else if (OB_FAIL(MTL(ObLSService *)->get_ls(ls_id_, ls_handle, ObLSGetMod::STORAGE_MOD))) {
    if (OB_LS_NOT_EXIST == ret) {
      // log stream may be removed during timer refresh task.
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("fail to get ls", K(ret), K(ls_id_), K(MTL_ID()));
    }
  } else if (OB_FAIL(archive_service->get_ls_archive_speed(ls_id_,
                                                           archive_speed,
                                                           force_wait /* force_wait, which is unused */,
                                                           ignore))) {
    LOG_WARN("fail to get archive speed for ls", K(ret), K(ls_id_));
  } 

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(log_service->get_palf_disk_options(disk_opt))) {
    LOG_WARN("fail to get palf_disk_options", K(ret));
  } else if (OB_FAIL(log_service->get_palf_disk_usage(total_used_space, total_disk_space))) {
    STORAGE_LOG(WARN, "failed to get the disk space that clog used", K(ret));
  } else if (OB_ISNULL(GCTX.bandwidth_throttle_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error, bandwidth throttle is null", K(ret), KP(GCTX.bandwidth_throttle_));
  } else if (OB_FAIL(GCTX.bandwidth_throttle_->get_rate(refresh_speed))) {
    LOG_WARN("fail to get rate", K(ret), K(refresh_speed));
  } else {
    // archive is not on if ignore = true.
    write_speed_ = ignore ? std::max(refresh_speed, 1 * MIN_WRITE_SPEED) : std::max(archive_speed, 1 * MIN_WRITE_SPEED);
    disk_used_stop_write_threshold_ = (disk_opt.log_disk_utilization_threshold_ + disk_opt.log_disk_utilization_limit_threshold_) / 2;
    need_stop_write_ = 100.0 * total_used_space / total_disk_space >= disk_used_stop_write_threshold_ ? true : false;
  }
  LOG_DEBUG("current ddl clog write speed", K(ret), K(need_stop_write_), K(ls_id_), K(archive_speed), K(write_speed_), 
    K(total_used_space), K(total_disk_space), K(disk_used_stop_write_threshold_), K(refresh_speed));
  return ret;
}

// calculate the sleep time for the input bytes, and return next available write timestamp.
int ObDDLCtrlSpeedItem::cal_limit(const int64_t bytes, int64_t &next_available_ts)
{
  int ret = OB_SUCCESS;
  next_available_ts = 0;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (bytes < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid input bytes.", K(ret), K(bytes));
  } else if (write_speed_ < MIN_WRITE_SPEED) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected write speed", K(ret), K(write_speed_));
  }
  if (OB_SUCC(ret)) {
    const int64_t need_sleep_us = static_cast<int64_t>(1.0 * bytes / (write_speed_ * 1024 * 1024) * 1000 * 1000);
    int64_t tmp_us = 0;
    do {
      tmp_us = next_available_write_ts_;
      next_available_ts = std::max(ObTimeUtility::current_time(), next_available_write_ts_ + need_sleep_us);
    } while (!ATOMIC_BCAS(&next_available_write_ts_, tmp_us, next_available_ts));
  }
  return ret;
}

int ObDDLCtrlSpeedItem::do_sleep(
  const int64_t next_available_ts,
  int64_t &real_sleep_us)
{
  int ret = OB_SUCCESS;
  real_sleep_us = 0;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (next_available_ts <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument.", K(ret), K(next_available_ts));
  } else if (OB_UNLIKELY(need_stop_write_)) /*clog disk used exceeds threshold*/ {
    while (OB_SUCC(ret) && need_stop_write_) {
      // TODO YIREN (FIXME-20221017), exit when task is canceled, etc.
      ob_usleep(SLEEP_INTERVAL);
      if (REACH_TIME_INTERVAL(10 * 1000 * 1000)) {
        ObTaskController::get().allow_next_syslog();
        FLOG_INFO("stop write ddl clog", K(ret), K(ls_id_), 
          K(write_speed_), K(need_stop_write_), K(ref_cnt_), K(disk_used_stop_write_threshold_));
      }
    }
  }
  if (OB_SUCC(ret)) {
    real_sleep_us = std::max(0L, next_available_ts - ObTimeUtility::current_time());
    ob_usleep(real_sleep_us);
  }
  return ret;
}

// calculate the sleep time for the input bytes, sleep.
int ObDDLCtrlSpeedItem::limit_and_sleep(
  const int64_t bytes,
  int64_t &real_sleep_us)
{
  int ret = OB_SUCCESS;
  real_sleep_us = 0;
  int64_t next_available_ts = 0;
  int64_t transmit_sleep_us = 0; // network related.
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if ((disk_used_stop_write_threshold_ <= 0
      || disk_used_stop_write_threshold_ > 100) || bytes < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument.", K(ret), K(disk_used_stop_write_threshold_), K(bytes));
  } else if (OB_FAIL(cal_limit(bytes, next_available_ts))) {
    LOG_WARN("fail to calculate sleep time", K(ret), K(bytes), K(next_available_ts));
  } else if (OB_ISNULL(GCTX.bandwidth_throttle_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error, bandwidth throttle is null", K(ret), KP(GCTX.bandwidth_throttle_));
  } else if (OB_FAIL(GCTX.bandwidth_throttle_->limit_out_and_sleep(bytes,
                                                                   ObTimeUtility::current_time(),
                                                                   INT64_MAX,
                                                                   &transmit_sleep_us))) {
    LOG_WARN("fail to limit out and sleep", K(ret), K(bytes), K(transmit_sleep_us));
  } else if (OB_FAIL(do_sleep(next_available_ts, real_sleep_us))) {
    LOG_WARN("fail to sleep", K(ret), K(next_available_ts), K(real_sleep_us));
  } else {/* do nothing. */}
  return ret;
}

int ObDDLCtrlSpeedHandle::ObDDLCtrlSpeedItemHandle::set_ctrl_speed_item(
    ObDDLCtrlSpeedItem *item)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(item)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected err, item is nullptr", K(ret));
  } else {
    item->inc_ref();
    item_ = item;
  }
  return ret;
}

int ObDDLCtrlSpeedHandle::ObDDLCtrlSpeedItemHandle::get_ctrl_speed_item(
    ObDDLCtrlSpeedItem *&item) const
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(item_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error, speed handle item is nullptr", K(ret));
  } else {
    item = item_;
  }
  return ret;
}

void ObDDLCtrlSpeedHandle::ObDDLCtrlSpeedItemHandle::reset()
{
  if (nullptr != item_) {
    if (0 == item_->dec_ref()) {
      item_->~ObDDLCtrlSpeedItem();
    }
    item_ = nullptr;
  }
}

ObDDLCtrlSpeedHandle::ObDDLCtrlSpeedHandle()
  : is_inited_(false), speed_handle_map_(), allocator_("DDLClogCtrl"), bucket_lock_(), refreshTimerTask_()
{
}

ObDDLCtrlSpeedHandle::~ObDDLCtrlSpeedHandle()
{
  bucket_lock_.destroy();
  if (speed_handle_map_.created()) {
    speed_handle_map_.destroy();
  }
  allocator_.reset();
}

ObDDLCtrlSpeedHandle &ObDDLCtrlSpeedHandle::get_instance()
{
  static ObDDLCtrlSpeedHandle instance;
  return instance;
}

int ObDDLCtrlSpeedHandle::init()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("inited twice", K(ret));
  } else if (OB_UNLIKELY(speed_handle_map_.created())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error, speed handle map is created", K(ret));
  } else if (OB_FAIL(bucket_lock_.init(MAP_BUCKET_NUM))) {
    LOG_WARN("init bucket lock failed", K(ret));
  } else if (OB_FAIL(speed_handle_map_.create(MAP_BUCKET_NUM, "DDLSpeedCtrl"))) {
    LOG_WARN("fail to create speed handle map", K(ret));
  } else {
    is_inited_ = true;
    if (OB_FAIL(refreshTimerTask_.init(lib::TGDefIDs::ServerGTimer))) {
      LOG_WARN("fail to init refreshTimerTask", K(ret));
    } else {
      LOG_INFO("succeed to init ObDDLCtrlSpeedHandle", K(ret));
    }
  }
  return ret;
}

int ObDDLCtrlSpeedHandle::limit_and_sleep(
  const uint64_t tenant_id,
  const share::ObLSID &ls_id,
  const int64_t bytes, int64_t &real_sleep_us)
{
  int ret = OB_SUCCESS;
  SpeedHandleKey speed_handle_key;
  ObDDLCtrlSpeedItem *speed_handle_item = nullptr;
  ObDDLCtrlSpeedItemHandle item_handle;
  item_handle.reset();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if(OB_UNLIKELY(OB_INVALID_TENANT_ID == tenant_id || !ls_id.is_valid() || bytes < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id), K(ls_id), K(bytes));
  } else if (FALSE_IT(speed_handle_key.tenant_id_ = tenant_id)) {
  } else if (FALSE_IT(speed_handle_key.ls_id_ = ls_id)) {
  } else if (OB_UNLIKELY(!speed_handle_map_.created())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("speed handle map is not created", K(ret));
  } else if (OB_FAIL(add_ctrl_speed_item(speed_handle_key, item_handle))) {
    LOG_WARN("add speed item failed", K(ret));
  } else if (OB_FAIL(item_handle.get_ctrl_speed_item(speed_handle_item))) {
    LOG_WARN("get speed handle item failed", K(ret));
  } else if (OB_ISNULL(speed_handle_item)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected err, ctrl speed item is nullptr", K(ret), K(speed_handle_key));
  } else if (OB_FAIL(speed_handle_item->limit_and_sleep(bytes,
                                                        real_sleep_us))) {
    LOG_WARN("fail to limit and sleep", K(ret), K(bytes), K(real_sleep_us));
  }
  return ret;
}

// add entry in speed_handle_map if it does not exist.
// set entry in ctrl_speed_item_handle.
int ObDDLCtrlSpeedHandle::add_ctrl_speed_item(
    const SpeedHandleKey &speed_handle_key, 
    ObDDLCtrlSpeedItemHandle &item_handle)
{
  int ret = OB_SUCCESS;
  common::ObBucketHashWLockGuard guard(bucket_lock_, speed_handle_key.hash());
  char *buf = nullptr;
  ObDDLCtrlSpeedItem *speed_handle_item = nullptr;
  item_handle.reset();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!speed_handle_key.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ls id is invalid", K(ret), K(speed_handle_key));
  } else if (OB_UNLIKELY(!speed_handle_map_.created())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected, speed handle map is not created", K(ret));
  } else if (nullptr != speed_handle_map_.get(speed_handle_key)) {
    // do nothing, speed handle item has already exist.
  } else if (OB_ISNULL(buf = static_cast<char *>(allocator_.alloc(sizeof(ObDDLCtrlSpeedItem))))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to allocate memory", K(ret));
  } else {
    speed_handle_item = new (buf) ObDDLCtrlSpeedItem();
    if (OB_ISNULL(speed_handle_item)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected nullptr", K(ret));
    } else if (OB_FAIL(speed_handle_item->init(speed_handle_key.ls_id_))) {
        LOG_WARN("fail to init new speed handle item", K(ret), K(speed_handle_key));
    } else if (OB_FAIL(speed_handle_map_.set_refactored(speed_handle_key, speed_handle_item))) {
      LOG_WARN("fail to add speed handle item", K(ret), K(speed_handle_key));
    } else {
      speed_handle_item->inc_ref();
    }
  }

  // set entry for ctrl_speed_item_handle.
  if (OB_SUCC(ret)) {
    ObDDLCtrlSpeedItem *curr_speed_handle_item = nullptr;
    if (OB_FAIL(speed_handle_map_.get_refactored(speed_handle_key, curr_speed_handle_item))) {
      LOG_WARN("get refactored failed", K(ret), K(speed_handle_key));
    } else if (OB_ISNULL(curr_speed_handle_item)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected err, speed handle item is nullptr", K(ret), K(speed_handle_key));
    } else if (OB_FAIL(item_handle.set_ctrl_speed_item(curr_speed_handle_item))) {
      LOG_WARN("set ctrl speed item failed", K(ret), K(speed_handle_key));
    }
  }
  if (OB_FAIL(ret)) {
    if (nullptr != speed_handle_item) {
      speed_handle_item->~ObDDLCtrlSpeedItem();
      speed_handle_item = nullptr;
    }
    if (nullptr != buf) {
      allocator_.free(buf);
      buf = nullptr;
    }
  }
  return ret;
}

// remove entry from speed_handle_map.
int ObDDLCtrlSpeedHandle::remove_ctrl_speed_item(const SpeedHandleKey &speed_handle_key)
{
  int ret = OB_SUCCESS;
  common::ObBucketHashWLockGuard guard(bucket_lock_, speed_handle_key.hash());
  char *buf = nullptr;
  ObDDLCtrlSpeedItem *speed_handle_item = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!speed_handle_key.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ls id is invalid", K(ret), K(speed_handle_key));
  } else if (OB_UNLIKELY(!speed_handle_map_.created())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected, speed handle map is not created", K(ret));
  } else if (OB_FAIL(speed_handle_map_.get_refactored(speed_handle_key, speed_handle_item))) {
    LOG_WARN("get refactored failed", K(ret), K(speed_handle_key));
  } else if (OB_FAIL(speed_handle_map_.erase_refactored(speed_handle_key))) {
    LOG_WARN("fail to erase_refactored", K(ret), K(speed_handle_key));
  } else if (OB_ISNULL(speed_handle_item)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error, speed handle item is nullptr", K(ret), K(speed_handle_key));
  } else {
    if (0 == speed_handle_item->dec_ref()) {
      speed_handle_item->~ObDDLCtrlSpeedItem();
      speed_handle_item = nullptr;
    }
  }
  return ret;
}

// refresh speed_handle_map, including
// 1. remove speed_handle_item whose ls/tenant does not exist;
// 2. refresh write_speed_ and refresh disk_used_stop_write_threshold_.
int ObDDLCtrlSpeedHandle::refresh()
{
  int ret = OB_SUCCESS;
  // 1. remove speed_handle_item whose ls/tenant does not exist;
  for (hash::ObHashMap<SpeedHandleKey, ObDDLCtrlSpeedItem*>::const_iterator iter = speed_handle_map_.begin();
      OB_SUCC(ret) && iter != speed_handle_map_.end(); ++iter) {
    bool erase = false;
    const SpeedHandleKey &speed_handle_key = iter->first;
    if (OB_UNLIKELY(!speed_handle_key.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected, speed handle item in speed_handle_map is invalid", K(ret), K(speed_handle_key));
    } else {
      MTL_SWITCH(speed_handle_key.tenant_id_) {
        ObLSHandle ls_handle;
        if (OB_FAIL(MTL(ObLSService *)->get_ls(speed_handle_key.ls_id_, ls_handle, ObLSGetMod::STORAGE_MOD))) {
          if (OB_LS_NOT_EXIST == ret) {
            erase = true;
            ret = OB_SUCCESS;
          } else {
            LOG_WARN("fail to get ls", K(ret), K(speed_handle_key));
          }
        }
      } else {
        if (OB_TENANT_NOT_IN_SERVER == ret || OB_IN_STOP_STATE == ret) { // tenant deleted or on deleting
          ret = OB_SUCCESS;
          erase = true;
        } else {
          LOG_WARN("fail to switch tenant id", K(ret), K(MTL_ID()), K(speed_handle_key));
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (erase && OB_FAIL(remove_ctrl_speed_item(speed_handle_key))) {
      LOG_WARN("remove speed handle item failed", K(ret), K(speed_handle_key));
    }
  }
  
  // 2. update speed and disk config.
  if (OB_SUCC(ret)) {
    UpdateSpeedHandleItemFn update_speed_handle_item_fn;
    if (OB_FAIL(speed_handle_map_.foreach_refactored(update_speed_handle_item_fn))) {
      LOG_WARN("update write speed and disk config failed", K(ret));
    }
  }
  return ret;
}

// UpdateSpeedHandleItemFn update ddl clog write speed and disk used config
int ObDDLCtrlSpeedHandle::UpdateSpeedHandleItemFn::operator() (
    hash::HashMapPair<SpeedHandleKey, ObDDLCtrlSpeedItem*> &entry)
{
  int ret = OB_SUCCESS;
  MTL_SWITCH(entry.first.tenant_id_) {
    if (OB_ISNULL(entry.second)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected nulptr", K(ret), K(entry.first));
    } else if (OB_FAIL(entry.second->refresh())) {
      LOG_WARN("refresh speed and disk config failed", K(ret), K(entry));
    }
  } else if (OB_TENANT_NOT_IN_SERVER == ret || OB_IN_STOP_STATE == ret) { // tenant deleted or on deleting
    if (OB_ISNULL(entry.second)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected nulptr", K(ret), K(entry.first));
    } else {
      entry.second->reset_need_stop_write();
      ret = OB_SUCCESS;
    }
  } else {
    LOG_WARN("switch tenant id failed", K(ret), K(MTL_ID()), K(entry));
  }
  return ret;
}

// RefreshSpeedHandle Timer Task
ObDDLCtrlSpeedHandle::RefreshSpeedHandleTask::RefreshSpeedHandleTask()
  : is_inited_(false) {}

ObDDLCtrlSpeedHandle::RefreshSpeedHandleTask::~RefreshSpeedHandleTask()
{
  is_inited_ = false;
}

int ObDDLCtrlSpeedHandle::RefreshSpeedHandleTask::init(int tg_id)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret));
  } else {
    is_inited_ = true;
    if (OB_FAIL(TG_SCHEDULE(tg_id, *this, REFRESH_INTERVAL, true /* schedule repeatedly */))) {
      LOG_WARN("fail to schedule RefreshSpeedHandle Timer Task", K(ret));
    }
  }
  return ret;
}

void ObDDLCtrlSpeedHandle::RefreshSpeedHandleTask::runTimerTask()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("RefreshSpeedHandleTask not init", K(ret));
  } else if (OB_FAIL(ObDDLCtrlSpeedHandle::get_instance().refresh())) {
    LOG_WARN("fail to refresh SpeedHandleMap", K(ret));
  }
}

ObDDLRedoLogWriter::ObDDLRedoLogWriter() : is_inited_(false), bucket_lock_()
{
}

ObDDLRedoLogWriter::~ObDDLRedoLogWriter()
{
}

ObDDLRedoLogWriter &ObDDLRedoLogWriter::get_instance()
{
  static ObDDLRedoLogWriter instance;
  return instance;
}

int ObDDLRedoLogWriter::init()
{
  int ret = OB_SUCCESS;
  const int64_t bucket_num = 10243L;
  if (is_inited_) {
  } else if (OB_FAIL(bucket_lock_.init(bucket_num))) {
    LOG_WARN("init bucket lock failed", K(ret), K(bucket_num));
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObDDLRedoLogWriter::write(
  const ObDDLRedoLog &log,
  const uint64_t tenant_id,
  const share::ObLSID &ls_id,
  ObLogHandler *log_handler,
  const blocksstable::MacroBlockId &macro_block_id,
  char *buffer,
  ObDDLRedoLogHandle &handle)
{
  int ret = OB_SUCCESS;
  const enum ObReplayBarrierType replay_barrier_type = ObReplayBarrierType::NO_NEED_BARRIER;
  logservice::ObLogBaseHeader base_header(logservice::ObLogBaseType::DDL_LOG_BASE_TYPE,
                                          replay_barrier_type);
  ObDDLClogHeader ddl_header(ObDDLClogType::DDL_REDO_LOG);
  const int64_t buffer_size = base_header.get_serialize_size()
                              + ddl_header.get_serialize_size()
                              + log.get_serialize_size();
  int64_t pos = 0;
  ObDDLMacroBlockClogCb *cb = nullptr;

  palf::LSN lsn;
  const bool need_nonblock= false;
  const int64_t base_log_ts = 0;
  int64_t log_ts = 0;
  if (!log.is_valid() || nullptr == log_handler || !ls_id.is_valid() || OB_INVALID_TENANT_ID == tenant_id || nullptr == buffer) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(log), K(ls_id), K(tenant_id), KP(buffer));
  } else if (OB_ISNULL(cb = op_alloc(ObDDLMacroBlockClogCb))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc memory", K(ret));
  } else if (OB_FAIL(cb->init(ls_id, log.get_redo_info(), macro_block_id))) {
    LOG_WARN("init ddl clog callback failed", K(ret));
  } else if (OB_FAIL(base_header.serialize(buffer, buffer_size, pos))) {
    LOG_WARN("failed to serialize log base header", K(ret));
  } else if (OB_FAIL(ddl_header.serialize(buffer, buffer_size, pos))) {
    LOG_WARN("fail to seriaize ddl redo log", K(ret));
  } else if (OB_FAIL(log.serialize(buffer, buffer_size, pos))) {
    LOG_WARN("fail to seriaize ddl redo log", K(ret));
  } else if (OB_FAIL(log_handler->append(buffer,
                                        buffer_size,
                                        base_log_ts,
                                        need_nonblock,
                                        cb,
                                        lsn,
                                        log_ts))) {
    LOG_WARN("fail to submit ddl redo log", K(ret), K(buffer), K(buffer_size));
  } else {
    handle.cb_ = cb;
    cb = nullptr;
    handle.log_ts_ = log_ts;
    LOG_INFO("submit ddl redo log succeed", K(lsn), K(base_log_ts), K(log_ts));
  }
  if (OB_SUCC(ret)) {
    int64_t real_sleep_us = 0;
    if (OB_FAIL(ObDDLCtrlSpeedHandle::get_instance().limit_and_sleep(tenant_id, ls_id, buffer_size, real_sleep_us))) {
      LOG_WARN("fail to limit and sleep", K(ret), K(tenant_id), K(ls_id), K(buffer_size), K(real_sleep_us));
    }
  }
  if (OB_FAIL(ret)) {
    if (nullptr != cb) {
      op_free(cb);
      cb = nullptr;
    }
  }
  return ret;
}

int ObDDLRedoLogWriter::write_ddl_start_log(ObDDLKvMgrHandle &ddl_kv_mgr_handle,
                                            const ObDDLStartLog &log,
                                            ObLogHandler *log_handler,
                                            int64_t &start_log_ts)
{
  int ret = OB_SUCCESS;
  start_log_ts = 0;
  const enum ObReplayBarrierType replay_barrier_type = ObReplayBarrierType::STRICT_BARRIER;
  logservice::ObLogBaseHeader base_header(logservice::ObLogBaseType::DDL_LOG_BASE_TYPE,
                                          replay_barrier_type);
  ObDDLClogHeader ddl_header(ObDDLClogType::DDL_START_LOG);
  const int64_t buffer_size = base_header.get_serialize_size()
                              + ddl_header.get_serialize_size()
                              + log.get_serialize_size();
  char buffer[buffer_size];
  int64_t pos = 0;
  ObDDLClogCb *cb = nullptr;

  palf::LSN lsn;
  const bool need_nonblock= false;
  int64_t base_log_ts = 0;
  int64_t log_ts = 0;
  bool is_external_consistent = false;
  ObBucketHashWLockGuard guard(bucket_lock_, log.get_table_key().get_tablet_id().hash());
  if (ddl_kv_mgr_handle.get_obj()->is_execution_id_older(log.get_execution_id())) {
    ret = OB_TASK_EXPIRED;
    LOG_INFO("receive a old execution id, don't do ddl start", K(ret), K(log));
  } else if (OB_ISNULL(cb = op_alloc(ObDDLClogCb))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc memory", K(ret));
  } else if (OB_FAIL(base_header.serialize(buffer, buffer_size, pos))) {
    LOG_WARN("failed to serialize log base header", K(ret));
  } else if (OB_FAIL(ddl_header.serialize(buffer, buffer_size, pos))) {
    LOG_WARN("fail to seriaize ddl start log", K(ret));
  } else if (OB_FAIL(log.serialize(buffer, buffer_size, pos))) {
    LOG_WARN("fail to seriaize ddl start log", K(ret));
  } else if (OB_FAIL(log_handler->append(buffer,
                                        buffer_size,
                                        base_log_ts,
                                        need_nonblock,
                                        cb,
                                        lsn,
                                        log_ts))) {
    LOG_WARN("fail to submit ddl start log", K(ret), K(buffer_size));
    if (ObDDLUtil::need_remote_write(ret)) {
      ret = OB_NOT_MASTER;
      LOG_INFO("overwrite return to OB_NOT_MASTER");
    }
  } else {
    ObDDLClogCb *tmp_cb = cb;
    cb = nullptr;
    bool finish = false;
    const int64_t start_time = ObTimeUtility::current_time();
    while (OB_SUCC(ret) && !finish) {
      if (tmp_cb->is_success()) {
        finish = true;
      } else if (tmp_cb->is_failed()) {
        ret = OB_NOT_MASTER;
      }
      if (OB_SUCC(ret) && !finish) {
        const int64_t current_time = ObTimeUtility::current_time();
        if (current_time - start_time > ObDDLRedoLogHandle::DDL_REDO_LOG_TIMEOUT) {
          ret = OB_TIMEOUT;
          LOG_WARN("write ddl start log timeout", K(ret));
        } else {
          ob_usleep(ObDDLRedoLogHandle::CHECK_DDL_REDO_LOG_FINISH_INTERVAL);
        }
      }
    }
    if (OB_SUCC(ret)) {
      start_log_ts = log_ts;
      if (OB_FAIL(ddl_kv_mgr_handle.get_obj()->ddl_start(log.get_table_key(),
                                                          start_log_ts,
                                                          log.get_cluster_version(),
                                                          log.get_execution_id(),
                                                          0/*checkpoint_log_ts*/))) {
        LOG_WARN("start ddl log failed", K(ret), K(start_log_ts), K(log));
      }
    }
    tmp_cb->try_release(); // release the memory no matter succ or not
  }
  if (OB_FAIL(ret)) {
    if (nullptr != cb) {
      op_free(cb);
      cb = nullptr;
    }
  }
  return ret;
}

template <typename T>
int ObDDLRedoLogWriter::write_ddl_finish_log(const T &log, const ObDDLClogType clog_type, ObLogHandler *log_handler, ObDDLCommitLogHandle &handle)
{
  int ret = OB_SUCCESS;
  const enum ObReplayBarrierType replay_barrier_type = ObReplayBarrierType::PRE_BARRIER;
  DEBUG_SYNC(BEFORE_WRITE_DDL_PREPARE_LOG);
  logservice::ObLogBaseHeader base_header(logservice::ObLogBaseType::DDL_LOG_BASE_TYPE,
                                          replay_barrier_type);
  ObDDLClogHeader ddl_header(clog_type);
  char *buffer = nullptr;
  const int64_t buffer_size = base_header.get_serialize_size()
                              + ddl_header.get_serialize_size()
                              + log.get_serialize_size();
  int64_t pos = 0;
  ObDDLClogCb *cb = nullptr;

  palf::LSN lsn;
  const bool need_nonblock= false;
  int64_t base_log_ts = 0;
  int64_t log_ts = 0;
  bool is_external_consistent = false;
  if (OB_ISNULL(buffer = static_cast<char *>(ob_malloc(buffer_size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc memory", K(ret));
  } else if (OB_ISNULL(cb = op_alloc(ObDDLClogCb))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc memory", K(ret));
  } else if (OB_FAIL(base_header.serialize(buffer, buffer_size, pos))) {
    LOG_WARN("failed to serialize log base header", K(ret));
  } else if (OB_FAIL(ddl_header.serialize(buffer, buffer_size, pos))) {
    LOG_WARN("fail to seriaize ddl commit log", K(ret));
  } else if (OB_FAIL(log.serialize(buffer, buffer_size, pos))) {
    LOG_WARN("fail to seriaize ddl commit log", K(ret));
  } else if (OB_FAIL(OB_TS_MGR.get_ts_sync(MTL_ID(), ObDDLRedoLogHandle::DDL_REDO_LOG_TIMEOUT, base_log_ts, is_external_consistent))) {
    LOG_WARN("fail to get gts sync", K(ret), K(log));
  } else if (OB_FAIL(log_handler->append(buffer,
                                        buffer_size,
                                        base_log_ts,
                                        need_nonblock,
                                        cb,
                                        lsn,
                                        log_ts))) {
    LOG_WARN("fail to submit ddl commit log", K(ret), K(buffer), K(buffer_size));
  } else {
    ObDDLClogCb *tmp_cb = cb;
    cb = nullptr;
    bool need_retry = true;
    while (need_retry) {
      if (OB_FAIL(OB_TS_MGR.wait_gts_elapse(MTL_ID(), log_ts))) {
        if (OB_EAGAIN != ret) {
          LOG_WARN("fail to wait gts elapse", K(ret), K(log));
        } else {
          ob_usleep(1000);
        }
      } else {
        need_retry = false;
      }
    }
    if (OB_SUCC(ret)) {
      handle.cb_ = tmp_cb;
      handle.commit_log_ts_ = log_ts;
      LOG_INFO("submit ddl commit log succeed", K(lsn), K(base_log_ts), K(log_ts));
    } else {
      tmp_cb->try_release(); // release the memory
    }
  }
  if (nullptr != buffer) {
    ob_free(buffer);
    buffer = nullptr;
  }
  if (OB_FAIL(ret)) {
    if (nullptr != cb) {
      op_free(cb);
      cb = nullptr;
    }
  }
  return ret;
}

ObDDLRedoLogHandle::ObDDLRedoLogHandle()
  : cb_(nullptr), log_ts_(0)
{
}

ObDDLRedoLogHandle::~ObDDLRedoLogHandle()
{
  reset();
}

void ObDDLRedoLogHandle::reset()
{
  if (nullptr != cb_) {
    cb_->try_release();
    cb_ = nullptr;
  }
}

int ObDDLRedoLogHandle::wait(const int64_t timeout)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(cb_)) {
  } else {
    bool finish = false;
    const int64_t start_time = ObTimeUtility::current_time();
    while (OB_SUCC(ret) && !finish) {
      if (cb_->is_success()) {
        finish = true;
      } else if (cb_->is_failed()) {
        ret = OB_NOT_MASTER;
      }
      if (OB_SUCC(ret) && !finish) {
        const int64_t current_time = ObTimeUtility::current_time();
        if (current_time - start_time > timeout) {
          ret = OB_TIMEOUT;
          LOG_WARN("write ddl redo log timeout", K(ret));
        } else {
          ob_usleep(ObDDLRedoLogHandle::CHECK_DDL_REDO_LOG_FINISH_INTERVAL);
        }
      }
    }
  }
  return ret;
}

ObDDLCommitLogHandle::ObDDLCommitLogHandle()
  : cb_(nullptr), commit_log_ts_(0)
{
}

ObDDLCommitLogHandle::~ObDDLCommitLogHandle()
{
  reset();
}

int ObDDLCommitLogHandle::wait(const int64_t timeout)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(cb_)) {
  } else {
    bool finish = false;
    const int64_t start_time = ObTimeUtility::current_time();
    while (OB_SUCC(ret) && !finish) {
      if (cb_->is_success()) {
        finish = true;
      } else if (cb_->is_failed()) {
        ret = OB_NOT_MASTER;
      }
      if (OB_SUCC(ret) && !finish) {
        const int64_t current_time = ObTimeUtility::current_time();
        if (current_time - start_time > timeout) {
          ret = OB_TIMEOUT;
          LOG_WARN("write ddl commit log timeout", K(ret));
        } else {
          ob_usleep(ObDDLRedoLogHandle::CHECK_DDL_REDO_LOG_FINISH_INTERVAL);
        }
      }
    }
  }
  return ret;
}

void ObDDLCommitLogHandle::reset()
{
  int tmp_ret = OB_SUCCESS;
  if (nullptr != cb_) {
    cb_->try_release();
    cb_ = nullptr;
  }
}

int ObDDLMacroBlockRedoWriter::write_macro_redo(const ObDDLMacroBlockRedoInfo &redo_info,
                                                const share::ObLSID &ls_id,
                                                ObLogHandler *log_handler,
                                                const blocksstable::MacroBlockId &macro_block_id,
                                                char *buffer,
                                                ObDDLRedoLogHandle &handle)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!redo_info.is_valid() || nullptr == log_handler || nullptr == buffer)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(redo_info), KP(log_handler), KP(buffer));
  } else {
    ObDDLRedoLog log;
    const uint64_t tenant_id = MTL_ID();
    if (OB_FAIL(log.init(redo_info))) {
      LOG_WARN("fail to init DDLRedoLog", K(ret), K(redo_info));
    } else if (OB_FAIL(ObDDLRedoLogWriter::get_instance().write(log, tenant_id, ls_id, log_handler, macro_block_id, buffer, handle))) {
      LOG_WARN("fail to write ddl redo log item", K(ret));
    }
  }
  return ret;
}

int ObDDLMacroBlockRedoWriter::remote_write_macro_redo(const ObAddr &leader_addr,
                                                       const ObLSID &leader_ls_id,
                                                       const ObDDLMacroBlockRedoInfo &redo_info)
{
  int ret = OB_SUCCESS;
  obrpc::ObSrvRpcProxy *srv_rpc_proxy = nullptr;
  if (OB_UNLIKELY(!redo_info.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(redo_info));
  } else if (OB_ISNULL(srv_rpc_proxy = GCTX.srv_rpc_proxy_)) {
    ret = OB_ERR_SYS;
    LOG_WARN("srv rpc proxy is null", K(ret), KP(srv_rpc_proxy));
  } else {
    obrpc::ObRpcRemoteWriteDDLRedoLogArg arg;
    if (OB_FAIL(arg.init(MTL_ID(), leader_ls_id, redo_info))) {
      LOG_WARN("fail to init ObRpcRemoteWriteDDLRedoLogArg", K(ret));
    } else if (OB_FAIL(srv_rpc_proxy->to(leader_addr).remote_write_ddl_redo_log(arg))) {
      LOG_WARN("fail to remote write ddl redo log", K(ret), K(arg));
    }
  }
  return ret;
}

ObDDLSSTableRedoWriter::ObDDLSSTableRedoWriter()
  : is_inited_(false), remote_write_(false), start_log_ts_(0),
    ls_id_(), tablet_id_(), ddl_redo_handle_(), leader_addr_(), leader_ls_id_(), buffer_(nullptr)
{
}

int ObDDLSSTableRedoWriter::init(const ObLSID &ls_id, const ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObDDLSSTableRedoWriter has been inited twice", K(ret));
  } else if (OB_UNLIKELY(!ls_id.is_valid() || !tablet_id.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(ls_id), K(tablet_id));
  } else {
    ls_id_ = ls_id;
    tablet_id_ = tablet_id;
    is_inited_ = true;
  }
  return ret;
}

int ObDDLSSTableRedoWriter::start_ddl_redo(const ObITable::TableKey &table_key,
                                           const int64_t execution_id,
                                           ObDDLKvMgrHandle &ddl_kv_mgr_handle)
{
  int ret = OB_SUCCESS;
  ObLSHandle ls_handle;
  ObLS *ls = nullptr;
  ObTabletHandle tablet_handle;
  ObDDLStartLog log;
  ddl_kv_mgr_handle.reset();
  int64_t tmp_log_ts = 0;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLSSTableRedoWriter has not been inited", K(ret));
  } else if (OB_UNLIKELY(!table_key.is_valid() || execution_id <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(table_key), K(execution_id));
  } else if (OB_FAIL(log.init(table_key, GET_MIN_CLUSTER_VERSION(), execution_id))) {
    LOG_WARN("fail to init DDLStartLog", K(ret), K(table_key), K(execution_id), "cluster_version", GET_MIN_CLUSTER_VERSION());
  } else if (OB_FAIL(MTL(ObLSService *)->get_ls(ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("get ls failed", K(ret), K(ls_id_));
  } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("ls should not be null", K(ret), K(table_key));
  } else if (OB_FAIL(ls->get_tablet(tablet_id_, tablet_handle, ObTabletCommon::NO_CHECK_GET_TABLET_TIMEOUT_US))) {
    LOG_WARN("get tablet handle failed", K(ret), K(ls_id_), K(tablet_id_));
  } else if (OB_FAIL(tablet_handle.get_obj()->get_ddl_kv_mgr(ddl_kv_mgr_handle, true/*try_create*/))) {
    LOG_WARN("create ddl kv mgr failed", K(ret));
  } else if (OB_FAIL(ObDDLRedoLogWriter::get_instance().write_ddl_start_log(ddl_kv_mgr_handle, log, ls->get_log_handler(), tmp_log_ts))) {
    LOG_WARN("fail to write ddl start log", K(ret), K(table_key));
  } else if (FALSE_IT(set_start_log_ts(tmp_log_ts))) {
  } else if (OB_FAIL(ddl_kv_mgr_handle.get_obj()->register_to_tablet(get_start_log_ts(), ddl_kv_mgr_handle))) {
    LOG_WARN("register ddl kv mgr to tablet failed", K(ret), K(ls_id_), K(tablet_id_));
  }
  return ret;
}

int ObDDLSSTableRedoWriter::write_redo_log(const ObDDLMacroBlockRedoInfo &redo_info, const blocksstable::MacroBlockId &macro_block_id)
{
  int ret = OB_SUCCESS;
  ObLSHandle ls_handle;
  ObLS *ls = nullptr;
  const int64_t BUF_SIZE = 2 * 1024 * 1024 + 16 * 1024;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLSSTableRedoWriter has not been inited", K(ret));
  } else if (OB_UNLIKELY(!redo_info.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret));
  } else if (OB_FAIL(MTL(ObLSService *)->get_ls(ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("get ls failed", K(ret), K(ls_id_));
  } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("ls should not be null", K(ret));
  } else if (nullptr == buffer_ && OB_ISNULL(buffer_ = static_cast<char *>(ob_malloc(BUF_SIZE)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("allocate memory failed", K(ret), K(BUF_SIZE));
  } else if (!remote_write_) {
    if (OB_FAIL(ObDDLMacroBlockRedoWriter::write_macro_redo(redo_info, ls->get_ls_id(), ls->get_log_handler(), macro_block_id, buffer_, ddl_redo_handle_))) {
      if (ObDDLUtil::need_remote_write(ret)) {
        if (OB_FAIL(switch_to_remote_write())) {
          LOG_WARN("fail to switch to remote write", K(ret));
        }
      } else {
        LOG_WARN("fail to write ddl redo clog", K(ret));
      }
    }
  }

  if (OB_SUCC(ret) && remote_write_) {
    if (OB_FAIL(ObDDLMacroBlockRedoWriter::remote_write_macro_redo(leader_addr_,
                                                                   leader_ls_id_,
                                                                   redo_info))) {
      LOG_WARN("fail to remote write ddl redo log", K(ret), K_(leader_ls_id), K_(leader_addr));
    }
  }
  return ret;
}

int ObDDLSSTableRedoWriter::wait_redo_log_finish(const ObDDLMacroBlockRedoInfo &redo_info,
                                                 const blocksstable::MacroBlockId &macro_block_id)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLSSTableRedoWriter has not been inited", K(ret));
  } else if (remote_write_) {
    // remote write no need to wait local handle
  } else if (OB_UNLIKELY(!ddl_redo_handle_.is_valid())) {
    // no redo log has been written yet
  } else if (OB_FAIL(ddl_redo_handle_.wait())) {
    LOG_WARN("fail to wait io finish", K(ret));
  } else if (OB_FAIL(ddl_redo_handle_.cb_->get_ret_code())) {
    LOG_WARN("ddl redo callback executed failed", K(ret));
  } else {
    ddl_redo_handle_.reset();
  }
  return ret;
}

int ObDDLSSTableRedoWriter::write_prepare_log(const ObITable::TableKey &table_key,
                                              const int64_t table_id,
                                              const int64_t execution_id,
                                              const int64_t ddl_task_id,
                                              int64_t &prepare_log_ts)

{
  int ret = OB_SUCCESS;
#ifdef ERRSIM
  SERVER_EVENT_SYNC_ADD("storage_ddl", "before_write_prepare_log",
                        "table_key", table_key,
                        "table_id", table_id,
                        "execution_id", execution_id,
                        "ddl_task_id", ddl_task_id);
  DEBUG_SYNC(BEFORE_DDL_WRITE_PREPARE_LOG);
#endif
  prepare_log_ts = 0;
  ObLSHandle ls_handle;
  ObLS *ls = nullptr;
  ObDDLPrepareLog log;
  ObDDLCommitLogHandle handle;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLSSTableRedoWriter has not been inited", K(ret));
  } else if (OB_UNLIKELY(!table_key.is_valid() || 0 == start_log_ts_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(table_key), K(start_log_ts_));
  } else if (OB_FAIL(log.init(table_key, get_start_log_ts()))) {
    LOG_WARN("fail to init DDLCommitLog", K(ret), K(table_key), K(start_log_ts_));
  } else if (OB_FAIL(MTL(ObLSService *)->get_ls(ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("get ls failed", K(ret), K(ls_id_));
  } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("ls should not be null", K(ret), K(table_key));
  } else if (!remote_write_) {
    if (OB_FAIL(ObDDLRedoLogWriter::get_instance().write_ddl_finish_log(log, ObDDLClogType::DDL_PREPARE_LOG, ls->get_log_handler(), handle))) {
      if (ObDDLUtil::need_remote_write(ret)) {
        if (OB_FAIL(switch_to_remote_write())) {
          LOG_WARN("fail to switch to remote write", K(ret), K(table_key));
        }
      } else {
        LOG_WARN("fail to write ddl commit log", K(ret), K(table_key));
      }
    } else if (OB_FAIL(handle.wait())) {
      LOG_WARN("wait ddl commit log finish failed", K(ret), K(table_key));
    } else {
      prepare_log_ts = handle.get_commit_log_ts();
    }
  }
  if (OB_SUCC(ret) && remote_write_) {
    ObSrvRpcProxy *srv_rpc_proxy = GCTX.srv_rpc_proxy_;
    obrpc::ObRpcRemoteWriteDDLPrepareLogArg arg;
    obrpc::Int64 log_ts;
    if (OB_FAIL(arg.init(MTL_ID(), leader_ls_id_, table_key, get_start_log_ts(), table_id, execution_id, ddl_task_id))) {
      LOG_WARN("fail to init ObRpcRemoteWriteDDLPrepareLogArg", K(ret));
    } else if (OB_ISNULL(srv_rpc_proxy)) {
      ret = OB_ERR_SYS;
      LOG_WARN("srv rpc proxy or location service is null", K(ret), KP(srv_rpc_proxy));
    } else if (OB_FAIL(srv_rpc_proxy->to(leader_addr_).remote_write_ddl_prepare_log(arg, log_ts))) {
      LOG_WARN("fail to remote write ddl redo log", K(ret), K(arg));
    } else {
      prepare_log_ts = log_ts;
    }
  }
  return ret;
}

int ObDDLSSTableRedoWriter::write_commit_log(const ObITable::TableKey &table_key,
                                             const int64_t prepare_log_ts)
{
  int ret = OB_SUCCESS;
  ObLSHandle ls_handle;
  ObLS *ls = nullptr;
  ObDDLCommitLog log;
  ObDDLCommitLogHandle handle;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLSSTableRedoWriter has not been inited", K(ret));
  } else if (OB_UNLIKELY(!table_key.is_valid() || 0 == start_log_ts_ || prepare_log_ts <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(table_key), K(start_log_ts_), K(prepare_log_ts));
  } else if (OB_FAIL(log.init(table_key, get_start_log_ts(), prepare_log_ts))) {
    LOG_WARN("fail to init DDLCommitLog", K(ret), K(table_key), K(start_log_ts_), K(prepare_log_ts));
  } else if (OB_FAIL(MTL(ObLSService *)->get_ls(ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("get ls failed", K(ret), K(ls_id_));
  } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("ls should not be null", K(ret), K(table_key));
  } else if (!remote_write_) {
    if (OB_FAIL(ObDDLRedoLogWriter::get_instance().write_ddl_finish_log(log, ObDDLClogType::DDL_COMMIT_LOG, ls->get_log_handler(), handle))) {
      if (ObDDLUtil::need_remote_write(ret)) {
        if (OB_FAIL(switch_to_remote_write())) {
          LOG_WARN("fail to switch to remote write", K(ret), K(table_key));
        }
      } else {
        LOG_WARN("fail to write ddl commit log", K(ret), K(table_key));
      }
    } else if (OB_FAIL(handle.wait())) {
      LOG_WARN("wait ddl commit log finish failed", K(ret), K(table_key));
    }
  }
  if (OB_SUCC(ret) && remote_write_) {
    ObSrvRpcProxy *srv_rpc_proxy = GCTX.srv_rpc_proxy_;
    obrpc::ObRpcRemoteWriteDDLCommitLogArg arg;
    obrpc::Int64 log_ts;
    if (OB_FAIL(arg.init(MTL_ID(), leader_ls_id_, table_key, get_start_log_ts(), prepare_log_ts))) {
      LOG_WARN("fail to init ObRpcRemoteWriteDDLCommitLogArg", K(ret));
    } else if (OB_ISNULL(srv_rpc_proxy)) {
      ret = OB_ERR_SYS;
      LOG_WARN("srv rpc proxy or location service is null", K(ret), KP(srv_rpc_proxy));
    } else if (OB_FAIL(srv_rpc_proxy->to(leader_addr_).remote_write_ddl_commit_log(arg, log_ts))) {
      LOG_WARN("fail to remote write ddl redo log", K(ret), K(arg));
    }
  }
  return ret;
}

int ObDDLSSTableRedoWriter::switch_to_remote_write()
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = MTL_ID();
  share::ObLocationService *location_service = nullptr;
  bool is_cache_hit = false;
  if (OB_ISNULL(location_service = GCTX.location_service_)) {
    ret = OB_ERR_SYS;
    LOG_WARN("location service is null", K(ret), KP(location_service));
  } else if (OB_FAIL(location_service->get(tenant_id,
                                           tablet_id_,
                                           INT64_MAX/*expire_renew_time*/,
                                           is_cache_hit,
                                           leader_ls_id_))) {
    LOG_WARN("fail to get log stream id", K(ret), K_(tablet_id));
  } else if (OB_FAIL(location_service->get_leader(GCONF.cluster_id,
                                                  tenant_id,
                                                  leader_ls_id_,
                                                  true, /*force_renew*/
                                                  leader_addr_))) {
      LOG_WARN("get leader failed", K(ret), K(leader_ls_id_));
  } else {
    remote_write_ = true;
    LOG_INFO("switch to remote write", K(ret), K_(tablet_id));
  }
  return ret;
}

ObDDLSSTableRedoWriter::~ObDDLSSTableRedoWriter()
{
  if (nullptr != buffer_) {
    ob_free(buffer_);
    buffer_ = nullptr;
  }
}

ObDDLRedoLogWriterCallback::ObDDLRedoLogWriterCallback()
  : is_inited_(false), redo_info_(), table_key_(), macro_block_id_(), ddl_writer_(nullptr), block_buffer_(nullptr)
{
}

ObDDLRedoLogWriterCallback::~ObDDLRedoLogWriterCallback()
{
  if (nullptr != block_buffer_) {
    ob_free(block_buffer_);
    block_buffer_ = nullptr;
  }
}

int ObDDLRedoLogWriterCallback::init(const ObDDLMacroBlockType block_type,
                                     const ObITable::TableKey &table_key,
                                     ObDDLSSTableRedoWriter *ddl_writer)
{
  int ret = OB_SUCCESS;
  ObLS *ls = nullptr;
  ObLSService *ls_service = nullptr;
  bool is_cache_hit = false;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObDDLSSTableRedoWriter has been inited twice", K(ret));
  } else if (OB_UNLIKELY(!table_key.is_valid() || nullptr == ddl_writer || DDL_MB_INVALID_TYPE == block_type)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(table_key), K(block_type));
  } else {
    block_type_ = block_type;
    table_key_ = table_key;
    ddl_writer_ = ddl_writer;
    is_inited_ = true;
  }
  return ret;
}

int ObDDLRedoLogWriterCallback::write(const ObMacroBlockHandle &macro_handle,
                                      const ObLogicMacroBlockId &logic_id,
                                      char *buf,
                                      const int64_t data_seq)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLRedoLogWriterCallback is not inited", K(ret));
  } else if (OB_FAIL(prepare_block_buffer_if_need())) {
    LOG_WARN("prepare block buffer failed", K(ret));
  } else {
    macro_block_id_ = macro_handle.get_macro_id();
    redo_info_.table_key_ = table_key_;
    redo_info_.data_buffer_.assign(buf, OB_SERVER_BLOCK_MGR.get_macro_block_size());
    redo_info_.block_type_ = block_type_;
    redo_info_.logic_id_ = logic_id;
    redo_info_.start_log_ts_ = ddl_writer_->get_start_log_ts();
    if (OB_FAIL(ddl_writer_->write_redo_log(redo_info_, macro_block_id_))) {
      LOG_WARN("fail to write ddl redo log", K(ret));
    }
  }
  return ret;
}

int ObDDLRedoLogWriterCallback::wait()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLRedoLogWriterCallback is not inited", K(ret));
  } else if (OB_FAIL(ddl_writer_->wait_redo_log_finish(redo_info_, macro_block_id_))) {
    LOG_WARN("fail to wait redo log finish", K(ret));
  }
  return ret;
}

int ObDDLRedoLogWriterCallback::prepare_block_buffer_if_need()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(block_buffer_)) {
    block_buffer_ = static_cast<char *>(ob_malloc(OB_SERVER_BLOCK_MGR.get_macro_block_size(), "DDL_REDO_CB"));
    if (nullptr == block_buffer_) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("allocate memory for block bufffer failed", K(ret));
    }
  }
  return ret;
}
