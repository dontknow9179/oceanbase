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

#ifndef OCEANBASE_ROOTSERVER_OB_DDL_TASK_H_
#define OCEANBASE_ROOTSERVER_OB_DDL_TASK_H_

#include "lib/container/ob_array.h"
#include "lib/thread/ob_async_task_queue.h"
#include "share/ob_ddl_task_executor.h"
#include "share/ob_rpc_struct.h"
#include "share/schema/ob_schema_struct.h"
#include "share/ob_ddl_common.h"
#include "rootserver/ddl_task/ob_ddl_single_replica_executor.h"

namespace oceanbase
{
namespace rootserver
{
class ObRootService;

struct ObDDLTaskRecord;
struct ObDDLTaskKey final
{
public:
  ObDDLTaskKey();
  ObDDLTaskKey(const int64_t object_id, const int64_t schema_version);
  ~ObDDLTaskKey() = default;
  uint64_t hash() const;
  bool operator==(const ObDDLTaskKey &other) const;
  bool is_valid() const { return OB_INVALID_ID != object_id_ && schema_version_ > 0; }
  int assign(const ObDDLTaskKey &other);
  TO_STRING_KV(K_(object_id), K_(schema_version));
public:
  int64_t object_id_;
  int64_t schema_version_;
};

struct ObDDLTaskRecord final
{
public:
  ObDDLTaskRecord() { reset(); }
  ~ObDDLTaskRecord() {}
  bool is_valid() const;
  void reset();
  TO_STRING_KV(K_(task_id), K_(parent_task_id), K_(ddl_type), K_(trace_id), K_(task_status), K_(tenant_id), K_(object_id),
      K_(schema_version), K_(target_object_id), K_(snapshot_version), K_(message), K_(task_version), K_(ret_code), K_(execution_id));
public:
  static const int64_t MAX_MESSAGE_LENGTH = 4096;
  typedef common::ObFixedLengthString<MAX_MESSAGE_LENGTH> TaskMessage;
public:
  int64_t task_id_;
  int64_t parent_task_id_;
  share::ObDDLType ddl_type_;
  common::ObCurTraceId::TraceId trace_id_;
  int64_t task_status_;
  uint64_t tenant_id_;
  uint64_t object_id_;
  uint64_t schema_version_;
  uint64_t target_object_id_;
  int64_t snapshot_version_;
  ObString message_;
  int64_t task_version_;
  int64_t ret_code_;
  int64_t execution_id_;
  ObString ddl_stmt_str_;
};

struct ObCreateDDLTaskParam final
{
public:
  ObCreateDDLTaskParam();
  ObCreateDDLTaskParam(const uint64_t tenant_id,
                       const share::ObDDLType &type,
                       const ObTableSchema *src_table_schema,
                       const ObTableSchema *dest_table_schema,
                       const int64_t object_id,
                       const int64_t schema_version,
                       const int64_t parallelism,
                       ObIAllocator *allocator,
                       const obrpc::ObDDLArg *ddl_arg = nullptr,
                       const int64_t parent_task_id = 0);
  ~ObCreateDDLTaskParam() = default;
  bool is_valid() const { return OB_INVALID_ID != tenant_id_ && type_ > share::DDL_INVALID
                                 && type_ < share::DDL_MAX && nullptr != allocator_; }
  TO_STRING_KV(K_(tenant_id), K_(object_id), K_(schema_version), K_(parallelism), K_(parent_task_id), K_(type),
               KPC_(src_table_schema), KPC_(dest_table_schema), KPC_(ddl_arg));
public:
  uint64_t tenant_id_;
  int64_t object_id_;
  int64_t schema_version_;
  int64_t parallelism_;
  int64_t parent_task_id_;
  share::ObDDLType type_;
  const ObTableSchema *src_table_schema_;
  const ObTableSchema *dest_table_schema_;
  const obrpc::ObDDLArg *ddl_arg_;
  common::ObIAllocator *allocator_;
};

class ObDDLTaskRecordOperator final
{
public:
  static int update_task_status(
      common::ObISQLClient &proxy,
      const uint64_t tenant_id,
      const int64_t task_id,
      const int64_t task_status);

  static int update_snapshot_version(
      common::ObISQLClient &sql_client,
      const uint64_t tenant_id,
      const int64_t task_id,
      const int64_t snapshot_version);

  static int update_ret_code(
      common::ObISQLClient &sql_client,
      const uint64_t tenant_id,
      const int64_t task_id,
      const int64_t ret_code);

  static int update_execution_id(
      common::ObISQLClient &sql_client,
      const uint64_t tenant_id,
      const int64_t task_id,
      const int64_t execution_id);

