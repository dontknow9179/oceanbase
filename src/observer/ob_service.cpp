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

#include "observer/ob_service.h"

#include <new>
#include <string.h>
#include <cmath>

#include "share/ob_define.h"
#include "lib/ob_running_mode.h"
#include "lib/utility/utility.h"
#include "lib/utility/ob_tracepoint.h"
#include "lib/thread_local/ob_tsi_factory.h"
#include "lib/utility/utility.h"
#include "lib/time/ob_tsc_timestamp.h"

#include "common/ob_member_list.h"
#include "common/ob_zone.h"
#include "share/ob_version.h"

#include "share/ob_version.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "share/deadlock/ob_deadlock_inner_table_service.h"
#include "share/ob_tenant_mgr.h"
#include "share/ob_zone_table_operation.h"
#include "share/tablet/ob_tablet_info.h" // for ObTabletReplica
#include "share/ob_tablet_replica_checksum_operator.h" // ObTabletReplicaChecksumItem
#include "share/rc/ob_tenant_base.h"

#include "storage/ob_partition_component_factory.h"
#include "storage/ob_i_table.h"
#include "storage/tx/ob_trans_service.h"
#include "sql/optimizer/ob_storage_estimator.h"
#include "sql/optimizer/ob_opt_est_cost.h"
#include "sql/optimizer/ob_join_order.h"
#include "rootserver/ob_bootstrap.h"
#include "observer/ob_server.h"
#include "observer/ob_dump_task_generator.h"
#include "observer/ob_server_schema_updater.h"
#include "ob_server_event_history_table_operator.h"
#include "share/ob_alive_server_tracer.h"
#include "storage/ddl/ob_complement_data_task.h" // complement data for drop column
#include "storage/ddl/ob_ddl_merge_task.h"
#include "storage/ddl/ob_build_index_task.h"
#include "storage/tablet/ob_tablet_multi_source_data.h"
#include "storage/tx_storage/ob_tenant_freezer.h"
#include "storage/tx_storage/ob_ls_map.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "storage/tx_storage/ob_checkpoint_service.h"
#include "storage/ls/ob_ls.h"
#include "logservice/ob_log_service.h"        // ObLogService
#include "logservice/palf_handle_guard.h"     // PalfHandleGuard
#include "storage/backup/ob_backup_handler.h"
#include "storage/backup/ob_ls_backup_clean_mgr.h"
#include "storage/ob_file_system_router.h"
#include "share/backup/ob_backup_path.h"
#include "share/backup/ob_backup_connectivity.h"
#include "observer/report/ob_tenant_meta_checker.h"//ObTenantMetaChecker
#include "storage/compaction/ob_tenant_tablet_scheduler.h"

namespace oceanbase
{

using namespace common;
using namespace rootserver;
using namespace obrpc;
using namespace share;
using namespace share::schema;
using namespace storage;
using namespace backup;

namespace observer
{


ObSchemaReleaseTimeTask::ObSchemaReleaseTimeTask()
: schema_updater_(nullptr), is_inited_(false)
{}

int ObSchemaReleaseTimeTask::init(ObServerSchemaUpdater &schema_updater, int tg_id)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObSchemaReleaseTimeTask has already been inited", K(ret));
  } else {
    schema_updater_ = &schema_updater;
    is_inited_ = true;
    if (OB_FAIL(TG_SCHEDULE(tg_id, *this, REFRESH_INTERVAL, true /*schedule repeatly*/))) {
      LOG_WARN("fail to schedule task ObSchemaReleaseTimeTask", K(ret));
    }
  }
  return ret;
}

void ObSchemaReleaseTimeTask::destroy()
{
  is_inited_ = false;
  schema_updater_ = nullptr;
}

void ObSchemaReleaseTimeTask::runTimerTask()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObSchemaReleaseTimeTask has not been inited", K(ret));
  } else if (OB_ISNULL(schema_updater_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ObSchemaReleaseTimeTask task got null ptr", K(ret));
  } else if (OB_FAIL(schema_updater_->try_release_schema())) {
    LOG_WARN("ObSchemaReleaseTimeTask failed", K(ret));
  }
}

ObRemoteMasterRsUpdateTask::ObRemoteMasterRsUpdateTask(const ObGlobalContext &gctx)
  : gctx_(gctx), is_inited_(false)
{}

int ObRemoteMasterRsUpdateTask::init(int tg_id)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObRemoteMasterRsUpdateTask has already been inited", KR(ret));
  } else if (OB_FAIL(TG_SCHEDULE(tg_id, *this, REFRESH_INTERVAL, true /*schedule repeatly*/))) {
    LOG_WARN("fail to schedule task ObSchemaReleaseTimeTask", KR(ret));
  } else {
    is_inited_ = true;
  }
  return ret;
}

void ObRemoteMasterRsUpdateTask::runTimerTask()
{
  int ret = OB_SUCCESS;
  if (!is_inited_ || OB_ISNULL(gctx_.rs_mgr_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("rs_mgr is not init yet", KR(ret), K_(is_inited));
  } else if (OB_FAIL(gctx_.rs_mgr_->renew_remote_master_rootserver())) {
    LOG_WARN("renew remote rs master failed", KR(ret));
  }
}

//////////////////////////////////////

// here gctx may hasn't been initialized already
ObService::ObService(const ObGlobalContext &gctx)
    : inited_(false), in_register_process_(false),
    service_started_(false), stopped_(false),
    schema_updater_(),
    lease_state_mgr_(), heartbeat_process_(gctx, schema_updater_, lease_state_mgr_),
    gctx_(gctx), server_trace_task_(), schema_release_task_(),
    schema_status_task_(), remote_master_rs_update_task_(gctx), ls_table_updater_(),
    tablet_table_updater_(), meta_table_checker_()
  {
  }

ObService::~ObService()
{
}

int ObService::init(common::ObMySQLProxy &sql_proxy,
                    share::ObIAliveServerTracer &server_tracer)
{
  int ret = OB_SUCCESS;
  FLOG_INFO("[OBSERVICE_NOTICE] init ob_service begin");
  const static int64_t REBUILD_FLAG_REPORT_THREAD_CNT = 1;

  if (inited_) {
    ret = OB_INIT_TWICE;
    FLOG_WARN("Oceanbase service has already init", KR(ret));
  } else if (!gctx_.is_inited()) {
    ret = OB_INVALID_ARGUMENT;
    FLOG_WARN("gctx not init", "gctx inited", gctx_.is_inited(), KR(ret));
  } else if (OB_FAIL(heartbeat_process_.init())) {
    FLOG_WARN("heartbeat_process_.init failed", KR(ret));
  } else if (OB_FAIL(schema_updater_.init(gctx_.self_addr(), gctx_.schema_service_))) {
    FLOG_WARN("client_manager_.initialize failed", "self_addr", gctx_.self_addr(), KR(ret));
  } else if (OB_FAIL(SERVER_EVENT_INSTANCE.init(sql_proxy, gctx_.self_addr()))) {
    FLOG_WARN("init server event history table failed", KR(ret));
  } else if (OB_FAIL(DEALOCK_EVENT_INSTANCE.init(sql_proxy))) {
    FLOG_WARN("init deadlock event history cleaner failed", KR(ret));
  } else if (OB_FAIL(ObAllServerTracer::get_instance().init(lib::TGDefIDs::ServerGTimer, server_trace_task_))) {
    FLOG_WARN("init ObAllServerTracer failed", KR(ret));
  } else if (OB_FAIL(OB_TSC_TIMESTAMP.init())) {
    FLOG_WARN("init tsc timestamp failed", KR(ret));
  } else if (OB_FAIL(schema_release_task_.init(schema_updater_, lib::TGDefIDs::ServerGTimer))) {
    FLOG_WARN("init schema release task failed", KR(ret));
  } else if (OB_FAIL(remote_master_rs_update_task_.init(lib::TGDefIDs::ServerGTimer))) {
    FLOG_WARN("init remote master rs update task failed", KR(ret));
  } else if (OB_FAIL(tablet_table_updater_.init(*this, *gctx_.tablet_operator_))) {
    FLOG_WARN("init tablet table updater failed", KR(ret));
  } else if (OB_FAIL(ls_table_updater_.init())) {
    FLOG_WARN("init log stream table updater failed", KR(ret));
  } else if (OB_FAIL(meta_table_checker_.init(
      gctx_.lst_operator_,
      gctx_.tablet_operator_,
      gctx_.omt_,
      gctx_.schema_service_))) {
    FLOG_WARN("init meta table checker failed", KR(ret));
  } else {
    inited_ = true;
  }
  FLOG_INFO("[OBSERVICE_NOTICE] init ob_service finish", KR(ret), K_(inited));
  return ret;
}

int ObService::register_self()
{
  int ret = OB_SUCCESS;
  in_register_process_ = true;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_ERROR("service not initialized, can't register self", KR(ret));
  } else if (!lease_state_mgr_.is_inited()) {
    ret = OB_NOT_INIT;
    LOG_WARN("lease_state_mgr_ not init", KR(ret));
  } else if (OB_FAIL(lease_state_mgr_.register_self_busy_wait())) {
    LOG_WARN("register self failed", KR(ret));
  } else if (!lease_state_mgr_.is_valid_heartbeat()) {
    ret = OB_ERROR;
    LOG_ERROR("can't renew lease", KR(ret),
              "heartbeat_expire_time", lease_state_mgr_.get_heartbeat_expire_time());
  } else {
    in_register_process_ = false;
    service_started_ = true;
    SERVER_EVENT_ADD("observice", "register");
  }
  return ret;
}

int ObService::start()
{
  int ret = OB_SUCCESS;
  FLOG_INFO("[OBSERVICE_NOTICE] start ob_service begin");
  if (!inited_) {
    ret = OB_NOT_INIT;
    FLOG_WARN("ob_service is not inited", KR(ret), K_(inited));
  } else if (OB_FAIL(lease_state_mgr_.init(gctx_.rs_rpc_proxy_,
                                           gctx_.rs_mgr_,
                                           &heartbeat_process_,
                                           *this))) {
    LOG_ERROR("init lease_state_mgr_ failed", KR(ret));
  } else if (OB_FAIL(register_self())) {
    LOG_ERROR("register self failed", KR(ret));
  } else {
    FLOG_INFO("regist to rs success");
  }
  if (FAILEDx(meta_table_checker_.start())) {
    LOG_ERROR("start meta table checker failed", KR(ret));
  }
  FLOG_INFO("[OBSERVICE_NOTICE] start ob_service end", KR(ret));
  return ret;
}


void ObService::set_stop()
{
  LOG_INFO("[OBSERVICE_NOTICE] observice need stop now");
  lease_state_mgr_.set_stop();
}

