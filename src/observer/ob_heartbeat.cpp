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

#define USING_LOG_PREFIX SERVER

#include "observer/ob_heartbeat.h"

#include "lib/mysqlclient/ob_mysql_transaction.h"
#include "share/ob_lease_struct.h"
#include "share/config/ob_server_config.h"
#include "share/config/ob_config_manager.h"
#include "share/ob_version.h"
#include "share/ob_zone_table_operation.h"
#include "storage/blocksstable/ob_block_manager.h"
#include "storage/ob_file_system_router.h"
#include "observer/omt/ob_multi_tenant.h"
#include "observer/omt/ob_tenant_node_balancer.h"
#include "observer/ob_server_schema_updater.h"
#include "observer/ob_server.h"
#include "observer/omt/ob_tenant_config_mgr.h"
#include "common/ob_timeout_ctx.h"

namespace oceanbase
{
namespace observer
{
using namespace storage;
using namespace blocksstable;
using namespace common;
using namespace share;
ObHeartBeatProcess::ObHeartBeatProcess(const ObGlobalContext &gctx,
                                       ObServerSchemaUpdater &schema_updater,
                                       ObLeaseStateMgr &lease_state_mgr)
  : inited_(false),
    update_task_(*this),
    zone_lease_info_(),
    newest_lease_info_version_(0),
    gctx_(gctx),
    schema_updater_(schema_updater),
    lease_state_mgr_(lease_state_mgr)
{
}

ObHeartBeatProcess::~ObHeartBeatProcess()
{
  TG_DESTROY(lib::TGDefIDs::ObHeartbeat);
}

int ObHeartBeatProcess::init()
{
  int ret = OB_SUCCESS;
  ObZone zone;
  const ObZone empty_zone = "";
  if (inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", KR(ret));
  } else if (!gctx_.is_inited()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("gctx not init", "gctx inited", gctx_.is_inited(), KR(ret));
  } else if (OB_FAIL(TG_START(lib::TGDefIDs::ObHeartbeat))) {
    LOG_WARN("fail to init timer", KR(ret));
  } else if (empty_zone == (zone = gctx_.config_->zone.str())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_ERROR("zone must not be empty", K(zone), KR(ret));
  } else {
    zone_lease_info_.zone_ = zone;
    inited_ = true;
  }
  return ret;
}


int ObHeartBeatProcess::init_lease_request(ObLeaseRequest &lease_request)
{
  int ret = OB_SUCCESS;
  omt::ObTenantNodeBalancer::ServerResource svr_res_assigned;
  common::ObArray<std::pair<uint64_t, uint64_t> > max_stored_versions;

  int64_t clog_free_size_byte = 0;
  int64_t clog_total_size_byte = 0;
  logservice::ObServerLogBlockMgr *log_block_mgr = GCTX.log_block_mgr_;

  if (!inited_ || OB_ISNULL(log_block_mgr)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init or log_block_mgr is null", KR(ret), K(inited_), K(GCTX.log_block_mgr_));
  } else if (OB_FAIL(omt::ObTenantNodeBalancer::get_instance().get_server_allocated_resource(svr_res_assigned))) {
    LOG_WARN("fail to get server allocated resource", KR(ret));
  } else if (OB_FAIL(log_block_mgr->get_disk_usage(clog_free_size_byte, clog_total_size_byte))) {
    LOG_WARN("Failed to get clog stat ", KR(ret));
  } else {
    lease_request.request_lease_time_ = 0; // this is not a valid member
    lease_request.version_ = ObLeaseRequest::LEASE_VERSION;
    lease_request.zone_ = gctx_.config_->zone.str();
    lease_request.server_ = gctx_.self_addr();
    lease_request.sql_port_ = gctx_.config_->mysql_port;
    lease_request.resource_info_.cpu_ = get_cpu_count();
    lease_request.resource_info_.report_cpu_assigned_ = svr_res_assigned.min_cpu_;
    lease_request.resource_info_.report_cpu_max_assigned_ = svr_res_assigned.max_cpu_;
    lease_request.resource_info_.report_mem_assigned_ = svr_res_assigned.memory_size_;
    lease_request.resource_info_.mem_in_use_ = 0;
    lease_request.resource_info_.mem_total_ = GMEMCONF.get_server_memory_avail();
    lease_request.resource_info_.disk_total_
        = OB_SERVER_BLOCK_MGR.get_total_macro_block_count() * OB_SERVER_BLOCK_MGR.get_macro_block_size();
    lease_request.resource_info_.disk_in_use_
        = OB_SERVER_BLOCK_MGR.get_used_macro_block_count() * OB_SERVER_BLOCK_MGR.get_macro_block_size();
    lease_request.resource_info_.log_disk_total_ = clog_total_size_byte;
    lease_request.resource_info_.report_log_disk_assigned_ = svr_res_assigned.log_disk_size_;
    get_package_and_svn(lease_request.build_version_, sizeof(lease_request.build_version_));
    OTC_MGR.get_lease_request(lease_request);
    lease_request.start_service_time_ = gctx_.start_service_time_;
    lease_request.ssl_key_expired_time_ = gctx_.ssl_key_expired_time_;
#ifdef ERRSIM
    common::ObZone err_zone("z3");
    const bool enable_disk_error_test = GCONF.enable_disk_error_test;
    lease_request.server_status_
      |= (err_zone == lease_request.zone_ && enable_disk_error_test) ? LEASE_REQUEST_DATA_DISK_ERROR : 0;
#else
    int tmp_ret = OB_SUCCESS;
    // TODO: add the func to check disk status
    const bool is_slog_disk_warning = false;
    ObDeviceHealthStatus dhs = DEVICE_HEALTH_NORMAL;
    int64_t abnormal_time = 0;
    if (OB_SUCCESS != (tmp_ret = ObIOManager::get_instance().get_device_health_status(dhs, abnormal_time))) {
      CLOG_LOG(WARN, "get device health status failed", K(tmp_ret));
    } else if (OB_UNLIKELY(DEVICE_HEALTH_ERROR == dhs) || OB_UNLIKELY(is_slog_disk_warning)) {
      const int64_t PRINT_LOG_INTERVAL_IN_US = 60 * 1000 * 1000; // 1min
      if (REACH_TIME_INTERVAL(PRINT_LOG_INTERVAL_IN_US)) {
        LOG_WARN("error occurs on data disk or slog disk",
            "data_disk_health_status", device_health_status_to_str(dhs), K(abnormal_time), K(is_slog_disk_warning));
      }
      if (OB_FILE_SYSTEM_ROUTER.is_single_zone_deployment_on()) {
        dhs = DEVICE_HEALTH_NORMAL; // ignore this error in scs single zone.
      }
    }
    const bool is_data_disk_error = (DEVICE_HEALTH_ERROR == dhs);
    lease_request.server_status_ |= (is_data_disk_error || is_slog_disk_warning) ? LEASE_REQUEST_DATA_DISK_ERROR : 0;
#endif

  }
  return ret;
}

//pay attention to concurrency control
int ObHeartBeatProcess::do_heartbeat_event(const ObLeaseResponse &lease_response)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret));
  } else if (!lease_response.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid lease_response", K(lease_response), KR(ret));
  } else if (ObLeaseResponse::LEASE_VERSION != lease_response.version_) {
    ret = OB_VERSION_NOT_MATCH;
    LOG_WARN("version mismatching", "version", lease_response.version_,
        LITERAL_K(ObLeaseResponse::LEASE_VERSION), KR(ret));
  } else {
    LOG_DEBUG("get lease_response", K(lease_response));

    if (OB_INVALID_ID != lease_response.server_id_) {
      if (GCTX.server_id_ != lease_response.server_id_) {
        LOG_INFO("receive new server id",
                 "old_id", GCTX.server_id_,
                 "new_id", lease_response.server_id_);
        GCTX.server_id_ = lease_response.server_id_;
      }
    }

    // update server status if needed
    if (RSS_INVALID != lease_response.rs_server_status_) {
      if (GCTX.rs_server_status_ != lease_response.rs_server_status_) {
        LOG_INFO("receive new server status recorded in rs",
                 "old_status", GCTX.rs_server_status_,
                 "new_status", lease_response.rs_server_status_);
        GCTX.rs_server_status_ = lease_response.rs_server_status_;
      }
    }
    // even try reload schema failed, we should continue do following things
    int schema_ret = schema_updater_.try_reload_schema(lease_response.refresh_schema_info_);

    if (OB_SUCCESS != schema_ret) {
      LOG_WARN("try reload schema failed", "schema_version", lease_response.schema_version_,
               "refresh_schema_info", lease_response.refresh_schema_info_, K(schema_ret));
    } else {
      LOG_INFO("try reload schema success", "schema_version", lease_response.schema_version_,
               "refresh_schema_info", lease_response.refresh_schema_info_, K(schema_ret));
    }

    const int64_t delay = 0;
    const bool repeat = false;
    // while rootservice startup, lease_info_version may be set to 0.
    if (lease_response.lease_info_version_ > 0) {
      newest_lease_info_version_ = lease_response.lease_info_version_;
    }
    bool is_exist = false;
    if (OB_FAIL(TG_TASK_EXIST(lib::TGDefIDs::ObHeartbeat, update_task_, is_exist))) {
      LOG_WARN("check exist failed", KR(ret));
    } else if (is_exist) {
      LOG_DEBUG("update task in scheduled, no need to schedule again");
    } else if (OB_FAIL(TG_SCHEDULE(lib::TGDefIDs::ObHeartbeat, update_task_, delay, repeat))) {
      LOG_WARN("schedule update zone lease info task failed", K(delay), K(repeat), KR(ret));
    }
    // generate the task for refreshing the Tenant-level configuration
    if (OB_SUCCESS != (tmp_ret = OTC_MGR.got_versions(lease_response.tenant_config_version_))) {
      LOG_WARN("tenant got versions failed", K(tmp_ret));
    }
  }
  return ret;
}