  static int update_message(
      common::ObISQLClient &proxy,
      const uint64_t tenant_id,
      const int64_t task_id,
      const ObString &message);

  static int delete_record(
      common::ObMySQLProxy &proxy,
      const uint64_t tenant_id,
      const int64_t task_id);

  static int select_for_update(
      common::ObMySQLTransaction &trans,
      const uint64_t tenant_id,
      const int64_t task_id,
      int64_t &task_status,
      int64_t &execution_id);

  static int get_all_record(
      common::ObMySQLProxy &proxy,
      common::ObIAllocator &allocator,
      common::ObIArray<ObDDLTaskRecord> &records);

  static int check_is_adding_constraint(
     common::ObMySQLProxy *proxy,
     common::ObIAllocator &allocator,
     const uint64_t table_id,
     bool &is_building);
  
  static int check_has_long_running_ddl(
     common::ObMySQLProxy *proxy,
     const uint64_t tenant_id,
     const uint64_t table_id,
     bool &has_long_running_ddl);

  static int check_has_conflict_ddl(
      common::ObMySQLProxy *proxy,
      const uint64_t tenant_id,
      const uint64_t table_id,
      const int64_t task_id,
      const share::ObDDLType ddl_type,
      bool &has_conflict_ddl);

  static int insert_record(
      common::ObISQLClient &proxy,
      const ObDDLTaskRecord &record);

  static int to_hex_str(const ObString &src, ObSqlString &dst);

private:
  static int fill_task_record(
      const common::sqlclient::ObMySQLResult *result_row,
      common::ObIAllocator &allocator,
      ObDDLTaskRecord &task_record);

  static int64_t get_record_id(share::ObDDLType ddl_type, int64_t origin_id);
};

class ObDDLWaitTransEndCtx
{
public:
  enum WaitTransType
  {
    MIN_WAIT_TYPE = 0,
    WAIT_SCHEMA_TRANS,
    WAIT_SSTABLE_TRANS,
    MAX_WAIT_TYPE
  };
public:
  ObDDLWaitTransEndCtx();
  ~ObDDLWaitTransEndCtx();
  int init(
      const uint64_t tenant_id,
      const uint64_t table_id,
      const WaitTransType wait_trans_type,
      const int64_t wait_version);
  void reset();
  bool is_inited() const { return is_inited_; }
  int try_wait(bool &is_trans_end, int64_t &snapshot_version, const bool need_wait_trans_end = true);
  TO_STRING_KV(K(is_inited_), K_(tenant_id), K(table_id_), K(is_trans_end_), K(wait_type_),
      K(wait_version_), K(tablet_ids_), K(snapshot_array_));

private:
  static bool is_wait_trans_type_valid(const WaitTransType wait_trans_type);
  int get_snapshot_check_list(
      common::ObIArray<common::ObTabletID> &need_check_tablets,
      ObIArray<int64_t> &tablet_pos_indexes);
  int get_snapshot(int64_t &snapshot_version);

  // check if all transactions before a schema version have ended
  int check_schema_trans_end(
      const int64_t schema_version,
      const common::ObIArray<common::ObTabletID> &tablet_ids,
      common::ObIArray<int> &ret_array,
      common::ObIArray<int64_t> &snapshot_array,
      const uint64_t tenant_id,
      obrpc::ObSrvRpcProxy *rpc_proxy,
      share::ObLocationService *location_service,
      const bool need_wait_trans_end);