void ObService::stop()
{
  FLOG_INFO("[OBSERVICE_NOTICE] start to stop observice");
  if (!inited_) {
    FLOG_WARN("ob_service not init", K_(inited));
  } else {
    FLOG_INFO("begin to add server event");
    SERVER_EVENT_ADD("observer", "stop");
    FLOG_INFO("add server event success");

    service_started_ = false;
    stopped_ = true;

    FLOG_INFO("begin to stop schema updater");
    schema_updater_.stop();
    FLOG_INFO("schema updater stopped");

    FLOG_INFO("begin to stop ls table updater");
    ls_table_updater_.stop();
    FLOG_INFO("ls table updater stopped");

    FLOG_INFO("begin to stop tablet table updater");
    tablet_table_updater_.stop();
    FLOG_INFO("tablet table updater stopped");

    FLOG_INFO("begin to stop meta table checker");
    meta_table_checker_.stop();
    FLOG_INFO("meta table checker stopped");
  }
  FLOG_INFO("[OBSERVICE_NOTICE] observice finish stop", K_(stopped));
}

void ObService::wait()
{
  FLOG_INFO("[OBSERVICE_NOTICE] wait ob_service begin");
  if (!inited_) {
    LOG_WARN("ob_service not init", K_(inited));
  } else {
    FLOG_INFO("begin to wait schema updater");
    schema_updater_.wait();
    FLOG_INFO("wait schema updater success");

    FLOG_INFO("begin to wait ls table updater");
    ls_table_updater_.wait();
    FLOG_INFO("wait ls table updater success");

    FLOG_INFO("begin to wait tablet table updater");
    tablet_table_updater_.wait();
    FLOG_INFO("wait tablet table updater success");

    FLOG_INFO("begin to wait meta table checker");
    meta_table_checker_.wait();
    FLOG_INFO("wait meta table checker success");
  }
  FLOG_INFO("[OBSERVICE_NOTICE] wait ob_service end");
}

int ObService::destroy()
{
  int ret = OB_SUCCESS;
  FLOG_INFO("[OBSERVICE_NOTICE] destroy ob_service begin");
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ob_service not init", KR(ret), K_(inited));
  } else {
    FLOG_INFO("begin to destroy schema updater");
    schema_updater_.destroy();
    FLOG_INFO("schema updater destroyed");

    FLOG_INFO("begin to destroy lease state manager");
    lease_state_mgr_.destroy();
    FLOG_INFO("lease state manager destroyed");

    FLOG_INFO("begin to destroy server event instance");
    SERVER_EVENT_INSTANCE.destroy();
    FLOG_INFO("server event instance destroyed");

    FLOG_INFO("begin to destroy meta table checker");
    meta_table_checker_.destroy();
    FLOG_INFO("meta table checker destroyed");

  }
  FLOG_INFO("[OBSERVICE_NOTICE] destroy ob_service end", KR(ret));
  return ret;
}


// used by standby cluster
int ObService::update_baseline_schema_version(const int64_t schema_version)
{
  int ret = OB_SUCCESS;
    ObMultiVersionSchemaService *schema_service = gctx_.schema_service_;
  if (schema_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(schema_version));
  } else if (OB_ISNULL(schema_service)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid schema service", KR(ret));
  } else if (OB_FAIL(schema_service->update_baseline_schema_version(
             OB_SYS_TENANT_ID, schema_version))) {
    LOG_WARN("fail to update baseline schema version", KR(ret), K(schema_version));
  } else {
    LOG_INFO("update baseline schema version success", K(schema_version));
  }
  return ret;
}

const ObAddr &ObService::get_self_addr()
{
  return gctx_.self_addr();
}

int ObService::submit_ls_update_task(
    const uint64_t tenant_id,
    const ObLSID &ls_id)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret));
  } else if (!ls_id.is_valid_with_tenant(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(ls_id));
  } else if (OB_FAIL(ls_table_updater_.async_update(tenant_id, ls_id))) {
    LOG_WARN("fail to async update log stream", KR(ret), K(tenant_id), K(ls_id));
  }
  return ret;
}

int ObService::submit_tablet_update_task(
    const uint64_t tenant_id,
    const ObLSID &ls_id,
    const ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret));
  } else if (!ls_id.is_valid_with_tenant(tenant_id) || !tablet_id.is_valid_with_tenant(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(ls_id), K(tablet_id));
  } else if (OB_FAIL(tablet_table_updater_.async_update(tenant_id, ls_id, tablet_id))) {
    LOG_WARN("fail to async update tablet", KR(ret), K(tenant_id), K(ls_id), K(tablet_id));
  }
  return ret;
}


int ObService::submit_async_refresh_schema_task(
    const uint64_t tenant_id,
    const int64_t schema_version)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_TENANT_ID == tenant_id
             || OB_INVALID_ID == tenant_id
             || !ObSchemaService::is_formal_version(schema_version)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", KR(ret), K(tenant_id), K(schema_version));
  } else if (OB_FAIL(schema_updater_.async_refresh_schema(tenant_id, schema_version))) {
    LOG_WARN("fail to async refresh schema", KR(ret), K(tenant_id), K(schema_version));
  }
  return ret;
}


// should return success if all partition have merge to specific frozen_version
int ObService::check_frozen_version(const obrpc::ObCheckFrozenVersionArg &arg)
{
  LOG_INFO("receive check frozen version request", K(arg));
  int ret = OB_SUCCESS;
//  ObPartitionScheduler &scheduler = ObPartitionScheduler::get_instance();
//  int64_t last_merged_version = scheduler.get_merged_version();
  int64_t last_merged_version = 0;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret));
  } else if (OB_UNLIKELY(!arg.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(arg));
  } else if (arg.frozen_version_ != last_merged_version) {
    ret = OB_ERR_CHECK_DROP_COLUMN_FAILED;
    LOG_WARN("last merged version not match", KR(ret), K(arg), K(last_merged_version));
  }
  return ret;
}

int ObService::get_min_sstable_schema_version(
    const obrpc::ObGetMinSSTableSchemaVersionArg &arg,
    obrpc::ObGetMinSSTableSchemaVersionRes &result)
{
  int ret = OB_SUCCESS;
  ObMultiVersionSchemaService *schema_service = gctx_.schema_service_;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret));
  } else if (OB_UNLIKELY(!arg.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(arg));
  } else if (OB_ISNULL(schema_service)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema service is null", KR(ret));
  } else {
    for (int i = 0; OB_SUCC(ret) && i < arg.tenant_id_arg_list_.size(); ++i) {
      // The minimum schema_version used by storage will increase with the major version,
      // storage only need to keep schema history used by a certain number major version.
      // For storage, there is no need to the server level statistics: https://yuque.antfin-inc.com/ob/rootservice/feqqfr
      // min_schema_version = scheduler.get_min_schema_version(arg.tenant_id_arg_list_.at(i));
      int tmp_ret = OB_SUCCESS;
      const uint64_t tenant_id = arg.tenant_id_arg_list_.at(i);
      int64_t min_schema_version = 0;
      int64_t tmp_min_schema_version = 0;
      if (OB_TMP_FAIL(schema_service->get_recycle_schema_version(
                         tenant_id, min_schema_version))) {
        min_schema_version = OB_INVALID_VERSION;
        LOG_WARN("fail to get recycle schema version", KR(tmp_ret), K(tenant_id));
      } else {
        MTL_SWITCH(tenant_id) {
          if (OB_TMP_FAIL(MTL(ObTenantTabletScheduler *)->get_min_dependent_schema_version(tmp_min_schema_version))) {
            min_schema_version = OB_INVALID_VERSION;
            if (OB_ENTRY_NOT_EXIST != tmp_ret) {
              LOG_WARN("failed to get min dependent schema version", K(tmp_ret));
            }
          } else if (tmp_min_schema_version != OB_INVALID_VERSION) {
            min_schema_version = MIN(min_schema_version, tmp_min_schema_version);
          }
        } else {
          if (OB_TENANT_NOT_IN_SERVER != ret) {
            STORAGE_LOG(WARN, "switch tenant failed", K(ret), K(tenant_id));
          } else {
            ret = OB_SUCCESS;
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(result.ret_list_.push_back(min_schema_version))) {
        LOG_WARN("push error", KR(ret), K(arg));
      }
    }
  }
  return ret;
}

int ObService::calc_column_checksum_request(const obrpc::ObCalcColumnChecksumRequestArg &arg, obrpc::ObCalcColumnChecksumRequestRes &res)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObService has not been inited", KR(ret));
  } else if (OB_UNLIKELY(!arg.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", KR(ret), K(arg));
  } else {
    // schedule unique checking task
    const uint64_t tenant_id = arg.tenant_id_;
    int saved_ret = OB_SUCCESS;
    MTL_SWITCH(tenant_id) {
      ObGlobalUniqueIndexCallback *callback = NULL;
      ObTenantDagScheduler* dag_scheduler = nullptr;
      if (OB_ISNULL(dag_scheduler = MTL(ObTenantDagScheduler *))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("error unexpected, dag scheduler must not be nullptr", KR(ret));
      } else if (OB_FAIL(res.ret_codes_.reserve(arg.calc_items_.count()))) {
        LOG_WARN("reserve return code array failed", K(ret), K(arg.calc_items_.count()));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < arg.calc_items_.count(); ++i) {
          const ObCalcColumnChecksumRequestArg::SingleItem &calc_item = arg.calc_items_.at(i);
          ObUniqueCheckingDag *dag = NULL;
          int tmp_ret = OB_SUCCESS;
          saved_ret = OB_SUCCESS;
          if (OB_TMP_FAIL(dag_scheduler->alloc_dag(dag))) {
            STORAGE_LOG(WARN, "fail to alloc dag", KR(tmp_ret));
          } else if (OB_TMP_FAIL(dag->init(arg.tenant_id_,
                                           calc_item.ls_id_,
                                           calc_item.tablet_id_,
                                           calc_item.calc_table_id_ == arg.target_table_id_,
                                           arg.target_table_id_,
                                           arg.schema_version_,
                                           arg.task_id_,
                                           arg.execution_id_,
                                           arg.snapshot_version_))) {
            STORAGE_LOG(WARN, "fail to init ObUniqueCheckingDag", KR(tmp_ret));
          } else if (OB_TMP_FAIL(dag->alloc_global_index_task_callback(calc_item.tablet_id_,
                                                                       arg.target_table_id_,
                                                                       arg.source_table_id_,
                                                                       arg.schema_version_,
                                                                       arg.task_id_,
                                                                       callback))) {
            STORAGE_LOG(WARN, "fail to alloc global index task callback", KR(tmp_ret));
          } else if (OB_TMP_FAIL(dag->alloc_unique_checking_prepare_task(callback))) {
            STORAGE_LOG(WARN, "fail to alloc unique checking prepare task", KR(tmp_ret));
          } else if (OB_TMP_FAIL(dag_scheduler->add_dag(dag))) {
            saved_ret = tmp_ret;
            if (OB_EAGAIN == tmp_ret) {
              tmp_ret = OB_SUCCESS;
            } else if (OB_SIZE_OVERFLOW == tmp_ret) {
              tmp_ret = OB_EAGAIN;
            } else {
              STORAGE_LOG(WARN, "fail to add dag to queue", KR(tmp_ret));
            }
          }
          if (OB_SUCCESS != saved_ret && NULL != dag) {
            dag_scheduler->free_dag(*dag);
            dag = NULL;
          }
          if (OB_SUCC(ret)) {
            if (OB_FAIL(res.ret_codes_.push_back(tmp_ret))) {
              LOG_WARN("push back return code failed", K(ret), K(tmp_ret));
            }
          }
        }
      }
    }
    LOG_INFO("receive column checksum request", K(arg));
  }
  return ret;
}