int ObHeartBeatProcess::update_lease_info()
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret));
  } else if (newest_lease_info_version_ == zone_lease_info_.lease_info_version_) {
    LOG_DEBUG("newest version lease info already got, no need to update",
        K_(newest_lease_info_version));
  } else if (newest_lease_info_version_ < zone_lease_info_.lease_info_version_) {
    ret = OB_ERR_SYS;
    LOG_WARN("newest_lease_info_version_ is smaller than old lease_info_version",
        K_(newest_lease_info_version),
        "lease_info_version", zone_lease_info_.lease_info_version_, KR(ret));
  } else if (OB_FAIL(ObZoneTableOperation::get_zone_lease_info(
      *GCTX.sql_proxy_, zone_lease_info_))) {
    LOG_WARN("get zone lease info failed", KR(ret));
  } else {
    LOG_INFO("succeed to update cluster_lease_info", K_(zone_lease_info));
  }
  return ret;
}

int ObHeartBeatProcess::try_update_infos()
{
  int ret = OB_SUCCESS;
  const int64_t config_version = zone_lease_info_.config_version_;

  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret));
  } else if (OB_FAIL(try_reload_config(config_version))) {
    LOG_WARN("try_reload_config failed", KR(ret), K(config_version));
  }

  return ret;
}

