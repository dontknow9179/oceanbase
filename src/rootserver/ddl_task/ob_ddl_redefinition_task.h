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

#ifndef OCEANBASE_ROOTSERVER_OB_DDL_REDEFINITION_TASK_H
#define OCEANBASE_ROOTSERVER_OB_DDL_REDEFINITION_TASK_H

#include "rootserver/ddl_task/ob_ddl_task.h"

namespace oceanbase
{
namespace rootserver
{
class ObRootService;

class ObDDLRedefinitionSSTableBuildTask : public share::ObAsyncTask
{
public:
  ObDDLRedefinitionSSTableBuildTask(
      const int64_t task_id,
      const uint64_t tenant_id,
      const int64_t data_table_id,
      const int64_t dest_table_id,
      const int64_t schema_version,
      const int64_t snapshot_version,
      const int64_t execution_id,
      const ObSQLMode &sql_mode,
      const common::ObCurTraceId::TraceId &trace_id,
      const int64_t parallelism,
      const bool use_heap_table_ddl_plan,
      ObRootService *root_service);
  int init(
      const ObTableSchema &orig_table_schema,
      const AlterTableSchema &alter_table_schema,
      const ObTimeZoneInfoWrap &tz_info_wrap);
  virtual ~ObDDLRedefinitionSSTableBuildTask() = default;
  virtual int process() override;
  virtual int64_t get_deep_copy_size() const override { return sizeof(*this); }
  virtual ObAsyncTask *deep_copy(char *buf, const int64_t buf_size) const override;
private:
  bool is_inited_;
  uint64_t tenant_id_;
  int64_t task_id_;
  int64_t data_table_id_;
  int64_t dest_table_id_;
  int64_t schema_version_;
  int64_t snapshot_version_;
  int64_t execution_id_;
  ObSQLMode sql_mode_;
  ObTimeZoneInfoWrap tz_info_wrap_;
  share::ObColumnNameMap col_name_map_;
  common::ObCurTraceId::TraceId trace_id_;
  int64_t parallelism_;
  bool use_heap_table_ddl_plan_;
  ObRootService *root_service_;
};

class ObSyncTabletAutoincSeqCtx final
{
public:
  ObSyncTabletAutoincSeqCtx();
  ~ObSyncTabletAutoincSeqCtx() {}
  int init(uint64_t tenant_id, int64_t src_table_id, int64_t dest_table_id);
  int sync();
  bool is_inited() const { return is_inited_; }
  TO_STRING_KV(K_(is_inited), K_(is_synced), K_(tenant_id), K_(orig_src_tablet_ids), K_(src_tablet_ids),
               K_(dest_tablet_ids), K_(autoinc_params));
private:
  int build_ls_to_tablet_map(
      share::ObLocationService *location_service,
      const uint64_t tenant_id,
      const common::ObIArray<share::ObMigrateTabletAutoincSeqParam> &tablet_ids,
      const int64_t timeout,
      const bool force_renew,
      const bool by_src_tablet,
      common::hash::ObHashMap<share::ObLSID, common::ObSEArray<share::ObMigrateTabletAutoincSeqParam, 1>> &map);
  template<typename P, typename A>
  int call_and_process_all_tablet_autoinc_seqs(P &proxy, A &arg, const bool is_get);
  bool is_error_need_retry(const int ret_code) const
  {
    return common::OB_TIMEOUT == ret_code || common::OB_TABLET_NOT_EXIST == ret_code || common::OB_NOT_MASTER == ret_code ||
           common::OB_EAGAIN == ret_code;
  }
private:
  static const int64_t MAP_BUCKET_NUM = 1024;
  bool is_inited_;
  bool is_synced_;
  uint64_t tenant_id_;
  ObSEArray<ObTabletID, 1> orig_src_tablet_ids_;
  ObSEArray<ObTabletID, 1> src_tablet_ids_;
  ObSEArray<ObTabletID, 1> dest_tablet_ids_;
  ObSEArray<share::ObMigrateTabletAutoincSeqParam, 1> autoinc_params_;
};

class ObDDLRedefinitionTask : public ObDDLTask
{
public:
  ObDDLRedefinitionTask(): 
    ObDDLTask(share::DDL_INVALID), lock_(), wait_trans_ctx_(), sync_tablet_autoinc_seq_ctx_(),
    build_replica_request_time_(0), complete_sstable_job_ret_code_(INT64_MAX), alter_table_arg_(),
    dependent_task_result_map_(), snapshot_held_(false), has_synced_autoincrement_(false),
    has_synced_stats_info_(false), update_autoinc_job_ret_code_(INT64_MAX), update_autoinc_job_time_(0),
    check_table_empty_job_ret_code_(INT64_MAX), check_table_empty_job_time_(0) {}
  virtual ~ObDDLRedefinitionTask(){};
  virtual int process() = 0;
  virtual int update_complete_sstable_job_status(
      const common::ObTabletID &tablet_id,
      const int64_t snapshot_version,
      const int64_t execution_id,
      const int ret_code) = 0;
  int on_child_task_finish(
      const ObDDLTaskKey &child_task_key,
      const int ret_code);
  virtual int serialize_params_to_message(char *buf, const int64_t buf_size, int64_t &pos) const override;
  virtual int deserlize_params_from_message(const char *buf, const int64_t buf_size, int64_t &pos) override;
  virtual int64_t get_serialize_param_size() const override;
  int notify_update_autoinc_finish(const uint64_t autoinc_val, const int ret_code);
protected:
  int prepare(const share::ObDDLTaskStatus next_task_status);
  int lock_table(const share::ObDDLTaskStatus next_task_status);
  int check_table_empty(const share::ObDDLTaskStatus next_task_status);
  int obtain_snapshot();
  bool check_can_validate_column_checksum(
      const bool is_oracle_mode,
      const share::schema::ObColumnSchemaV2 &src_column_schema,
      const share::schema::ObColumnSchemaV2 &dest_column_schema);
  int get_validate_checksum_columns_id(
      const share::schema::ObTableSchema &data_table_schema,
      const share::schema::ObTableSchema &dest_table_schema,
      common::hash::ObHashMap<uint64_t, uint64_t> &validate_checksum_column_ids);
  int check_data_dest_tables_columns_checksum(const int64_t execution_id);
  int unlock_table();
  int fail();
  int success();
  int hold_snapshot(const int64_t snapshot_version);
  int release_snapshot(const int64_t snapshot_version);
  int cleanup();
  int add_constraint_ddl_task(const int64_t constraint_id, share::schema::ObSchemaGetterGuard &schema_guard);
  int add_fk_ddl_task(const int64_t fk_id, share::schema::ObSchemaGetterGuard &schema_guard);
  int sync_auto_increment_position();
  int modify_autoinc(const share::ObDDLTaskStatus next_task_status);
  int finish();
  int check_health();
  int check_update_autoinc_end(bool &is_end);
  int check_check_table_empty_end(bool &is_end);
  int sync_stats_info();
  int sync_table_level_stats_info(common::ObMySQLTransaction &trans, const ObTableSchema &data_table_schema);
  int sync_partition_level_stats_info(common::ObMySQLTransaction &trans,
                                      const ObTableSchema &data_table_schema,
                                      const ObTableSchema &new_table_schema);
  int sync_column_level_stats_info(common::ObMySQLTransaction &trans,
                                   const ObTableSchema &data_table_schema,
                                   const ObTableSchema &new_table_schema,
                                   ObSchemaGetterGuard &schema_guard);
  int sync_one_column_table_level_stats_info(common::ObMySQLTransaction &trans,
                                             const ObTableSchema &data_table_schema,
                                             const uint64_t old_col_id,
                                             const uint64_t new_col_id);
  int sync_one_column_partition_level_stats_info(common::ObMySQLTransaction &trans,
                                                 const ObTableSchema &data_table_schema,
                                                 const ObTableSchema &new_table_schema,
                                                 const uint64_t old_col_id,
                                                 const uint64_t new_col_id);   
  int generate_sync_partition_level_stats_sql(const char *table_name,
                                              const ObIArray<ObObjectID> &src_partition_ids,
                                              const ObIArray<ObObjectID> &dest_partition_ids,
                                              const int64_t batch_start,
                                              const int64_t batch_end,
                                              ObSqlString &sql_string);
  int generate_sync_column_partition_level_stats_sql(const char *table_name,
                                                     const ObIArray<ObObjectID> &src_partition_ids,
                                                     const ObIArray<ObObjectID> &dest_partition_ids,
                                                     const uint64_t old_col_id,
                                                     const uint64_t new_col_id,
                                                     const int64_t batch_start,
                                                     const int64_t batch_end,
                                                     ObSqlString &sql_string);
  int sync_tablet_autoinc_seq();
  int check_need_rebuild_constraint(const ObTableSchema &table_schema,
                                    ObIArray<uint64_t> &constraint_ids,
                                    bool &need_rebuild_constraint);
  int check_need_check_table_empty(bool &need_check_table_empty);
protected:
  struct DependTaskStatus final
  {
  public:
    DependTaskStatus()
      : ret_code_(INT64_MAX), task_id_(0)
    {}
    ~DependTaskStatus() = default;
  public:
    int64_t ret_code_;
    int64_t task_id_;
  };
  static const int64_t MAX_DEPEND_OBJECT_COUNT = 100L;
  static const int64_t RETRY_INTERVAL = 1 * 1000 * 1000; // 1s
  static const int64_t RETRY_LIMIT = 100;   
  common::TCRWLock lock_;
  ObDDLWaitTransEndCtx wait_trans_ctx_;
  ObSyncTabletAutoincSeqCtx sync_tablet_autoinc_seq_ctx_;
  int64_t build_replica_request_time_;
  int64_t complete_sstable_job_ret_code_;
  obrpc::ObAlterTableArg alter_table_arg_;
  common::hash::ObHashMap<ObDDLTaskKey, DependTaskStatus> dependent_task_result_map_;
  bool snapshot_held_;
  bool has_synced_autoincrement_;
  bool has_synced_stats_info_;
  int64_t update_autoinc_job_ret_code_;
  int64_t update_autoinc_job_time_;
  int64_t check_table_empty_job_ret_code_;
  int64_t check_table_empty_job_time_;
};

}  // end namespace rootserver
}  // end namespace oceanbase

#endif  // OCEANBASE_ROOTSERVER_OB_TABLE_REDEFINITION_TASK_H