int ObService::fetch_sys_ls(share::ObLSReplica &replica)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret));
  } else if (OB_FAIL(fill_ls_replica(OB_SYS_TENANT_ID, SYS_LS, replica))) {
    LOG_WARN("fetch_sys_ls failed", KR(ret), K(replica));
  } else {
    LOG_INFO("fetch sys_ls succeed", K(replica));
  }
  return ret;
}

int ObService::backup_ls_data(const obrpc::ObBackupDataArg &arg)
{
  int ret = OB_SUCCESS;
  FLOG_INFO("[BACKUP] receive backup ls data rpc", K(arg));
  ObBackupJobDesc job_desc;
  job_desc.job_id_ = arg.job_id_;
  job_desc.task_id_ = arg.task_id_;
  job_desc.trace_id_ = arg.trace_id_;
  share::ObBackupDest backup_dest;
  uint64_t tenant_id = arg.tenant_id_;
  ObBackupSetDesc backup_set_desc;
  backup_set_desc.backup_set_id_ = arg.backup_set_id_;
  backup_set_desc.backup_type_.type_ = arg.backup_type_;
  const ObLSID &ls_id = arg.ls_id_;
  const int64_t turn_id = arg.turn_id_;
  const int64_t retry_id = arg.retry_id_;
  const ObBackupDataType &backup_data_type = arg.backup_data_type_;
  ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  if (!arg.is_valid() || OB_ISNULL(sql_proxy)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid args", KR(ret), K(arg));
  } else if (OB_FAIL(ObBackupStorageInfoOperator::get_backup_dest(*sql_proxy, tenant_id, arg.backup_path_, backup_dest))) {
    LOG_WARN("failed to get backup dest", KR(ret), K(arg));
  } else if (OB_FAIL(ObBackupHandler::schedule_backup_data_dag(job_desc,
      backup_dest, tenant_id, backup_set_desc, ls_id, turn_id, retry_id, backup_data_type))) {
    LOG_WARN("failed to schedule backup data dag", K(ret), K(arg));
  } else {
    const char *backup_event_str = NULL;
    if (backup_data_type.is_sys_backup()) {
      backup_event_str = "schedule_backup_ls_sys_data";
    } else if (backup_data_type.is_minor_backup()) {
      backup_event_str = "schedule_backup_ls_minor_data";
    } else if (backup_data_type.is_major_backup()) {
      backup_event_str = "schedule_backup_ls_major_data";
    } else {
      backup_event_str = "unknown";
    }
    SERVER_EVENT_ADD("backup_data", backup_event_str,
      "tenant_id", arg.tenant_id_,
      "backup_set_id", arg.backup_set_id_,
      "ls_id", arg.ls_id_.id(),
      "turn_id", arg.turn_id_,
      "retry_id", arg.retry_id_,
      "trace_id", arg.trace_id_);
    LOG_INFO("success recevied backup ls data rpc", K(arg));
  }
  return ret;
}

int ObService::backup_completing_log(const obrpc::ObBackupComplLogArg &arg)
{
  int ret = OB_SUCCESS;
  FLOG_INFO("[BACKUP] receive backup completing log rpc", K(arg));
  ObBackupJobDesc job_desc;
  job_desc.job_id_ = arg.job_id_;
  job_desc.task_id_ = arg.task_id_;
  job_desc.trace_id_ = arg.trace_id_;
  share::ObBackupDest backup_dest;
  uint64_t tenant_id = arg.tenant_id_;
  ObBackupSetDesc backup_set_desc;
  backup_set_desc.backup_set_id_ = arg.backup_set_id_;
  backup_set_desc.backup_type_.type_ = arg.backup_type_;
  share::ObBackupSCN start_scn = arg.start_scn_;
  share::ObBackupSCN end_scn = arg.end_scn_;
  ObLSID ls_id = arg.ls_id_;
  ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  if (!arg.is_valid() || OB_ISNULL(sql_proxy)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid args", KR(ret), K(arg));
  } else if (OB_FAIL(ObBackupStorageInfoOperator::get_backup_dest(*sql_proxy, tenant_id, arg.backup_path_, backup_dest))) {
    LOG_WARN("failed to get backup dest", KR(ret), K(arg));
  } else if (OB_FAIL(ObBackupHandler::schedule_backup_complement_log_dag(
      job_desc, backup_dest, tenant_id, backup_set_desc, ls_id, start_scn, end_scn))) {
    LOG_WARN("failed to schedule backup data dag", KR(ret), K(arg));
  } else {
    SERVER_EVENT_ADD("backup_data", "schedule_backup_complement_log",
      "tenant_id", arg.tenant_id_,
      "backup_set_id", arg.backup_set_id_,
      "ls_id", arg.ls_id_.id(),
      "start_scn", arg.start_scn_,
      "end_scn", arg.end_scn_,
      "trace_id", arg.trace_id_);
    LOG_INFO("success recevied backup compl log rpc", K(arg));
  }
  return ret;
}

int ObService::backup_build_index(const obrpc::ObBackupBuildIdxArg &arg)
{
  int ret = OB_SUCCESS;
  FLOG_INFO("[BACKUP] receive backup build index rpc", K(arg));
  ObBackupJobDesc job_desc;
  job_desc.job_id_ = arg.job_id_;
  job_desc.task_id_ = arg.task_id_;
  job_desc.trace_id_ = arg.trace_id_;
  share::ObBackupDest backup_dest;
  uint64_t tenant_id = arg.tenant_id_;
  ObBackupSetDesc backup_set_desc;
  backup_set_desc.backup_set_id_ = arg.backup_set_id_;
  backup_set_desc.backup_type_.type_ = arg.backup_type_;
  const int64_t turn_id = arg.turn_id_;
  const int64_t retry_id = arg.retry_id_;
  const share::ObBackupDataType backup_data_type = arg.backup_data_type_;
  ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  if (!arg.is_valid() || OB_ISNULL(sql_proxy)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid args", KR(ret), K(arg));
  } else if (OB_FAIL(ObBackupStorageInfoOperator::get_backup_dest(*sql_proxy, tenant_id, arg.backup_path_, backup_dest))) {
    LOG_WARN("failed to get backup dest", K(ret), K(arg));
  } else if (OB_FAIL(ObBackupHandler::schedule_build_tenant_level_index_dag(job_desc,
      backup_dest, tenant_id, backup_set_desc, turn_id, retry_id, backup_data_type))) {
    LOG_WARN("failed to schedule backup data dag", K(ret), K(arg));
  } else {
    SERVER_EVENT_ADD("backup_data", "schedule_build_tenant_level_index",
      "tenant_id", arg.tenant_id_,
      "backup_set_id", arg.backup_set_id_,
      "turn_id", arg.turn_id_,
      "backup_data_type", backup_data_type.type_,
      "job_id", arg.job_id_,
      "trace_id", arg.trace_id_);
  }
  LOG_INFO("success recevied backup build index rpc", K(ret), K(arg));
  return ret;
}

int ObService::backup_meta(const obrpc::ObBackupMetaArg &arg)
{
  int ret = OB_SUCCESS;
  FLOG_INFO("[BACKUP] receive backup meta rpc", K(arg));
  ObBackupJobDesc job_desc;
  job_desc.job_id_ = arg.job_id_;
  job_desc.task_id_ = arg.task_id_;
  job_desc.trace_id_ = arg.trace_id_;
  share::ObBackupDest backup_dest;
  uint64_t tenant_id = arg.tenant_id_;
  ObBackupSetDesc backup_set_desc;
  backup_set_desc.backup_set_id_ = arg.backup_set_id_;
  backup_set_desc.backup_type_.type_ = arg.backup_type_;
  const ObLSID &ls_id = arg.ls_id_;
  const int64_t turn_id = arg.turn_id_;
  const int64_t retry_id = arg.retry_id_;
  const share::ObBackupSCN start_scn = arg.start_scn_;
  ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  if (!arg.is_valid() || OB_ISNULL(sql_proxy)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid args", KR(ret), K(arg));
  } else if (OB_FAIL(ObBackupStorageInfoOperator::get_backup_dest(*sql_proxy, tenant_id, arg.backup_path_, backup_dest))) {
    LOG_WARN("failed to get backup dest", K(ret), K(arg));
  } else if (OB_FAIL(ObBackupHandler::schedule_backup_meta_dag(job_desc,
      backup_dest, tenant_id, backup_set_desc, ls_id, turn_id, retry_id, start_scn))) {
    LOG_WARN("failed to schedule backup data dag", KR(ret), K(arg));
  } else {
    SERVER_EVENT_ADD("backup_data", "schedule_backup_ls_meta",
      "tenant_id", arg.tenant_id_,
      "backup_set_id", arg.backup_set_id_,
      "ls_id", arg.ls_id_.id(),
      "turn_id", arg.turn_id_,
      "retry_id", arg.retry_id_,
      "trace_id", arg.trace_id_);
    LOG_INFO("success recevied backup ls meta rpc", K(arg));
  }
  return ret;
}