  // check if all transactions before a timestamp have ended
   int check_sstable_trans_end(
      const uint64_t tenant_id,
      const int64_t sstable_exist_ts,
      const common::ObIArray<common::ObTabletID> &tablet_ids,
      obrpc::ObSrvRpcProxy *rpc_proxy,
      share::ObLocationService *location_service,
      common::ObIArray<int> &ret_array,
      common::ObIArray<int64_t> &snapshot_array);
private:
  static const int64_t INDEX_SNAPSHOT_VERSION_DIFF = 100 * 1000; // 100ms
  bool is_inited_;
  uint64_t tenant_id_;
  uint64_t table_id_;
  bool is_trans_end_;
  WaitTransType wait_type_;
  int64_t wait_version_;
  common::ObArray<common::ObTabletID> tablet_ids_;
  common::ObArray<int64_t> snapshot_array_;
};

class ObDDLTask : public common::ObDLinkBase<ObDDLTask>
{
public:
  explicit ObDDLTask(const share::ObDDLType task_type)
    : is_inited_(false), need_retry_(true), is_running_(false),
      task_type_(task_type), trace_id_(), tenant_id_(0), object_id_(0), schema_version_(0),
      target_object_id_(0), task_status_(share::ObDDLTaskStatus::PREPARE), snapshot_version_(0), ret_code_(OB_SUCCESS), task_id_(0),
      parent_task_id_(0), parent_task_key_(), task_version_(0), parallelism_(0),
      allocator_(lib::ObLabel("DdlTask")), compat_mode_(lib::Worker::CompatMode::INVALID), err_code_occurence_cnt_(0),
      delay_schedule_time_(0), next_schedule_ts_(0), execution_id_(0)
  {}
  virtual ~ObDDLTask() {}
  virtual int process() = 0;
  virtual bool is_valid() const { return is_inited_; }
  typedef common::ObCurTraceId::TraceId TraceId;
  virtual const TraceId &get_trace_id() const { return trace_id_; }
  virtual int set_trace_id(const TraceId &trace_id) { return trace_id_.set(trace_id.get()); }
  virtual bool need_retry() const { return need_retry_; };
  share::ObDDLType get_task_type() const { return task_type_; }
  void set_not_running() { ATOMIC_SET(&is_running_, false); }
  bool try_set_running() { return !ATOMIC_CAS(&is_running_, false, true); }
  uint64_t get_tenant_id() const { return tenant_id_; }
  uint64_t get_object_id() const { return object_id_; }
  int64_t get_schema_version() const { return schema_version_; }
  uint64_t get_target_object_id() const { return target_object_id_; }
  int64_t get_task_status() const { return task_status_; }
  int64_t get_snapshot_version() const { return snapshot_version_; }
  int get_ddl_type_str(const int64_t ddl_type, const char *&ddl_type_str);
  int64_t get_ret_code() const { return ret_code_; }
  int64_t get_task_id() const { return task_id_; }
  ObDDLTaskKey get_task_key() const { return ObDDLTaskKey(target_object_id_, schema_version_); }
  int64_t get_parent_task_id() const { return parent_task_id_; }
  int64_t get_task_version() const { return task_version_; }
  int64_t get_execution_id() const { return execution_id_; }
  int64_t get_parallelism() const { return parallelism_; }
  static int deep_copy_table_arg(common::ObIAllocator &allocator, const obrpc::ObDDLArg &source_arg, obrpc::ObDDLArg &dest_arg);
  static int fetch_new_task_id(ObMySQLProxy &sql_proxy, int64_t &new_task_id);
  virtual int serialize_params_to_message(char *buf, const int64_t buf_size, int64_t &pos) const = 0;
  virtual int deserlize_params_from_message(const char *buf, const int64_t buf_size, int64_t &pos) = 0;
  virtual int64_t get_serialize_param_size() const = 0;
  const ObString &get_ddl_stmt_str() const { return ddl_stmt_str_; }
  int set_ddl_stmt_str(const ObString &ddl_stmt_str);
  int convert_to_record(ObDDLTaskRecord &task_record, common::ObIAllocator &allocator);
  int switch_status(share::ObDDLTaskStatus new_status, const int ret_code);
  int refresh_status();
  int refresh_schema_version();
  int remove_task_record();
  int report_error_code(const ObString &forward_user_message, const int64_t affected_rows = 0);
  int wait_trans_end(
      ObDDLWaitTransEndCtx &wait_trans_ctx,
      const share::ObDDLTaskStatus next_task_status);
  lib::Worker::CompatMode get_compat_mode() { return compat_mode_; }
  int batch_release_snapshot(
      const int64_t snapshot_version, 
      const common::ObIArray<common::ObTabletID> &tablet_ids);
  void set_sys_task_id(const TraceId &sys_task_id) { sys_task_id_ = sys_task_id; }
  const TraceId &get_sys_task_id() const { return sys_task_id_; }
  void calc_next_schedule_ts(int ret_code);
  bool need_schedule() { return next_schedule_ts_ <= ObTimeUtility::current_time(); }
  int push_execution_id();
  bool is_replica_build_need_retry(const int ret_code);
  #ifdef ERRSIM
  int check_errsim_error();
  #endif
  VIRTUAL_TO_STRING_KV(
      K(is_inited_), K(need_retry_), K(task_type_), K(trace_id_),
      K(tenant_id_), K(object_id_), K(schema_version_),
      K(target_object_id_), K(task_status_), K(snapshot_version_),
      K_(ret_code), K_(task_id), K_(parent_task_id), K_(parent_task_key),
      K_(task_version), K_(parallelism), K_(ddl_stmt_str), K_(compat_mode),
      K_(sys_task_id), K_(err_code_occurence_cnt), K_(next_schedule_ts), K_(delay_schedule_time), K(execution_id_));
protected:
  int check_is_latest_execution_id(const int64_t execution_id, bool &is_latest);
  virtual bool is_error_need_retry(const int ret_code)
  {
    return !share::ObIDDLTask::in_ddl_retry_black_list(ret_code) && (share::ObIDDLTask::in_ddl_retry_white_list(ret_code)
             || MAX_ERR_TOLERANCE_CNT > ++err_code_occurence_cnt_);
  }
protected:
  static const int64_t MAX_ERR_TOLERANCE_CNT = 3L; // Max torlerance count for error code.
  bool is_inited_;
  bool need_retry_;
  bool is_running_;
  share::ObDDLType task_type_;
  TraceId trace_id_;
  uint64_t tenant_id_;
  uint64_t object_id_;
  uint64_t schema_version_;
  uint64_t target_object_id_;
  share::ObDDLTaskStatus task_status_;
  int64_t snapshot_version_;
  int64_t ret_code_;
  int64_t task_id_;
  int64_t parent_task_id_;
  ObDDLTaskKey parent_task_key_;
  int64_t task_version_;
  int64_t parallelism_;
  ObString ddl_stmt_str_;
  common::ObArenaAllocator allocator_;
  lib::Worker::CompatMode compat_mode_;
  TraceId sys_task_id_;
  int64_t err_code_occurence_cnt_; // occurence count for all error return codes not in white list.
  int64_t delay_schedule_time_;
  int64_t next_schedule_ts_;
  int64_t execution_id_;
};

enum ColChecksumStat
{
  CCS_INVALID = 0,
  CCS_NOT_MASTER,
  CCS_SUCCEED,
  CCS_FAILED,
};

struct PartitionColChecksumStat
{
  PartitionColChecksumStat()
    : tablet_id_(),
      col_checksum_stat_(CCS_INVALID),
      snapshot_(-1),
      execution_id_(common::OB_INVALID_ID),
      ret_code_(OB_SUCCESS)
  {}
  void reset() {
    tablet_id_.reset();
    col_checksum_stat_ = CCS_INVALID;
    snapshot_ = -1;
    execution_id_ = common::OB_INVALID_ID;
    ret_code_ = common::OB_SUCCESS;
    table_id_ = common::OB_INVALID_ID;
  }
  bool is_valid() const { return tablet_id_.is_valid() && common::OB_INVALID_ID != execution_id_ && common::OB_INVALID_ID != table_id_; }
  TO_STRING_KV(K_(tablet_id),
               K_(col_checksum_stat),
               K_(snapshot),
               K_(execution_id),
               K_(table_id));
  ObTabletID tablet_id_; // may be data table, local index or global index
  ColChecksumStat col_checksum_stat_;
  int64_t snapshot_;
  uint64_t execution_id_;
  int ret_code_;
  int64_t table_id_;
};

class ObDDLWaitColumnChecksumCtx final
{
public:
  ObDDLWaitColumnChecksumCtx();
  ~ObDDLWaitColumnChecksumCtx();
  int init(
      const int64_t task_id,
      const uint64_t tenant_id,
      const uint64_t source_table_id,
      const uint64_t target_table_id,
      const int64_t schema_version,
      const int64_t snapshot_version,
      const uint64_t execution_id,
      const int64_t timeout_us);
  void reset();
  bool is_inited() const { return is_inited_; }
  int try_wait(bool &is_column_checksum_ready);
  int update_status(const common::ObTabletID &tablet_id, const int ret_code);
  TO_STRING_KV(K(is_inited_), K(is_calc_done_), K(source_table_id_), K(target_table_id_),
      K(schema_version_), K(snapshot_version_), K(execution_id_), K(timeout_us_),
      K(last_drive_ts_), K(stat_array_), K_(tenant_id));

private:
  int send_calc_rpc(int64_t &send_succ_count);
  int refresh_zombie_task();

private:
  bool is_inited_;
  bool is_calc_done_;
  uint64_t source_table_id_;
  uint64_t target_table_id_;
  int64_t schema_version_;
  int64_t snapshot_version_;
  int64_t execution_id_;
  int64_t timeout_us_;
  int64_t last_drive_ts_;
  common::ObArray<PartitionColChecksumStat> stat_array_;
  int64_t task_id_;
  uint64_t tenant_id_;
  common::SpinRWLock lock_;
};

} // end namespace rootserver
} // end namespace oceanbase


#endif//OCEANBASE_ROOTSERVER_OB_DDL_TASK_H_