int ObHeartBeatProcess::try_reload_config(const int64_t config_version)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret));
  } else if (config_version < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid config_version", K(config_version), KR(ret));
  } else {
    ObConfigManager &config_mgr = *gctx_.config_mgr_;
    if (OB_FAIL(config_mgr.got_version(config_version, true))) {
      LOG_WARN("got_version failed", K(config_version), KR(ret));
    }
  }
  return ret;
}

ObHeartBeatProcess::ObZoneLeaseInfoUpdateTask::ObZoneLeaseInfoUpdateTask(
    ObHeartBeatProcess &hb_process)
  : hb_process_(hb_process)
{
}

ObHeartBeatProcess::ObZoneLeaseInfoUpdateTask::~ObZoneLeaseInfoUpdateTask()
{
}

void ObHeartBeatProcess::ObZoneLeaseInfoUpdateTask::runTimerTask()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(hb_process_.update_lease_info())) {
    LOG_WARN("update_lease_info failed", KR(ret));
  } else {
    // while rootservice startup, lease_info_version may be set to 0.
    if (OB_LIKELY(hb_process_.zone_lease_info_.lease_info_version_ > 0)) {
      if (OB_FAIL(hb_process_.try_update_infos())) {
        LOG_WARN("try_update_infos failed", KR(ret));
      }
    }
  }
}

}//end namespace observer
}//end namespace oceanbase