// At backup meta stage, observer will backup tablet id list
// And RS will compare the observer's tablet id list with the newest tablet id list
// and will get the diff tablet id list, for each tablet not in observer's
// backed up list, it will compare using observer's backup scn and tx data's tx log ts
int ObService::check_not_backup_tablet_create_scn(const obrpc::ObBackupCheckTabletArg &arg)
{
  int ret = OB_SUCCESS;
  LOG_INFO("success received backup check tablet from rs", K(arg));
  if (!arg.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(arg));
  } else {
    MTL_SWITCH(arg.tenant_id_) {
      ObLSService *ls_srv = nullptr;
      ObLSHandle ls_handle;
      ObLS *ls = nullptr;
      ObLSTabletService *ls_tablet_svr = nullptr;
      const ObSArray<ObTabletID> &tablet_ids = arg.tablet_ids_;
      if (OB_ISNULL(ls_srv = MTL(ObLSService*))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ls service is nullptr", KR(ret));
      } else if (OB_FAIL(ls_srv->get_ls(arg.ls_id_, ls_handle, ObLSGetMod::OBSERVER_MOD))) {
        LOG_WARN("fail to get log stream", KR(ret), K(arg.tenant_id_), K(arg.ls_id_));
      } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("log stream should not be NULL", KR(ret), K(arg.tenant_id_), K(arg.ls_id_), KPC(ls));
      } else if (OB_ISNULL(ls_tablet_svr = ls->get_tablet_svr())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ls tablet service should not be NULL", KR(ret), K(arg.tenant_id_), K(arg.ls_id_), KPC(ls));
      } else {
        const int64_t timeout_us = ObTabletCommon::DIRECT_GET_COMMITTED_TABLET_TIMEOUT_US;
        ObTabletHandle tablet_handle;
        for (int64_t i = 0; OB_SUCC(ret) && i < tablet_ids.count(); ++i) {
          const ObTabletID &tablet_id = tablet_ids.at(i);
          tablet_handle.reset();
          if (OB_FAIL(ls_tablet_svr->get_tablet(tablet_id, tablet_handle, timeout_us))) {
            if (OB_TABLET_NOT_EXIST == ret) {
              LOG_INFO("tablet has been deleted, no need to check", K(tablet_id));
              ret = OB_SUCCESS;
            } else {
              LOG_WARN("failed to get tablet", KR(ret), K(tablet_id), K(timeout_us));
            }
          } else if (OB_UNLIKELY(!tablet_handle.is_valid())) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unexpected error : tablet handle is invalid", KR(ret), K(tablet_handle));
          } else {
            const ObTabletMeta &tablet_meta = tablet_handle.get_obj()->get_tablet_meta();
            if (OB_UNLIKELY(tablet_meta.create_scn_ <= arg.backup_scn_)) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("unexpected error : tablet has't been backup", KR(ret),
                  K(arg.tenant_id_), K(arg.ls_id_), K(tablet_id),
                  K(tablet_meta), "backup_scn", arg.backup_scn_);
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObService::check_backup_task_exist(const ObBackupCheckTaskArg &arg, bool &res)
{
  int ret = OB_SUCCESS;
  res = false;
  if (!arg.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(arg));
  } else {
    MTL_SWITCH(arg.tenant_id_) {
      ObTenantDagScheduler* dag_scheduler = nullptr;
      if (OB_ISNULL(dag_scheduler = MTL(ObTenantDagScheduler *))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("error unexpected, dag scheduler must not be nullptr", K(ret));
      } else if (OB_FAIL(dag_scheduler->check_dag_net_exist(arg.trace_id_, res))) {
        LOG_WARN("failed to check dag net exist", K(ret), K(arg));
      }
    }
  }
  return ret;
}

int ObService::delete_backup_ls_task(const obrpc::ObLSBackupCleanArg &arg)
{
  int ret = OB_SUCCESS;
  LOG_INFO("receive delete backup ls task request", K(arg));
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObService not init", K(ret));
  } else if (!arg.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(arg));
  } else if (OB_FAIL(ObLSBackupCleanScheduler::schedule_backup_clean_dag(arg))) {
    LOG_WARN("failed to schedule backup clean dag", K(ret), K(arg));
  } else {
    LOG_INFO("success receive delete backup ls task rpc", K(arg));
  }

  return ret;
}


int ObService::check_sys_task_exist(
    const share::ObTaskId &arg, bool &res)
{
  int ret = OB_SUCCESS;

  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObService not init", K(ret));
  } else if (arg.is_invalid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(arg), K(ret));
  } else if (OB_FAIL(SYS_TASK_STATUS_MGR.task_exist(arg, res))) {
    LOG_WARN("failed to check task exist", K(ret), K(arg));
  }
  return ret;
}

int ObService::check_migrate_task_exist(
    const share::ObTaskId &arg, bool &res)
{
  int ret = OB_NOT_SUPPORTED;

  LOG_ERROR("not supported", K(ret), K(arg), K(res));
  return ret;
}

int ObService::minor_freeze(const obrpc::ObMinorFreezeArg &arg,
                            obrpc::Int64 &result)
{
  int ret = OB_SUCCESS;
  const int64_t start_ts = ObTimeUtility::current_time();
  LOG_INFO("receive minor freeze request", K(arg));

  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!arg.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(arg), K(ret));
  } else {
    if (arg.tablet_id_.is_valid()) {
      // minor feeeze tablet
      if (1 == arg.tenant_ids_.count()) {
        uint64_t tenant_id = arg.tenant_ids_.at(0);
        if (OB_UNLIKELY(OB_FAIL(tablet_freeze(tenant_id, arg.tablet_id_)))) {
          LOG_WARN("fail to freeze tablet", K(ret), K(tenant_id), K(arg.tablet_id_));
        }
      } else {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("only one tenant is needed", K(ret), K(arg.tenant_ids_), K(arg.tablet_id_));
      }
    } else if (arg.tenant_ids_.count() > 0) {
      // minor freeze tenants
      for (int i = 0; i < arg.tenant_ids_.count(); ++i) {
        int tmp_ret = OB_SUCCESS;
        uint64_t tenant_id = arg.tenant_ids_.at(i);
        if (OB_UNLIKELY(OB_SUCCESS != (tmp_ret = tenant_freeze(tenant_id)))) {
          LOG_WARN("fail to freeze tenant", K(tmp_ret), K(tenant_id));
        }
        // record the first error code
        if (OB_SUCCESS != tmp_ret && OB_SUCC(ret)) {
          ret = tmp_ret;
        }
      }
    } else {
      // for minor freeze server
      // freeze all tenants
      if (OB_ISNULL(GCTX.omt_)) {
        ret = OB_ERR_UNEXPECTED;
        SERVER_LOG(WARN, "failed to get multi tenant from GCTX", K(ret));
      } else {
        omt::TenantIdList all_tenants;
        GCTX.omt_->get_tenant_ids(all_tenants);
        for (int i = 0; i < all_tenants.size(); ++i) {
          int tmp_ret = OB_SUCCESS;
          uint64_t tenant_id = all_tenants[i];
          if (OB_UNLIKELY(OB_SUCCESS != (tmp_ret = tenant_freeze(tenant_id)))) {
            if (OB_TENANT_NOT_IN_SERVER == tmp_ret) {
              LOG_INFO("skip freeze stopped tenant", K(tmp_ret), K(tenant_id));
              tmp_ret = OB_SUCCESS;
            } else {
              LOG_WARN("fail to freeze tenant", K(tmp_ret), K(tenant_id));
            }
          }
          // record the first error code
          if (OB_SUCCESS != tmp_ret && OB_SUCC(ret)) {
            ret = tmp_ret;
          }
        }
      }
    }
  }

  result = ret;
  const int64_t cost_ts = ObTimeUtility::current_time() - start_ts;
  LOG_INFO("finish minor freeze request", K(ret), K(arg), K(cost_ts));
  return ret;
}

int ObService::tablet_freeze(const uint64_t tenant_id, const common::ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;

  if (is_virtual_tenant_id(tenant_id)) {
    LOG_INFO("no need to freeze virtual tenant", K(ret), K(tenant_id), K(tablet_id));
  } else {
    MTL_SWITCH(tenant_id) {
      storage::ObTenantFreezer* freezer = nullptr;
      if (OB_ISNULL(freezer = MTL(storage::ObTenantFreezer*))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ObTenantFreezer shouldn't be null", K(ret), K(tenant_id));
      } else if (OB_FAIL(freezer->tablet_freeze(tablet_id))) {
        LOG_WARN("fail to freeze tablet", K(ret), K(tenant_id), K(tablet_id));
      } else {
        LOG_INFO("succeed to freeze tablet", K(ret), K(tenant_id), K(tablet_id));
      }
    }
  }

  return ret;
}

int ObService::tenant_freeze(const uint64_t tenant_id)
{
  int ret = OB_SUCCESS;

  if (is_virtual_tenant_id(tenant_id)) {
    LOG_INFO("no need to freeze virtual tenant", K(ret), K(tenant_id));
  } else {
    MTL_SWITCH(tenant_id) {
      checkpoint::ObCheckPointService* checkpoint_serv = nullptr;
      if (OB_ISNULL(checkpoint_serv = MTL(checkpoint::ObCheckPointService*))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ObCheckPointService shouldn't be null", K(ret), K(tenant_id));
      } else if (OB_FAIL(checkpoint_serv->do_minor_freeze())) {
        if (OB_ENTRY_EXIST == ret) {
          ret = OB_SUCCESS;
        } else {
          LOG_WARN("fail to freeze tenant", K(tenant_id), K(ret));
        }
      } else {
        LOG_INFO("succeed to freeze tenant", K(tenant_id), K(ret));
      }
    } else {
      LOG_WARN("fail to switch tenant", K(ret), K(tenant_id));
    }
  }

  return ret;
}

int ObService::check_modify_time_elapsed(
    const obrpc::ObCheckModifyTimeElapsedArg &arg,
    obrpc::ObCheckModifyTimeElapsedResult &result)
{
  int ret = OB_SUCCESS;
  LOG_INFO("receive get checksum cal snapshot", K(arg));
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!arg.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(arg));
  } else {
    MTL_SWITCH(arg.tenant_id_) {
      ObLSHandle ls_handle;
      transaction::ObTransService *txs = MTL(transaction::ObTransService *);
      ObLSService *ls_service = MTL(ObLSService *);
      if (OB_FAIL(result.results_.reserve(arg.tablets_.count()))) {
        LOG_WARN("reserve result array failed", K(ret), K(arg.tablets_.count()));
      }

      for (int64_t i = 0; OB_SUCC(ret) && i < arg.tablets_.count(); ++i) {
        ObTabletHandle tablet_handle;
        ObLSHandle ls_handle;
        const ObLSID &ls_id = arg.tablets_.at(i).ls_id_;
        const ObTabletID &tablet_id = arg.tablets_.at(i).tablet_id_;
        ObCheckTransElapsedResult single_result;
        int tmp_ret = OB_SUCCESS;
        if (OB_TMP_FAIL(ls_service->get_ls(ls_id, ls_handle, ObLSGetMod::OBSERVER_MOD))) {
          LOG_WARN("get ls failed", K(tmp_ret), K(ls_id));
        } else if (OB_TMP_FAIL(ls_handle.get_ls()->check_modify_time_elapsed(tablet_id,
                                                                             arg.sstable_exist_ts_,
                                                                             single_result.pending_tx_id_))) {
          if (OB_EAGAIN != tmp_ret) {
            LOG_WARN("check schema version elapsed failed", K(tmp_ret), K(arg));
          }
        } else if (OB_TMP_FAIL(txs->get_max_commit_version(single_result.snapshot_))) {
          LOG_WARN("fail to get max commit version", K(tmp_ret));
        }
        if (OB_SUCC(ret)) {
          single_result.ret_code_ = tmp_ret;
          if (OB_FAIL(result.results_.push_back(single_result))) {
            LOG_WARN("push back single result failed", K(ret), K(i), K(single_result));
          }
        }
      }
    }
  }
  return ret;
}

int ObService::check_schema_version_elapsed(
    const obrpc::ObCheckSchemaVersionElapsedArg &arg,
    obrpc::ObCheckSchemaVersionElapsedResult &result)
{
  int ret = OB_SUCCESS;
  LOG_INFO("receive check schema version elapsed", K(arg));
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!arg.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(arg));
  } else {
    MTL_SWITCH(arg.tenant_id_) {
      ObLSService *ls_service = nullptr;
      if (OB_ISNULL(ls_service = MTL(ObLSService *))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("error unexpected, get ls service failed", K(ret));
      } else if (OB_FAIL(result.results_.reserve(arg.tablets_.count()))) {
        LOG_WARN("reserve result array failed", K(ret), K(arg.tablets_.count()));
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < arg.tablets_.count(); ++i) {
        ObTabletHandle tablet_handle;
        ObLSHandle ls_handle;
        const ObLSID &ls_id = arg.tablets_.at(i).ls_id_;
        const ObTabletID &tablet_id = arg.tablets_.at(i).tablet_id_;
        ObCheckTransElapsedResult single_result;
        int tmp_ret = OB_SUCCESS;
        if (OB_TMP_FAIL(ls_service->get_ls(ls_id, ls_handle, ObLSGetMod::OBSERVER_MOD))) {
          LOG_WARN("get ls failed", K(tmp_ret), K(i), K(ls_id));
        } else if (OB_TMP_FAIL(ls_handle.get_ls()->get_tablet(tablet_id, tablet_handle))) {
          LOG_WARN("fail to get tablet", K(tmp_ret), K(i), K(ls_id), K(tablet_id));
        } else if (OB_TMP_FAIL(tablet_handle.get_obj()->check_schema_version_elapsed(arg.schema_version_,
                                                                                     arg.need_wait_trans_end_,
                                                                                     single_result.snapshot_,
                                                                                     single_result.pending_tx_id_))) {
          LOG_WARN("check schema version elapsed failed", K(tmp_ret), K(arg), K(ls_id), K(tablet_id));
        }
        if (OB_SUCC(ret)) {
          single_result.ret_code_ = tmp_ret;
          if (OB_FAIL(result.results_.push_back(single_result))) {
            LOG_WARN("push back single result failed", K(ret), K(i), K(single_result));
          }
        }
      }
    }
  }
  return ret;
}


int ObService::batch_switch_rs_leader(const ObAddr &arg)
{
  UNUSEDx(arg);
  int ret = OB_NOT_SUPPORTED;
  // LOG_INFO("receive batch switch rs leader request", K(arg));

  // int64_t start_timestamp = ObTimeUtility::current_time();
  // if (OB_UNLIKELY(!inited_)) {
  //   ret = OB_NOT_INIT;
  //   LOG_WARN("not init", KR(ret));
  // } else if (OB_ISNULL(gctx_.par_ser_)) {
  //   ret = OB_ERR_UNEXPECTED;
  //   LOG_WARN("gctx par_ser is NULL", K(arg));
  // } else if (!arg.is_valid()) {
  //   if (OB_FAIL(gctx_.par_ser_->auto_batch_change_rs_leader())) {
  //     LOG_WARN("fail to auto batch change rs leader", KR(ret));
  //   }
  // } else if (OB_FAIL(gctx_.par_ser_->batch_change_rs_leader(arg))) {
  //   LOG_WARN("fail to batch change rs leader", K(arg), KR(ret));
  // }

  // int64_t cost = ObTimeUtility::current_time() - start_timestamp;
  // SERVER_EVENT_ADD("election", "batch_switch_rs_leader", K(ret),
  //                  "leader", arg,
  //                  K(cost));
  return ret;
}

int ObService::switch_schema(
    const obrpc::ObSwitchSchemaArg &arg,
    obrpc::ObSwitchSchemaResult &result)
{
  int ret = OB_SUCCESS;
  LOG_INFO("start to switch schema", K(arg));
  const ObRefreshSchemaInfo &schema_info = arg.schema_info_;
  const int64_t schema_version = schema_info.get_schema_version();
  const uint64_t tenant_id = schema_info.get_tenant_id();
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret));
  } else if (OB_UNLIKELY(schema_version <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument",  KR(ret), K(schema_version));
  //FIXME:(yanmu.ztl)
  // As a temporary solution to reduce schema error after execute ddl,
  // try the best to refresh schema in cluster synchronously.
  //} else if (!arg.force_refresh_) {
  //  if (OB_FAIL(schema_updater_.try_reload_schema(schema_info))) {
  //    LOG_WARN("reload schema failed", K(schema_info), K(ret));
  //  }
  } else {
    ObSEArray<uint64_t, 1> tenant_ids;
    ObMultiVersionSchemaService *schema_service = gctx_.schema_service_;
    int64_t local_schema_version = OB_INVALID_VERSION;
    int64_t abs_timeout = OB_INVALID_TIMESTAMP;
    if (OB_UNLIKELY(!schema_info.is_valid() || !is_valid_tenant_id(tenant_id))) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid schema info", KR(ret), K(schema_info), K(tenant_id));
    } else if (OB_FAIL(tenant_ids.push_back(tenant_id))) {
      LOG_WARN("fail to push back tenant_id", KR(ret), K(tenant_id));
    } else if (OB_ISNULL(schema_service)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("schema service is null", KR(ret));
    } else if (OB_FAIL(ObShareUtil::get_abs_timeout(GCONF.rpc_timeout, abs_timeout))) {
      LOG_WARN("fail to get abs timeout", KR(ret), "default_timeout", static_cast<int64_t>(GCONF.rpc_timeout));
    } else {
      bool need_retry = arg.force_refresh_; // sync refresh schema should retry until timeout
      do {
        int tmp_ret = OB_SUCCESS;
        if (ObTimeUtility::current_time() >= abs_timeout) {
          ret = OB_TIMEOUT;
          LOG_WARN("already timeout", KR(ret), K(abs_timeout));
        } else if (OB_TMP_FAIL(schema_service->refresh_and_add_schema(tenant_ids))) {
          LOG_WARN("fail to refresh schema", KR(tmp_ret), K(schema_info), K(tenant_ids));
          if (need_retry) {
            ob_usleep(100 * 1000L); // 100ms
          } else {
            ret = tmp_ret;
          }
        } else {
          break;
        }
      } while (OB_SUCC(ret));

      if (OB_FAIL(ret)) {
      } else if (schema_info.get_schema_version() <= 0) {
        // skip
      } else if (OB_FAIL(schema_service->get_tenant_refreshed_schema_version(
                         tenant_id, local_schema_version))) {
        LOG_WARN("fail to get local tenant schema_version", KR(ret), K(tenant_id));
      } else if (OB_UNLIKELY(schema_info.get_schema_version() > local_schema_version)) {
        ret = OB_EAGAIN;
        LOG_WARN("schema is not new enough", KR(ret), K(schema_info), K(local_schema_version));
      }
    }
  }
  LOG_INFO("switch schema", K(ret), K(schema_info));
  //SERVER_EVENT_ADD("schema", "switch_schema", K(ret), K(schema_info));
  result.set_ret(ret);
  return ret;
}

int ObService::bootstrap(const obrpc::ObBootstrapArg &arg)
{
  int ret = OB_SUCCESS;
  const int64_t timeout = 600 * 1000 * 1000LL; // 10 minutes
  const obrpc::ObServerInfoList &rs_list = arg.server_list_;
  LOG_INFO("bootstrap timeout", K(timeout), "worker_timeout_ts", THIS_WORKER.get_timeout_ts());
  if (!inited_) {
    ret = OB_NOT_INIT;
    BOOTSTRAP_LOG(WARN, "not init", K(ret));
  } else if (rs_list.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    BOOTSTRAP_LOG(WARN, "rs_list is empty", K(rs_list), K(ret));
  } else {
    ObPreBootstrap pre_bootstrap(*gctx_.srv_rpc_proxy_,
                                 rs_list,
                                 *gctx_.lst_operator_,
                                 *gctx_.config_,
                                 arg,
                                 *gctx_.rs_rpc_proxy_);
    ObAddr master_rs;
    bool server_empty = false;
    ObCheckServerEmptyArg new_arg;
    new_arg.mode_ = ObCheckServerEmptyArg::BOOTSTRAP;
    // when OFS mode, this server dir hasn't been created, skip log scan
    const bool wait_log_scan = true;
    if (OB_FAIL(check_server_empty(new_arg, wait_log_scan, server_empty))) {
      BOOTSTRAP_LOG(WARN, "check_server_empty failed", K(ret), K(new_arg));
    } else if (!server_empty) {
      ret = OB_ERR_SYS;
      BOOTSTRAP_LOG(WARN, "observer is not empty", K(ret));
    } else if (OB_FAIL(pre_bootstrap.prepare_bootstrap(master_rs))) {
      BOOTSTRAP_LOG(ERROR, "failed to prepare boot strap", K(rs_list), K(ret));
    } else {
      const ObCommonRpcProxy &rpc_proxy = *gctx_.rs_rpc_proxy_;
      bool boot_done = false;
      const int64_t MAX_RETRY_COUNT = 30;
      for (int i = 0; !boot_done && i < MAX_RETRY_COUNT; i++) {
        ret = OB_SUCCESS;
        int64_t rpc_timeout = timeout;
        if (INT64_MAX != THIS_WORKER.get_timeout_ts()) {
          rpc_timeout = max(rpc_timeout, THIS_WORKER.get_timeout_remain());
        }
        if (OB_FAIL(rpc_proxy.to_addr(master_rs).timeout(rpc_timeout)
                    .execute_bootstrap(arg))) {
          if (OB_RS_NOT_MASTER == ret) {
            BOOTSTRAP_LOG(INFO, "master root service not ready",
                          K(master_rs), "retry_count", i, K(rpc_timeout), K(ret));
            USLEEP(200 * 1000);
          } else {
            const ObAddr rpc_svr = rpc_proxy.get_server();
            BOOTSTRAP_LOG(ERROR, "execute bootstrap fail", KR(ret), K(rpc_svr), K(master_rs), K(rpc_timeout));
            break;
          }
        } else {
          boot_done = true;
        }
      }
      if (boot_done) {
        BOOTSTRAP_LOG(INFO, "succeed to do_boot_strap", K(rs_list), K(master_rs));
      }
    }
  }

  return ret;
}

int ObService::check_deployment_mode_match(
    const obrpc::ObCheckDeploymentModeArg &arg,
    obrpc::Bool &match)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    bool single_zone_deployment_on = OB_FILE_SYSTEM_ROUTER.is_single_zone_deployment_on();
    if (single_zone_deployment_on == arg.single_zone_deployment_on_) {
      match = true;
    } else {
      match = false;
      LOG_INFO("deployment mode not match", K(single_zone_deployment_on), K(arg));
    }
  }
  return ret;
}

int ObService::is_empty_server(const obrpc::ObCheckServerEmptyArg &arg, obrpc::Bool &is_empty)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    bool server_empty = false;
    // server dir must be created when 1) local mode, 2) OFS bootstrap this server
    const bool wait_log_scan = ObCheckServerEmptyArg::BOOTSTRAP == arg.mode_;
    if (OB_FAIL(check_server_empty(arg, wait_log_scan, server_empty))) {
      LOG_WARN("check_server_empty failed", K(ret), K(arg));
    } else {
      is_empty = server_empty;
    }
  }
  return ret;
}


int ObService::get_partition_count(obrpc::ObGetPartitionCountResult &result)
{
  UNUSEDx(result);
  int ret = OB_NOT_SUPPORTED;
  // result.reset();

  // if (!inited_) {
  //   ret = OB_NOT_INIT;
  //   LOG_WARN("not inited", K(ret));
  // } else if (OB_FAIL(gctx_.par_ser_->get_partition_count(result.partition_count_))) {
  //   LOG_WARN("failed to get partition count", K(ret));
  // }
  return ret;
}


int ObService::check_server_empty(const ObCheckServerEmptyArg &arg, const bool wait_log_scan, bool &is_empty)
{
  int ret = OB_SUCCESS;
  is_empty = true;
  UNUSED(wait_log_scan);
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    if (ObCheckServerEmptyArg::BOOTSTRAP == arg.mode_) {
      // "is_valid_heartbeat" is valid:
      // 1. RS is between "start_service" and "full_service", the server has not added to RS;
      // 2. RS is in "full_service" and the server has added to RS;
      // 3. To avoid misjudgment in scenario 1 while add server, this check is skipped here
      if (lease_state_mgr_.is_valid_heartbeat()) {
        LOG_WARN("server already in rootservice lease");
        is_empty = false;
      }
    }

    // wait log scan finish
    //
    // For 4.0, it is not necessary to wait log scan finished.
    //
    // if (is_empty && wait_log_scan) {
    //   const int64_t WAIT_LOG_SCAN_TIME_US = 2 * 1000 * 1000; // only wait 2s for empty server
    //   const int64_t SLEEP_INTERVAL_US = 500;
    //   const int64_t start_time_us = ObTimeUtility::current_time();
    //   int64_t end_time_us = start_time_us;
    //   int64_t timeout_ts = THIS_WORKER.get_timeout_ts();
    //   if (INT64_MAX == THIS_WORKER.get_timeout_ts()) {
    //     timeout_ts = start_time_us + WAIT_LOG_SCAN_TIME_US;
    //   }
    //   while (!stopped_ && !gctx_.par_ser_->is_scan_disk_finished()) {
    //     end_time_us = ObTimeUtility::current_time();
    //     if (end_time_us > timeout_ts) {
    //       LOG_WARN("wait log scan finish timeout", K(timeout_ts), LITERAL_K(WAIT_LOG_SCAN_TIME_US));
    //       is_empty = false;
    //       break;
    //     }
    //     ob_usleep(static_cast<int32_t>(std::min(timeout_ts - end_time_us, SLEEP_INTERVAL_US)));
    //   }
    // }
    if (is_empty) {
      // if (!gctx_.par_ser_->is_empty()) {
      //   LOG_WARN("partition service is not empty");
      //   is_empty = false;
      // }
    }
    if (is_empty) {
      if (!OBSERVER.is_log_dir_empty()) {
        LOG_WARN("log dir is not empty");
        is_empty = false;
      }
    }
  }
  return ret;
}

int ObService::report_replica()
{
  // TODO: yanyuan.cxf this maybe need at 4.0
  // return OB_SUCCESS just for bootstrap.
  int ret = OB_SUCCESS;
  // LOG_INFO("receive report all replicas request");
  // if (!inited_) {
  //   ret = OB_NOT_INIT;
  //   LOG_WARN("not init", K(ret));
  // } else {
  //   ObIPartitionGroupIterator *partition_iter = NULL;
  //   ObIPartitionGroup *partition = NULL;
  //   int64_t replica_count = 0;

  //   if (NULL == (partition_iter = gctx_.par_ser_->alloc_pg_iter())) {
  //     ret = OB_ALLOCATE_MEMORY_FAILED;
  //     LOG_WARN("Fail to alloc partition iter, ", K(ret));
  //   } else {
  //     while (OB_SUCC(ret)) {
  //       ObPartitionArray pkeys;
  //       if (OB_FAIL(partition_iter->get_next(partition))) {
  //         if (OB_ITER_END != ret) {
  //           LOG_WARN("Fail to get next partition, ", K(ret));
  //         }
  //       } else if (NULL == partition) {
  //         ret = OB_ERR_UNEXPECTED;
  //         LOG_WARN("The partition is NULL, ", K(ret));
  //       } else if (OB_FAIL(partition->get_all_pg_partition_keys(pkeys))) {
  //         LOG_WARN("get all pg partition keys error", "pg_key", partition->get_partition_key());
  //         if (OB_ENTRY_NOT_EXIST != ret && OB_PARTITION_NOT_EXIST != ret) {
  //           LOG_WARN("get partition failed", K(ret));
  //         } else {
  //           // The partition has been deleted. There is no need to trigger the report
  //           ret = OB_SUCCESS;
  //         }
  //       } else if (OB_FAIL(submit_pt_update_task(
  //               partition->get_partition_key(),
  //               true/*need report checksum*/))) {
  //         if (OB_PARTITION_NOT_EXIST == ret) {
  //           // The GC thread is already working,
  //           // and deleted during traversal, the replica has been deleted needs to be avoided blocking the start process
  //           ret = OB_SUCCESS;
  //           LOG_INFO("this partition is already not exist",
  //               K(ret), "partition_key", partition->get_partition_key());
  //         } else {
  //           LOG_WARN("submit partition table update task failed",
  //               K(ret), "partition_key", partition->get_partition_key());
  //         }
  //       } else if (OB_FAIL(submit_pt_update_role_task(
  //               partition->get_partition_key()))) {
  //         LOG_WARN("fail to submit pt update role task", K(ret),
  //                  "pkey", partition->get_partition_key());
  //       } else {
  //         //Update partition meta table without concern for error codes
  //         submit_pg_pt_update_task(pkeys);
  //         ++replica_count;
  //       }
  //     }

  //     if (OB_ITER_END == ret) {
  //       ret = OB_SUCCESS;
  //     }
  //   }

  //   if (NULL != partition_iter) {
  //     gctx_.par_ser_->revert_pg_iter(partition_iter);
  //   }
  //   LOG_INFO("submit all replicas report", K(ret));
  //   SERVER_EVENT_ADD("storage", "report_replica", K(ret), "replica_count", replica_count);
  // }
  return ret;
}

int ObService::recycle_replica()
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    //gctx_.par_ser_->garbage_clean();
  }
  return ret;
}

int ObService::clear_location_cache()
{
  // TODO baihua: implement after kvcache support clear interface
  int ret = OB_NOT_IMPLEMENT;
  return ret;
}

int ObService::set_ds_action(const obrpc::ObDebugSyncActionArg &arg)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!arg.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(arg), K(ret));
  } else if (OB_FAIL(GDS.set_global_action(arg.reset_, arg.clear_, arg.action_))) {
    LOG_WARN("set debug sync global action failed", K(ret), K(arg));
  }
  return ret;
}
int ObService::request_heartbeat(ObLeaseRequest &lease_request)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_FAIL(heartbeat_process_.init_lease_request(lease_request))) {
    LOG_WARN("init_lease_request failed", K(ret));
  }
  return ret;
}

// used by bootstrap/create_tenant
int ObService::batch_broadcast_schema(
    const obrpc::ObBatchBroadcastSchemaArg &arg,
    ObBatchBroadcastSchemaResult &result)
{
  int ret = OB_SUCCESS;
  ObMultiVersionSchemaService *schema_service = gctx_.schema_service_;
  const int64_t sys_schema_version = arg.get_sys_schema_version();
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret));
  } else if (!arg.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("arg is invaild", KR(ret), K(arg));
  } else if (OB_ISNULL(schema_service)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema_service is null", KR(ret));
  } else if (OB_FAIL(schema_service->async_refresh_schema(
             OB_SYS_TENANT_ID, sys_schema_version))) {
    LOG_WARN("fail to refresh sys schema", KR(ret), K(sys_schema_version));
  } else if (OB_FAIL(schema_service->broadcast_tenant_schema(
             arg.get_tenant_id(), arg.get_tables()))) {
    LOG_WARN("fail to broadcast tenant schema", KR(ret), K(arg));
  }
  result.set_ret(ret);
  return ret;
}

// get tenant's refreshed schema version in new mode
int ObService::get_tenant_refreshed_schema_version(
    const obrpc::ObGetTenantSchemaVersionArg &arg,
    obrpc::ObGetTenantSchemaVersionResult &result)
{
  int ret = OB_SUCCESS;
  result.schema_version_ = OB_INVALID_VERSION;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!arg.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret));
  } else if (OB_ISNULL(gctx_.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema_service is null", K(ret));
  } else if (OB_FAIL(gctx_.schema_service_->get_tenant_refreshed_schema_version(
             arg.tenant_id_, result.schema_version_, false/*core_version*/))) {
    LOG_WARN("fail to get tenant refreshed schema version", K(ret), K(arg));
  }
  return ret;
}

int ObService::sync_partition_table(const obrpc::Int64 &arg)
{
  return OB_NOT_SUPPORTED;
}

int ObService::get_server_heartbeat_expire_time(int64_t &lease_expire_time)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    lease_expire_time = lease_state_mgr_.get_heartbeat_expire_time();
  }
  return ret;
}

bool ObService::is_heartbeat_expired() const
{
  bool bret = false;  // returns false on error
  if (OB_UNLIKELY(!inited_)) {
    LOG_WARN("not init");
  } else {
    bret = !lease_state_mgr_.is_valid_heartbeat();
  }
  return bret;
}

bool ObService::is_svr_lease_valid() const
{
  // Determine if local lease is valid in OFS mode
  bool bret = false;
  if (OB_UNLIKELY(!inited_)) {
    LOG_WARN("not init");
  } else {
    bret = lease_state_mgr_.is_valid_lease();
  }
  return bret;
}

int ObService::set_tracepoint(const obrpc::ObAdminSetTPArg &arg)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    if (arg.event_name_.length() > 0) {
      ObSqlString str;
      if (OB_FAIL(str.assign(arg.event_name_))) {
        LOG_WARN("string assign failed", K(ret));
      } else {
        TP_SET_EVENT(str.ptr(), arg.error_code_, arg.occur_, arg.trigger_freq_);
      }
    } else {
      TP_SET_EVENT(arg.event_no_, arg.error_code_, arg.occur_, arg.trigger_freq_);
    }
    LOG_INFO("set event", K(arg));
  }
  return ret;
}

int ObService::cancel_sys_task(
    const share::ObTaskId &task_id)
{
  int ret = OB_SUCCESS;

  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (task_id.is_invalid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(task_id));
  } else if (OB_FAIL(SYS_TASK_STATUS_MGR.cancel_task(task_id))) {
    LOG_WARN("failed to cancel sys task", K(ret), K(task_id));
  }
  return ret;
}

int ObService::get_all_partition_status(int64_t &inactive_num, int64_t &total_num) const
{
  UNUSEDx(inactive_num, total_num);
  int ret = OB_NOT_SUPPORTED;
  // if (!inited_) {
  //   ret = OB_NOT_INIT;
  //   LOG_WARN("not init", K(ret));
  // } else if (OB_FAIL(gctx_.par_ser_->get_all_partition_status(inactive_num, total_num))) {
  //   LOG_WARN("fail to get all partition status", K(ret));
  // }
  return ret;
}

int ObService::detect_master_rs_ls(
    const ObDetectMasterRsArg &arg,
    obrpc::ObDetectMasterRsLSResult &result)
{
  int ret = OB_SUCCESS;
  const int64_t local_cluster_id = GCONF.cluster_id;
  const ObAddr &self_addr = gctx_.self_addr();
  ObAddr master_rs;
  ObLSReplica replica;
  ObLSInfo ls_info;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret));
  } else if (OB_ISNULL(gctx_.root_service_) || OB_ISNULL(gctx_.rs_mgr_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("rs or rs_mgr is null", KR(ret));
  } else if (!arg.is_valid()
             || arg.get_cluster_id() != local_cluster_id
             || arg.get_addr() != self_addr) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", KR(ret), K(arg), K(local_cluster_id), K(self_addr));
  } else if (OB_FAIL(gctx_.rs_mgr_->get_master_root_server(master_rs))) {
    LOG_WARN("fail to get master rs", KR(ret));
  } else if (OB_FAIL(fill_ls_replica(OB_SYS_TENANT_ID, SYS_LS, replica))) {
    if (OB_LS_NOT_EXIST == ret) {
      // case 1 : replica not exist
      ret = OB_SUCCESS;
      result.reset(); // only master_rs valid
      result.set_master_rs(master_rs);
    } else {
      LOG_WARN("fail to fill ls replica", KR(ret), K(SYS_LS));
    }
  } else if (replica.is_strong_leader()) {
    // case 2 : replica is leader, do not use in_service to check whether it is leader or not
    //          use in_service could lead to bad case: https://yuque.antfin.com/ob/rootservice/pbw2qw
    const ObLSReplica *leader = NULL;

    // FIXME: Need Use in memory table operator to fill log stream info
    if (OB_FAIL(ls_info.init_by_replica(replica))) {
      LOG_WARN("init by replica failed", KR(ret), K(replica));
    } else if (OB_FAIL(result.init(ObRole::LEADER, master_rs, replica, ls_info))) {
      LOG_WARN("fail to init result", KR(ret), K(master_rs), K(replica), K(ls_info));
    }
  } else {
    // case 3 : replica is follower
    replica.set_role(ObRole::FOLLOWER);
    ls_info.reset();
    result.reset(); // only ls info is invalid
    if (OB_FAIL(result.init(ObRole::FOLLOWER, master_rs, replica, ls_info))) {
      LOG_WARN("fail to init result", KR(ret), K(master_rs), K(replica), K(ls_info));
    }
  }
  return ret;
}

int ObService::get_root_server_status(ObGetRootserverRoleResult &get_role_result)
{
  int ret = OB_SUCCESS;
  int64_t tenant_id = OB_SYS_TENANT_ID;
  ObLSReplica replica;
  ObRole role = FOLLOWER;

  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret));
  } else if (OB_ISNULL(gctx_.root_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get invalid root_service", KR(ret));
  } else if (OB_FAIL(fill_ls_replica(tenant_id, SYS_LS, replica))) {
    if (OB_LS_NOT_EXIST == ret) {
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("fail to fill log_stream replica", KR(ret), K(tenant_id), K(SYS_LS));
    }
  } else {
    role = replica.get_role();
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(get_role_result.init(role, gctx_.root_service_->get_status()))) {
      LOG_WARN("fail to init a ObGetRootserverRoleResult", KR(ret), K(replica));
    }
  }
  return ret;
}

int ObService::refresh_sys_tenant_ls()
{
  int ret = OB_SUCCESS;
  const int64_t cluster_id = GCONF.cluster_id;
  const uint64_t tenant_id = OB_SYS_TENANT_ID;
  int64_t expire_renew_time = INT64_MAX;
  bool is_cache_hit = false;
  share::ObLSLocation location;
  if (OB_UNLIKELY(nullptr == GCTX.location_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("location service ptr is null", KR(ret));
  } else if (OB_FAIL(GCTX.location_service_->get(
          cluster_id, tenant_id, SYS_LS, expire_renew_time, is_cache_hit, location))) {
    LOG_WARN("fail to refresh sys tenant log stream",
             KR(ret), K(cluster_id), K(tenant_id), K(SYS_LS));
  } else {
#if !defined(NDEBUG)
    LOG_INFO("refresh sys tenant log stream success", K(location));
#endif
  }
  return ret;
}

int ObService::stop_partition_write(const obrpc::Int64 &switchover_timestamp, obrpc::Int64 &result)
{
  //TODO for switchover
  int ret = OB_SUCCESS;
  result = switchover_timestamp;
  return ret;
}

int ObService::check_partition_log(const obrpc::Int64 &switchover_timestamp, obrpc::Int64 &result)
{
  UNUSEDx(switchover_timestamp, result);
  // Check that the log of all replicas in local have reached synchronization status
  // The primary has stopped writing
  int ret = OB_NOT_SUPPORTED;

  // ObTenantDagScheduler is at tenant level now, check all tenants task
  // int64_t balance_task = 0;
  // omt::TenantIdList all_tenants;
  // all_tenants.set_label(ObModIds::OB_TENANT_ID_LIST);
  // if (OB_ISNULL(GCTX.omt_)) {
  //   ret = OB_ERR_UNEXPECTED;
  //   LOG_WARN("failed to get multi tenant from GCTX", K(ret));
  // } else {
  //   GCTX.omt_->get_tenant_ids(all_tenants);
  //   for (int64_t i = 0; OB_SUCC(ret) && i < all_tenants.size(); ++i) {
  //     uint64_t tenant_id = all_tenants[i];
  //     if (!is_virtual_tenant_id(tenant_id)) { // skip virtual tenant
  //       if (OB_SUCC(guard.switch_to(tenant_id))) {
  //         ObTenantDagScheduler *scheduler = nullptr;
  //         if (OB_ISNULL(scheduler = MTL(ObTenantDagScheduler *))) {
  //           ret = OB_ERR_UNEXPECTED;
  //           FLOG_WARN("MTL ObTenantDagScheduler is NULL", K(ret), K(scheduler), K(tenant_id));
  //         } else {
  //           balance_task += scheduler->get_dag_count(ObDagType::DAG_TYPE_MIGRATE);
  //         }
  //       }
  //     }
  //   }
  // }

  // if (OB_SUCC(ret)) {
  //   if (balance_task > 0) {
  //     ret = OB_EAGAIN;
  //     result = switchover_timestamp;
  //     LOG_INFO("observer already has task to do", K(switchover_timestamp), K(balance_task));
  //   } else if (OB_FAIL(gctx_.par_ser_->check_all_partition_sync_state(switchover_timestamp))) {
  //     LOG_WARN("fail to check_all_partition_sync_state", K(ret));
  //   } else {
  //     result = switchover_timestamp;
  //   }
  // }
  return ret;
}

int ObService::estimate_partition_rows(const obrpc::ObEstPartArg &arg,
                                       obrpc::ObEstPartRes &res) const
{
  int ret = OB_SUCCESS;
  LOG_DEBUG("receive estimate rows request", K(arg));
  if (!inited_) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("service is not inited", K(ret));
  } else if (OB_FAIL(sql::ObStorageEstimator::estimate_row_count(arg, res))) {
    LOG_WARN("failed to estimate partition rowcount", K(ret));
  }
  return ret;
}

int ObService::get_wrs_info(const obrpc::ObGetWRSArg &arg,
                            obrpc::ObGetWRSResult &result)
{
  UNUSEDx(arg, result);
  int ret = OB_NOT_SUPPORTED;
  return ret;
}

int ObService::refresh_memory_stat()
{
  return ObDumpTaskGenerator::generate_mod_stat_task();
}

int ObService::wash_memory_fragmentation()
{
  ObMallocAllocator::get_instance()->sync_wash();
  return OB_SUCCESS;
}

int ObService::renew_in_zone_hb(
    const share::ObInZoneHbRequest &arg,
    share::ObInZoneHbResponse &result)
{
  int ret = OB_SUCCESS;
  UNUSED(arg);
  UNUSED(result);
  return ret;
}

int ObService::broadcast_rs_list(const ObRsListArg &arg)
{
  int ret = OB_SUCCESS;
  if (!arg.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(arg));
  } else if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("service do not init", KR(ret), K(arg));
  } else if (OB_ISNULL(GCTX.rs_mgr_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("rs mgr is null", KR(ret), K(arg));
  } else if (OB_FAIL(GCTX.rs_mgr_->force_set_master_rs(arg.master_rs_))) {
    LOG_WARN("fail to set master rs", KR(ret), K(arg));
  } else {
    LOG_INFO("observer set master rs success", K(arg));
  }
  return ret;
}

int ObService::build_ddl_single_replica_request(const ObDDLBuildSingleReplicaRequestArg &arg)
{
  int ret = OB_SUCCESS;
  LOG_INFO("receive build single replica request", K(arg));
  if (OB_UNLIKELY(!arg.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(arg));
  } else {
    if (DDL_DROP_COLUMN == arg.ddl_type_
        || DDL_ADD_COLUMN_OFFLINE == arg.ddl_type_
        || DDL_COLUMN_REDEFINITION == arg.ddl_type_) {
      MTL_SWITCH(arg.tenant_id_) {
        int saved_ret = OB_SUCCESS;
        ObTenantDagScheduler *dag_scheduler = nullptr;
        ObComplementDataDag *dag = nullptr;
        if (OB_ISNULL(dag_scheduler = MTL(ObTenantDagScheduler *))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("dag scheduler is null", K(ret));
        } else if (OB_FAIL(dag_scheduler->alloc_dag(dag))) {
          LOG_WARN("fail to alloc dag", K(ret));
        } else if (OB_ISNULL(dag)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected error, dag is null", K(ret), KP(dag));
        } else if (OB_FAIL(dag->init(arg))) {
          LOG_WARN("fail to init complement data dag", K(ret), K(arg));
        } else if (OB_FAIL(dag->create_first_task())) {
          LOG_WARN("create first task failed", K(ret));
        } else if (OB_FAIL(dag_scheduler->add_dag(dag))) {
          saved_ret = ret;
          LOG_WARN("add dag failed", K(ret), K(arg));
        }
        if (OB_FAIL(ret) && OB_NOT_NULL(dag)) {
          (void) dag->handle_init_failed_ret_code(ret);
          dag_scheduler->free_dag(*dag);
          dag = nullptr;
        }
        if (OB_FAIL(ret)) {
          // RS does not retry send RPC to tablet leader when the dag exists.
          ret = OB_EAGAIN == saved_ret ? OB_SUCCESS : ret;
          ret = OB_SIZE_OVERFLOW == saved_ret ? OB_EAGAIN : ret;
        }
      }
      LOG_INFO("obs get rpc to build drop column dag", K(ret));
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("not supported ddl type", K(ret), K(arg));
    }

  }
  return ret;
}

int ObService::write_ddl_sstable_commit_log(const ObDDLWriteSSTableCommitLogArg &arg)
{
  int ret = OB_SUCCESS;
  LOG_INFO("receive write ddl sstable commit log", K(arg));
  if (OB_UNLIKELY(!arg.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(arg));
  } else {
    // ObDDLSSTableRedoWriter redo_writer;
    // const ObITable::TableKey &table_key = arg.table_key_;
    // if (OB_FAIL(ObDDLTableMergeTask::commit_ddl_sstable(table_key))) {
    //   LOG_WARN("fail to commit ddl sstable", K(ret), K(table_key));
    // } else if (OB_FAIL(redo_writer.init(table_key.get_partition_key().get_tenant_id(), table_key.tablet_id_))) {
    //   LOG_WARN("fail to init ObDDLSSTableRedoWriter", K(ret), K(table_key));
    // } else if (OB_FAIL(redo_writer.write_commit_log(table_key))) {
    //   LOG_WARN("fail to write commit log", K(ret), K(table_key));
    // }
  }
  return ret;
}

int ObService::inner_fill_tablet_info_(
    const int64_t tenant_id,
    const ObTabletID &tablet_id,
    storage::ObLS *ls,
    ObTabletReplica &tablet_replica,
    share::ObTabletReplicaChecksumItem &tablet_checksum,
    const bool need_checksum)
{
  ObLSHandle ls_handle;
  ObTabletHandle tablet_handle;
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("service not inited", KR(ret));
  } else if (!tablet_id.is_valid()
             || OB_INVALID_TENANT_ID == tenant_id
             || OB_ISNULL(ls)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument or nullptr", KR(ret), K(tablet_id), K(tenant_id));
  } else if (OB_ISNULL(ls->get_tablet_svr())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get_tablet_svr is null", KR(ret), K(tenant_id), K(tablet_id));
  } else if (OB_FAIL(ls->get_tablet_svr()->get_tablet(
      tablet_id,
      tablet_handle,
      ObTabletCommon::NO_CHECK_GET_TABLET_TIMEOUT_US))) {
    if (OB_TABLET_NOT_EXIST != ret) {
      LOG_WARN("get tablet failed", KR(ret), K(tenant_id), K(tablet_id));
    }
  } else if (OB_ISNULL(gctx_.config_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("gctx_.config_ is null", KR(ret), K(tenant_id), K(tablet_id));
  } else {
    const common::ObAddr &addr = gctx_.self_addr();
    const int64_t snapshot_version = tablet_handle.get_obj()->get_tablet_meta().report_status_.merge_snapshot_version_;
    const ObLSID &ls_id = ls->get_ls_id();
    int64_t data_size = 0;
    int64_t required_size = 0;
    ObArray<int64_t> column_checksums;
    if (OB_FAIL(tablet_handle.get_obj()->get_tablet_report_info(snapshot_version, column_checksums,
        data_size, required_size, need_checksum))) {
      LOG_WARN("fail to get tablet report info from tablet", KR(ret), K(tenant_id), K(tablet_id));
    } else if (OB_FAIL(tablet_replica.init(
        tenant_id,
        tablet_id,
        ls_id,
        addr,
        snapshot_version,
        data_size,
        required_size))) {
      LOG_WARN("fail to init a tablet replica", KR(ret), K(tenant_id),
          K(tablet_id), K(tablet_replica));
    } else if (!need_checksum) {
    } else if (OB_FAIL(tablet_checksum.column_meta_.init(column_checksums))) {
      LOG_WARN("fail to init report column meta with column_checksums", KR(ret), K(column_checksums));
    } else {
      tablet_checksum.tenant_id_ = tenant_id;
      tablet_checksum.ls_id_ = ls->get_ls_id();
      tablet_checksum.tablet_id_ = tablet_id;
      tablet_checksum.server_ = gctx_.self_addr();
      tablet_checksum.snapshot_version_ = snapshot_version;
      tablet_checksum.row_count_ = tablet_handle.get_obj()->get_tablet_meta().report_status_.row_count_;
      tablet_checksum.data_checksum_ = tablet_handle.get_obj()->get_tablet_meta().report_status_.data_checksum_;
    }
  }
  return ret;
}

int ObService::fill_tablet_report_info(
    const uint64_t tenant_id,
    const ObLSID &ls_id,
    const ObTabletID &tablet_id,
    ObTabletReplica &tablet_replica,
    share::ObTabletReplicaChecksumItem &tablet_checksum,
    const bool need_checksum)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("service not inited", KR(ret));
  } else if (!tablet_id.is_valid() || !ls_id.is_valid() || OB_INVALID_TENANT_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tablet_id), K(ls_id), K(tenant_id));
  } else {
    MTL_SWITCH(tenant_id) {
      ObSharedGuard<ObLSIterator> ls_iter;
      ObTabletHandle tablet_handle;
      ObLSHandle ls_handle;
      storage::ObLS *ls = nullptr;
      ObLSService* ls_svr = nullptr;
      if (OB_ISNULL(ls_svr = MTL(ObLSService*))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("MTL ObLSService is null", KR(ret), K(tenant_id));
      } else if (OB_FAIL(ls_svr->get_ls(ls_id, ls_handle, ObLSGetMod::OBSERVER_MOD))) {
        if (OB_LS_NOT_EXIST != ret) {
          LOG_WARN("fail to get log_stream's ls_handle", KR(ret), K(tenant_id), K(ls_id));
        } else {
          LOG_TRACE("log stream not exist in this tenant", KR(ret), K(tenant_id), K(ls_id));
        }
      } else if (FALSE_IT(ls = ls_handle.get_ls())) {
      } else if (OB_FAIL(inner_fill_tablet_info_(tenant_id,
                                                 tablet_id,
                                                 ls,
                                                 tablet_replica,
                                                 tablet_checksum,
                                                 need_checksum))) {
        if (OB_TABLET_NOT_EXIST != ret) {
          LOG_WARN("fail to inner fill tenant's tablet replica", KR(ret),
                    K(tenant_id), K(tablet_id), K(ls), K(tablet_replica), K(tablet_checksum), K(need_checksum));
        } else {
          LOG_TRACE("tablet not exist in this log stream", KR(ret),
                    K(tenant_id), K(tablet_id), K(ls), K(tablet_replica), K(tablet_checksum), K(need_checksum));
        }
      }
    }
  }
  return ret;
}

int ObService::fill_ls_replica(
    const uint64_t tenant_id,
    const ObLSID &ls_id,
    share::ObLSReplica &replica)
{
  int ret = OB_SUCCESS;
  uint64_t unit_id = common::OB_INVALID_ID;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("service not inited", KR(ret));
  } else if (!ls_id.is_valid()
             || OB_INVALID_TENANT_ID == tenant_id
             || OB_ISNULL(gctx_.config_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(ls_id));
  } else if (OB_FAIL(GCTX.omt_->get_unit_id(tenant_id, unit_id))) {
    LOG_WARN("get tenant unit id failed", KR(ret), K(tenant_id), K(ls_id));
  } else {
    MTL_SWITCH(tenant_id) {
      ObLSHandle ls_handle;
      palf::PalfHandleGuard palf_handle_guard;
      ObLSService *ls_svr = nullptr;
      logservice::ObLogService *log_service = nullptr;
      common::ObRole role = FOLLOWER;
      ObMemberList ob_member_list;
      ObLSReplica::MemberList member_list;
      int64_t proposal_id = 0;
      int64_t paxos_replica_number = 0;
      ObLSRestoreStatus restore_status;
      if (OB_ISNULL(ls_svr = MTL(ObLSService*))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("MTL ObLSService is null", KR(ret), K(tenant_id));
      } else if (OB_FAIL(ls_svr->get_ls(
            ObLSID(ls_id),
            ls_handle, ObLSGetMod::OBSERVER_MOD))) {
        LOG_WARN("get ls handle failed", KR(ret));
      } else if (OB_FAIL(ls_handle.get_ls()->get_paxos_member_list(ob_member_list, paxos_replica_number))) {
        LOG_WARN("get paxos_member_list from ObLS failed", KR(ret));
      } else if (OB_FAIL(ls_handle.get_ls()->get_restore_status(restore_status))) {
        LOG_WARN("get restore status failed", KR(ret));
      } else if (OB_ISNULL(log_service = MTL(logservice::ObLogService*))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("MTL ObLogService is null", KR(ret), K(tenant_id));
      } else if (OB_FAIL(log_service->open_palf(ls_id, palf_handle_guard))) {
        LOG_WARN("open palf failed", KR(ret), K(tenant_id), K(ls_id));
      } else if (OB_FAIL(palf_handle_guard.get_role(role, proposal_id))) {
        LOG_WARN("get role failed", KR(ret), K(tenant_id), K(ls_id));
      } else if (OB_FAIL(replica.init(
            0,          /*create_time_us*/
            0,          /*modify_time_us*/
            tenant_id,  /*tenant_id*/
            ls_id,      /*ls_id*/
            gctx_.self_addr(),         /*server*/
            gctx_.config_->mysql_port, /*sql_port*/
            role,                      /*role*/
            REPLICA_TYPE_FULL,         /*replica_type*/
            proposal_id,              /*proposal_id*/
            REPLICA_STATUS_NORMAL,     /*replica_status*/
            restore_status,            /*restore_status*/
            100,                       /*memstore_percent*/
            unit_id,                   /*unit_id*/
            gctx_.config_->zone.str(), /*zone*/
            paxos_replica_number,                    /*paxos_replica_number*/
            0,                         /*data_size*/
            0))) {                     /*required_size*/
        LOG_WARN("fail to init a ls replica", KR(ret), K(tenant_id), K(ls_id), K(role),
                 K(proposal_id), K(unit_id), K(paxos_replica_number));
      } else if (OB_FAIL(ObLSReplica::transform_ob_member_list(ob_member_list, member_list))) {
        LOG_WARN("fail to transfrom ob_member_list into member_list", KR(ret), K(ob_member_list));
      } else if (OB_FAIL(replica.set_member_list(member_list))) {
        LOG_WARN("fail to set member_list", KR(ret), K(member_list), K(replica));
      } else {
        LOG_TRACE("finish fill ls replica", KR(ret), K(tenant_id), K(ls_id), K(replica));
      }
    }
  }
  return ret;
}

int ObService::check_backup_dest_connectivity(const obrpc::ObCheckBackupConnectivityArg &arg)
{
  int ret = OB_SUCCESS;
  share::ObBackupDestCheck backup_check;
  share::ObBackupPath path;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObService not init", K(ret));
  } else if (!arg.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(arg), K(ret));
  } else if (OB_FAIL(path.init(arg.check_path_))) {
    LOG_WARN("failed to init path", K(ret), K(arg));
  } else if (OB_FAIL(backup_check.check_backup_dest_connectivity(arg.tenant_id_, arg.backup_path_, path))) {
    LOG_WARN("failed to check backup dest connectivity", K(ret), K(arg));
  }
  return ret;
}

int ObService::estimate_tablet_block_count(const obrpc::ObEstBlockArg &arg,
                                           obrpc::ObEstBlockRes &res) const
{
  int ret = OB_SUCCESS;
  LOG_DEBUG("receive estimate tablet block count request", K(arg));
  if (!inited_) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("service is not inited", K(ret));
  } else if (OB_FAIL(sql::ObStorageEstimator::estimate_block_count(arg, res))) {
    LOG_WARN("failed to estimate partition rowcount", K(ret));
  }
  return ret;
}

}// end namespace observer
}// end namespace oceanbase
