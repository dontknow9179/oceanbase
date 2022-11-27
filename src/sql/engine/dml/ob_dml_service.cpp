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

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ob_dml_service.h"
#include "sql/das/ob_data_access_service.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/das/ob_das_insert_op.h"
#include "sql/das/ob_das_delete_op.h"
#include "sql/das/ob_das_update_op.h"
#include "sql/das/ob_das_lock_op.h"
#include "sql/das/ob_das_def_reg.h"
#include "sql/das/ob_das_utils.h"
#include "sql/engine/dml/ob_trigger_handler.h"
#include "lib/utility/ob_tracepoint.h"
#include "sql/engine/dml/ob_err_log_service.h"
#include "share/ob_tablet_autoincrement_service.h"
#include "storage/tx/ob_trans_service.h"
#include "pl/ob_pl.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace transaction;
namespace sql
{
int ObDMLService::check_row_null(const ObExprPtrIArray &row,
                                 ObEvalCtx &eval_ctx,
                                 int64_t row_num,
                                 const ColContentIArray &column_infos,
                                 bool is_ignore,
                                 ObTableModifyOp &dml_op)
{
  int ret = OB_SUCCESS;
  CK(row.count() >= column_infos.count());
  for (int i = 0; OB_SUCC(ret) && i < column_infos.count(); i++) {
    ObDatum *datum = NULL;
    const bool is_nullable = column_infos.at(i).is_nullable_;
    uint64_t col_idx = column_infos.at(i).projector_index_;
    if (OB_FAIL(row.at(col_idx)->eval(eval_ctx, datum))) {
      dml_op.log_user_error_inner(ret, row_num, column_infos.at(i));
    } else if (!is_nullable && datum->is_null()) {
      if (is_ignore) {
        ObObj zero_obj;
        if (is_oracle_mode()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("dml with ignore not supported in oracle mode");
        } else if (OB_FAIL(ObObjCaster::get_zero_value(
            row.at(col_idx)->obj_meta_.get_type(),
            row.at(col_idx)->obj_meta_.get_collation_type(),
            zero_obj))) {
          LOG_WARN("get column default zero value failed", K(ret), K(column_infos.at(i)));
        } else if (OB_FAIL(row.at(col_idx)->locate_datum_for_write(eval_ctx).from_obj(zero_obj))) {
          LOG_WARN("assign zero obj to datum failed", K(ret), K(zero_obj));
        } else {
          //output warning msg
          const ObString &column_name = column_infos.at(i).column_name_;
          LOG_USER_WARN(OB_BAD_NULL_ERROR, column_name.length(), column_name.ptr());
        }
      } else {
        //output warning msg
        const ObString &column_name = column_infos.at(i).column_name_;
        ret = OB_BAD_NULL_ERROR;
        LOG_USER_ERROR(OB_BAD_NULL_ERROR, column_name.length(), column_name.ptr());
      }
    }
  }
  return ret;
}

int ObDMLService::check_column_type(const ExprFixedArray &dml_row,
                                    int64_t row_num,
                                    const ObIArray<ColumnContent> &column_infos,
                                    ObTableModifyOp &dml_op)
{
  int ret = OB_SUCCESS;
  CK(dml_row.count() >= column_infos.count());
  for (int64_t i = 0; OB_SUCC(ret) && i < column_infos.count(); ++i) {
    const ColumnContent &column_info = column_infos.at(i);
    ObExpr *expr = dml_row.at(column_info.projector_index_);
    ObDatum *datum = nullptr;
    if (OB_FAIL(expr->eval(dml_op.get_eval_ctx(), datum))) {
      dml_op.log_user_error_inner(ret, row_num, column_info);
    }
  }
  return ret;
}

int ObDMLService::check_rowkey_is_null(const ObExprPtrIArray &row,
                                       int64_t rowkey_cnt,
                                       ObEvalCtx &eval_ctx,
                                       bool &is_null)
{
  int ret = OB_SUCCESS;
  is_null = false;
  CK(row.count() >= rowkey_cnt);
  ObDatum *datum = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && !is_null && i < rowkey_cnt; ++i) {
    if (OB_FAIL(row.at(i)->eval(eval_ctx, datum))) {
      LOG_WARN("eval expr failed", K(ret), K(i));
    } else {
      is_null = datum->is_null();
    }
  }
  return ret;
}

int ObDMLService::check_rowkey_whether_distinct(const ObExprPtrIArray &row,
                                                int64_t rowkey_cnt,
                                                int64_t estimate_row,
                                                DistinctType distinct_algo,
                                                ObEvalCtx &eval_ctx,
                                                ObExecContext &root_ctx,
                                                SeRowkeyDistCtx *rowkey_dist_ctx,
                                                bool &is_dist)
{
  int ret = OB_SUCCESS;
  is_dist = true;
  if (T_DISTINCT_NONE != distinct_algo) {
    if (T_HASH_DISTINCT == distinct_algo) {
      ObIAllocator &allocator = root_ctx.get_allocator();
      if (OB_ISNULL(rowkey_dist_ctx)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("distinct check hash set is null", K(ret));
      } else {
        SeRowkeyItem rowkey_item;
        if (OB_ISNULL(rowkey_dist_ctx)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("rowkey_dist_ctx cannot be NULL", K(ret));
        } else if (OB_FAIL(rowkey_item.init(row, eval_ctx, allocator,
                                            rowkey_cnt))) {
          LOG_WARN("init rowkey item failed", K(ret));
        } else {
          ret = rowkey_dist_ctx->exist_refactored(rowkey_item);
          if (OB_HASH_EXIST == ret) {
            ret = OB_SUCCESS;
            is_dist = false;
          } else if (OB_HASH_NOT_EXIST == ret) {
            if (OB_FAIL(rowkey_item.copy_datum_data(allocator))) {
              LOG_WARN("deep_copy rowkey item failed", K(ret));
            } else if (OB_FAIL(rowkey_dist_ctx->set_refactored(rowkey_item))) {
              LOG_WARN("set rowkey item failed", K(ret));
            }
          } else {
            LOG_WARN("check if rowkey item exists failed", K(ret));
          }
        }
      }
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "Check Update/Delete row with merge distinct");
    }
  }
  return ret;
}

int ObDMLService::create_rowkey_check_hashset(int64_t estimate_row,
                                           ObExecContext *root_ctx,
                                           SeRowkeyDistCtx *&rowkey_dist_ctx)
{
  int ret = OB_SUCCESS;
  ObIAllocator &allocator = root_ctx->get_allocator();
  if (OB_ISNULL(rowkey_dist_ctx)) {
    //create rowkey distinct context
    void *buf = allocator.alloc(sizeof(SeRowkeyDistCtx));
    ObSQLSessionInfo *my_session = root_ctx->get_my_session();
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("allocate memory failed", K(ret), "size", sizeof(SeRowkeyDistCtx));
    } else if (OB_ISNULL(my_session)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("my session is null", K(ret));
    } else {
      rowkey_dist_ctx = new (buf) SeRowkeyDistCtx();
      int64_t match_rows = estimate_row > ObDMLBaseCtDef::MIN_ROWKEY_DISTINCT_BUCKET_NUM ?
                            estimate_row : ObDMLBaseCtDef::MIN_ROWKEY_DISTINCT_BUCKET_NUM;
      // https://work.aone.alibaba-inc.com/issue/23348769
      // match_rows是优化器估行的结果，如果这个值很大，
      // 直接创建有这么多bucket的hashmap会申请
      // 不到内存，这里做了限制为64k，防止报内存不足的错误
      const int64_t max_bucket_num = match_rows > ObDMLBaseCtDef::MAX_ROWKEY_DISTINCT_BUCKET_NUM ?
                              ObDMLBaseCtDef::MAX_ROWKEY_DISTINCT_BUCKET_NUM : match_rows;
      if (OB_FAIL(rowkey_dist_ctx->create(max_bucket_num,
                                      ObModIds::OB_DML_CHECK_ROWKEY_DISTINCT_BUCKET,
                                      ObModIds::OB_DML_CHECK_ROWKEY_DISTINCT_NODE,
                                      my_session->get_effective_tenant_id()))) {
        LOG_WARN("create rowkey distinct context failed", K(ret), "rows", estimate_row);
      }
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Create hash set on a pointer that is not null", K(ret));
  }
  return ret;
}

int ObDMLService::check_row_whether_changed(const ObUpdCtDef &upd_ctdef,
                                            ObUpdRtDef &upd_rtdef,
                                            ObEvalCtx &eval_ctx)
{
  int ret = OB_SUCCESS;
  NG_TRACE_TIMES(2, update_start_check_row);
  if (!upd_ctdef.is_primary_index_) {
    // if in pdml, upd_rtdef.primary_rtdef_ is null
    // in normal dml stmt, upd_rtdef.primary_rtdef_ is not null
    upd_rtdef.is_row_changed_ = false;
    if (OB_NOT_NULL(upd_rtdef.primary_rtdef_)) {
      upd_rtdef.is_row_changed_ = upd_rtdef.primary_rtdef_->is_row_changed_;
    } else if (lib::is_mysql_mode()) {
      // whether the global index row is updated is subject to the result of the primary index
      // pdml e.g.:
      // create table t22(a int primary key,
      //                 b int,
      //                 c int,
      //                 d timestamp DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
      // ) partition by hash(a) partitions 5;
      // create index idx on t22(c, d) global;
      // insert into t22 values(1, 1, 1, now());
      // set _force_parallel_dml_dop = 3;
      // update t22 set b=2, c=1 where a=1;
      // this case, we must update data_table and index_table
      const ObExprPtrIArray &old_row = upd_ctdef.old_row_;
      const ObExprPtrIArray &new_row = upd_ctdef.new_row_;
      FOREACH_CNT_X(info, upd_ctdef.assign_columns_, OB_SUCC(ret) && !upd_rtdef.is_row_changed_) {
        const uint64_t idx = info->projector_index_;
        ObDatum *old_datum = NULL;
        ObDatum *new_datum = NULL;
        if (OB_FAIL(old_row.at(idx)->eval(eval_ctx, old_datum))
            || OB_FAIL(new_row.at(idx)->eval(eval_ctx, new_datum))) {
          LOG_WARN("evaluate value failed", K(ret));
        } else {
          upd_rtdef.is_row_changed_ = !ObDatum::binary_equal(*old_datum, *new_datum);
        }
      }
    } else {
      //in oracle mode, no matter whether the updated row is changed or not,
      //the row will be updated in the storage
      upd_rtdef.is_row_changed_ = true;
    }
  } else if (lib::is_mysql_mode()) {
    upd_rtdef.is_row_changed_ = false;
    const ObExprPtrIArray &old_row = upd_ctdef.old_row_;
    const ObExprPtrIArray &new_row = upd_ctdef.new_row_;
    FOREACH_CNT_X(info, upd_ctdef.assign_columns_, OB_SUCC(ret) && !upd_rtdef.is_row_changed_) {
      const uint64_t idx = info->projector_index_;
      if (OB_LIKELY(!info->auto_filled_timestamp_)) {
        ObDatum *old_datum = NULL;
        ObDatum *new_datum = NULL;
        if (OB_FAIL(old_row.at(idx)->eval(eval_ctx, old_datum))
            || OB_FAIL(new_row.at(idx)->eval(eval_ctx, new_datum))) {
          LOG_WARN("evaluate value failed", K(ret));
        } else {
          upd_rtdef.is_row_changed_ = !ObDatum::binary_equal(*old_datum, *new_datum);
        }
      }
    }
  } else {
    //in oracle mode, no matter whether the updated row is changed or not,
    //the row will be updated in the storage
    upd_rtdef.is_row_changed_ = true;
  }
  if (OB_SUCC(ret) &&
      upd_rtdef.is_row_changed_ &&
      upd_ctdef.is_primary_index_ &&
      upd_ctdef.dupd_ctdef_.is_batch_stmt_) {
    //check predicate column whether changed in batch stmt execution
    const ObExprPtrIArray &old_row = upd_ctdef.old_row_;
    const ObExprPtrIArray &new_row = upd_ctdef.new_row_;
    for (int64_t i = 0; OB_SUCC(ret) && i < upd_ctdef.assign_columns_.count(); ++i) {
      const ColumnContent &info = upd_ctdef.assign_columns_.at(i);
      uint64_t idx = info.projector_index_;
      ObDatum *old_datum = NULL;
      ObDatum *new_datum = NULL;
      if (info.is_predicate_column_) {
        if (OB_FAIL(old_row.at(idx)->eval(eval_ctx, old_datum))
            || OB_FAIL(new_row.at(idx)->eval(eval_ctx, new_datum))) {
          LOG_WARN("evaluate value failed", K(ret));
        } else if (!ObDatum::binary_equal(*old_datum, *new_datum)) {
          //update the predicate column, will lead to the next stmt result change, need rollback
          ret = OB_BATCHED_MULTI_STMT_ROLLBACK;
          LOG_TRACE("batch stmt update the predicate column, need to rollback", K(ret),
                    K(info), KPC(old_datum), KPC(new_datum));
        }
      }
    }
  }
  LOG_DEBUG("after check update row changed", K(ret), K(upd_rtdef.is_row_changed_),
              "old_row", ROWEXPR2STR(eval_ctx, upd_ctdef.old_row_),
              "new_row", ROWEXPR2STR(eval_ctx, upd_ctdef.new_row_));
  NG_TRACE_TIMES(2, update_end_check_row);
  return ret;
}

int ObDMLService::filter_row_for_check_cst(const ExprFixedArray &cst_exprs,
                                           ObEvalCtx &eval_ctx,
                                           bool &filtered)
{
  int ret = OB_SUCCESS;
  filtered = false;
  for (int64_t i = 0; OB_SUCC(ret) && !filtered && i < cst_exprs.count(); ++i) {
    ObExpr *expr = cst_exprs.at(i);
    ObDatum *datum = nullptr;
    if (OB_FAIL(expr->eval(eval_ctx, datum))) {
      if (is_mysql_mode()) {
        LOG_INFO("cover original errno while calulating check expr in mysql mode", K(ret));
        filtered = true;
        ret = OB_SUCCESS;
      } else { // oracle mode
        LOG_WARN("eval check constraint expr failed", K(ret));
      }
    } else {
      OB_ASSERT(ob_is_int_tc(expr->datum_meta_.type_));
      if (!datum->is_null() && 0 == datum->get_int()) {
        filtered = true;
      }
    }
  }
  return ret;
}

int ObDMLService::filter_row_for_view_check(const ExprFixedArray &cst_exprs,
                                           ObEvalCtx &eval_ctx,
                                           bool &filtered)
{
  int ret = OB_SUCCESS;
  filtered = false;
  for (int64_t i = 0; OB_SUCC(ret) && !filtered && i < cst_exprs.count(); ++i) {
    ObExpr *expr = cst_exprs.at(i);
    ObDatum *datum = nullptr;
    if (OB_FAIL(expr->eval(eval_ctx, datum))) {
      LOG_WARN("eval check constraint expr failed", K(ret));
    } else {
      OB_ASSERT(ob_is_int_tc(expr->datum_meta_.type_));
      if (datum->is_null() || 0 == datum->get_int()) {
        filtered = true;
      }
    }
  }
  return ret;
}

int ObDMLService::process_before_stmt_trigger(const ObDMLBaseCtDef &dml_ctdef,
                                              ObDMLBaseRtDef &dml_rtdef,
                                              ObDMLRtCtx &dml_rtctx,
                                              const ObDmlEventType &dml_event)
{
  int ret = OB_SUCCESS;
  dml_rtctx.get_exec_ctx().set_dml_event(dml_event);
  if (dml_ctdef.is_primary_index_ && !dml_ctdef.trig_ctdef_.tg_args_.empty()) {
    if (!dml_rtctx.op_.get_spec().use_dist_das()
        || dml_rtctx.get_exec_ctx().get_my_session()->is_remote_session()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "Do before stmt trigger without DAS");
      LOG_WARN("Do before stmt trigger without DAS not supported", K(ret),
               K(dml_rtctx.op_.get_spec().use_dist_das()),
               K(dml_rtctx.get_exec_ctx().get_my_session()->is_remote_session()));
    } else if (OB_FAIL(TriggerHandle::do_handle_before_stmt(dml_rtctx.op_,
                                                            dml_ctdef.trig_ctdef_,
                                                            dml_rtdef.trig_rtdef_,
                                                            dml_event))) {
      LOG_WARN("failed to handle before stmt trigger", K(ret));
    } else if (OB_FAIL(ObSqlTransControl::stmt_refresh_snapshot(dml_rtctx.get_exec_ctx()))) {
      LOG_WARN("failed to get new snapshot after before stmt trigger evaluated", K(ret));
    }
  }
  return ret;
}

int ObDMLService::process_after_stmt_trigger(const ObDMLBaseCtDef &dml_ctdef,
                                             ObDMLBaseRtDef &dml_rtdef,
                                             ObDMLRtCtx &dml_rtctx,
                                             const ObDmlEventType &dml_event)
{
  int ret = OB_SUCCESS;
  dml_rtctx.get_exec_ctx().set_dml_event(dml_event);
  if (dml_ctdef.is_primary_index_ && !dml_ctdef.trig_ctdef_.tg_args_.empty()) {
    if (!dml_rtctx.op_.get_spec().use_dist_das()
        || dml_rtctx.get_exec_ctx().get_my_session()->is_remote_session()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "Do after stmt trigger without DAS");
      LOG_WARN("Do after stmt trigger without DAS not supported", K(ret),
               K(dml_rtctx.op_.get_spec().use_dist_das()),
               K(dml_rtctx.get_exec_ctx().get_my_session()->is_remote_session()));
    } else if (OB_FAIL(TriggerHandle::do_handle_after_stmt(dml_rtctx.op_,
                                                           dml_ctdef.trig_ctdef_,
                                                           dml_rtdef.trig_rtdef_,
                                                           dml_event))) {
      LOG_WARN("failed to handle after stmt trigger", K(ret));
    }
  }
  return ret;
}

int ObDMLService::init_heap_table_pk_for_ins(const ObInsCtDef &ins_ctdef, ObEvalCtx &eval_ctx)
{
  int ret = OB_SUCCESS;
  if (ins_ctdef.is_primary_index_ && ins_ctdef.is_heap_table_ && !ins_ctdef.has_instead_of_trigger_) {
    ObExpr *auto_inc_expr = ins_ctdef.new_row_.at(0);
    if (OB_ISNULL(auto_inc_expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("expr is null", K(ret));
    } else if (auto_inc_expr->type_ != T_TABLET_AUTOINC_NEXTVAL) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("auto_inc_expr type unexpected", K(ret), KPC(auto_inc_expr));
    } else {
      ObDatum &datum = auto_inc_expr->locate_datum_for_write(eval_ctx);
      datum.set_null();
      auto_inc_expr->get_eval_info(eval_ctx).evaluated_ = true;
    }
  }
  return ret;
}

int ObDMLService::process_insert_row(const ObInsCtDef &ins_ctdef,
                                     ObInsRtDef &ins_rtdef,
                                     ObTableModifyOp &dml_op,
                                     bool &is_skipped)
{
  int ret = OB_SUCCESS;
  if (ins_ctdef.is_primary_index_) {
    bool is_filtered = false;
    is_skipped = false;
    ObEvalCtx &eval_ctx = dml_op.get_eval_ctx();
    uint64_t ref_table_id = ins_ctdef.das_base_ctdef_.index_tid_;
    ObSQLSessionInfo *my_session = NULL;
    bool has_instead_of_trg = ins_ctdef.has_instead_of_trigger_;
    //first, check insert value whether matched column type
    if (OB_FAIL(check_column_type(ins_ctdef.new_row_,
                                  ins_rtdef.cur_row_num_,
                                  ins_ctdef.column_infos_,
                                  dml_op))) {
      LOG_WARN("check column type failed", K(ret));
    } else if (OB_ISNULL(my_session = dml_op.get_exec_ctx().get_my_session())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("session is NULL", K(ret));
    } else if (OB_FAIL(check_nested_sql_legality(dml_op.get_exec_ctx(), ins_ctdef.das_ctdef_.index_tid_))) {
      LOG_WARN("failed to check stmt table", K(ret), K(ref_table_id));
    } else if (OB_FAIL(TriggerHandle::init_param_new_row(
        eval_ctx, ins_ctdef.trig_ctdef_, ins_rtdef.trig_rtdef_))) {
      LOG_WARN("failed to handle before trigger", K(ret));
    } else if (OB_FAIL(TriggerHandle::do_handle_before_row(
        dml_op, ins_ctdef.das_base_ctdef_, ins_ctdef.trig_ctdef_, ins_rtdef.trig_rtdef_))) {
      LOG_WARN("failed to handle before trigger", K(ret));
    } 
    if (OB_FAIL(ret)) {
    } else if (has_instead_of_trg) {
      is_skipped = true;
    } else if (OB_FAIL(check_row_null(ins_ctdef.new_row_,
                                      dml_op.get_eval_ctx(),
                                      ins_rtdef.cur_row_num_,
                                      ins_ctdef.column_infos_,
                                      ins_ctdef.das_ctdef_.is_ignore_,
                                      dml_op))) {
      LOG_WARN("check row null failed", K(ret));
    } else if (OB_FAIL(ForeignKeyHandle::do_handle(dml_op, ins_ctdef, ins_rtdef))) {
      LOG_WARN("do handle new row with foreign key failed", K(ret));
    } else if (OB_FAIL(filter_row_for_view_check(ins_ctdef.view_check_exprs_,
                                                 eval_ctx, is_filtered))) {
      //check column constraint expr
      LOG_WARN("filter row for check cst failed", K(ret));
    } else if (OB_UNLIKELY(is_filtered)) {
      ret = OB_ERR_CHECK_OPTION_VIOLATED;
      LOG_WARN("view check option violated", K(ret));
    } else if (OB_FAIL(filter_row_for_check_cst(ins_ctdef.check_cst_exprs_,
                                                eval_ctx, is_filtered))) {
      //check column constraint expr
      LOG_WARN("filter row for check cst failed", K(ret));
    } else if (OB_UNLIKELY(is_filtered)) {
      if (is_mysql_mode() && ins_ctdef.das_ctdef_.is_ignore_) {
        is_skipped = true;
        LOG_USER_WARN(OB_ERR_CHECK_CONSTRAINT_VIOLATED);
        LOG_WARN("check constraint violated, skip this row", K(ret));
      } else {
        ret = OB_ERR_CHECK_CONSTRAINT_VIOLATED;
        LOG_WARN("column constraint check failed", K(ret));
      }
    }

    if (OB_FAIL(ret) && dml_op.is_error_logging_ && should_catch_err(ret) && !has_instead_of_trg) {
      dml_op.err_log_rt_def_.first_err_ret_ = ret;
      // cover the err_ret  by design
      ret = OB_SUCCESS;
      for (int64_t i = 0; OB_SUCC(ret) && i < ins_ctdef.new_row_.count(); ++i) {
        ObExpr *expr = ins_ctdef.new_row_.at(i);
        ObDatum *datum = nullptr;
        if (OB_FAIL(expr->eval(dml_op.get_eval_ctx(), datum))) {
          if (should_catch_err(ret) && !(IS_CONST_TYPE(expr->type_))) {
            expr->locate_datum_for_write(dml_op.get_eval_ctx()).set_null();
            expr->set_evaluated_flag(dml_op.get_eval_ctx());
            ret = OB_SUCCESS;
          }
        }
      }
    }
  }
  ret = (ret == OB_SUCCESS ? dml_op.err_log_rt_def_.first_err_ret_ : ret);
  // If any error occurred before, the error code here is not OB_SUCCESS;
  return ret;
}

int ObDMLService::process_lock_row(const ObLockCtDef &lock_ctdef,
                                   ObLockRtDef &lock_rtdef,
                                   bool &is_skipped,
                                   ObTableModifyOp &dml_op)
{
  int ret = OB_SUCCESS;
  is_skipped = false;
  bool is_null = false;
  UNUSED(dml_op);
  if (lock_ctdef.need_check_filter_null_ &&
      OB_FAIL(check_rowkey_is_null(lock_ctdef.old_row_,
                                   lock_ctdef.das_ctdef_.rowkey_cnt_,
                                   dml_op.get_eval_ctx(),
                                   is_null))) {
    LOG_WARN("failed to check rowkey is null", K(ret));
  } else if (is_null) {
    // no need to lock
    is_skipped = true;
  }
  return ret;
}

int ObDMLService::process_delete_row(const ObDelCtDef &del_ctdef,
                                     ObDelRtDef &del_rtdef,
                                     bool &is_skipped,
                                     ObTableModifyOp &dml_op)
{
  int ret = OB_SUCCESS;
  is_skipped = false;
  if (del_ctdef.is_primary_index_) {
    uint64_t ref_table_id = del_ctdef.das_base_ctdef_.index_tid_;
    ObSQLSessionInfo *my_session = NULL;
    bool has_instead_of_trg = del_ctdef.has_instead_of_trigger_;
    if (OB_ISNULL(my_session = dml_op.get_exec_ctx().get_my_session())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("session is NULL", K(ret));
    } else if (OB_FAIL(check_nested_sql_legality(dml_op.get_exec_ctx(), del_ctdef.das_ctdef_.index_tid_))) {
      LOG_WARN("failed to check stmt table", K(ret), K(ref_table_id));
    }
    if (OB_SUCC(ret) && del_ctdef.need_check_filter_null_ && !has_instead_of_trg) {
      bool is_null = false;
      if (OB_FAIL(check_rowkey_is_null(del_ctdef.old_row_,
                                       del_ctdef.das_ctdef_.rowkey_cnt_,
                                       dml_op.get_eval_ctx(),
                                       is_null))) {
        LOG_WARN("check rowkey is null failed", K(ret), K(del_ctdef), K(del_rtdef));
      } else if (is_null) {
        is_skipped = true;
      }
    }

    if (OB_SUCC(ret) && !is_skipped && !OB_ISNULL(del_rtdef.se_rowkey_dist_ctx_) && !has_instead_of_trg) {
      bool is_distinct = false;
      ObExecContext *root_ctx = nullptr;
      if (OB_FAIL(dml_op.get_exec_ctx().get_root_ctx(root_ctx))) {
        LOG_WARN("get root ExecContext failed", K(ret));
      } else if (OB_ISNULL(root_ctx)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the root ctx of foreign key nested session is null", K(ret));
      } else if (OB_FAIL(check_rowkey_whether_distinct(del_ctdef.distinct_key_,
                                                      del_ctdef.distinct_key_.count(),
                                                      dml_op.get_spec().rows_,
                                                      T_HASH_DISTINCT,
                                                      dml_op.get_eval_ctx(),
                                                      *root_ctx,
                                                      del_rtdef.se_rowkey_dist_ctx_,
                                                      is_distinct))) {
        LOG_WARN("check rowkey whether distinct failed", K(ret),
                  K(del_ctdef), K(del_rtdef), K(dml_op.get_spec().rows_));
      } else if (!is_distinct) {
        is_skipped = true;
      }
    }

    if (OB_SUCC(ret) && !is_skipped) {
      if (!has_instead_of_trg && OB_FAIL(ForeignKeyHandle::do_handle(dml_op, del_ctdef, del_rtdef))) {
        LOG_WARN("do handle old row for delete op failed", K(ret), K(del_ctdef), K(del_rtdef));
      } else if (OB_FAIL(TriggerHandle::init_param_old_row(
        dml_op.get_eval_ctx(), del_ctdef.trig_ctdef_, del_rtdef.trig_rtdef_))) {
        LOG_WARN("failed to handle before trigger", K(ret));
      } else if (OB_FAIL(TriggerHandle::do_handle_before_row(
          dml_op, del_ctdef.das_base_ctdef_, del_ctdef.trig_ctdef_, del_rtdef.trig_rtdef_))) {
        LOG_WARN("failed to handle before trigger", K(ret));
      } else if (OB_SUCC(ret) && OB_FAIL(TriggerHandle::do_handle_after_row(
          dml_op, del_ctdef.trig_ctdef_, del_rtdef.trig_rtdef_, ObTriggerEvents::get_delete_event()))) {
        LOG_WARN("failed to handle before trigger", K(ret));
      } else if (has_instead_of_trg) {
        is_skipped = true;
      }
    }
    // here only catch foreign key execption
    if (OB_FAIL(ret) && dml_op.is_error_logging_ && should_catch_err(ret) && !has_instead_of_trg) {
      dml_op.err_log_rt_def_.first_err_ret_ = ret;
    }

    LOG_DEBUG("process delete row", K(ret), K(is_skipped),
               "old_row", ROWEXPR2STR(dml_op.get_eval_ctx(), del_ctdef.old_row_));
  }

  return ret;
}

int ObDMLService::process_update_row(const ObUpdCtDef &upd_ctdef,
                                     ObUpdRtDef &upd_rtdef,
                                     bool &is_skipped,
                                     ObTableModifyOp &dml_op)
{
  int ret = OB_SUCCESS;
  is_skipped = false;
  bool has_instead_of_trg = upd_ctdef.has_instead_of_trigger_;
  if (upd_ctdef.is_primary_index_) {
    uint64_t ref_table_id = upd_ctdef.das_base_ctdef_.index_tid_;
    ObSQLSessionInfo *my_session = NULL;
    if (upd_ctdef.is_heap_table_ &&
        OB_FAIL(copy_heap_table_hidden_pk(dml_op.get_eval_ctx(), upd_ctdef))) {
      LOG_WARN("fail to copy heap table hidden pk", K(ret), K(upd_ctdef));
    }

    if (OB_SUCC(ret) && upd_ctdef.need_check_filter_null_ && !has_instead_of_trg) {
      bool is_null = false;
      if (OB_FAIL(check_rowkey_is_null(upd_ctdef.old_row_,
                                       upd_ctdef.dupd_ctdef_.rowkey_cnt_,
                                       dml_op.get_eval_ctx(),
                                       is_null))) {
        LOG_WARN("check rowkey is null failed", K(ret), K(upd_ctdef), K(upd_rtdef));
      } else if (is_null) {
        is_skipped = true;
      }
    }
    if (OB_SUCC(ret) && !is_skipped && !has_instead_of_trg) {
      bool is_distinct = false;
      if (OB_FAIL(check_rowkey_whether_distinct(upd_ctdef.distinct_key_,
                                                upd_ctdef.distinct_key_.count(),
                                                dml_op.get_spec().rows_,
                                                upd_ctdef.distinct_algo_,
                                                dml_op.get_eval_ctx(),
                                                dml_op.get_exec_ctx(),
                                                upd_rtdef.se_rowkey_dist_ctx_,
                                                is_distinct))) {
        LOG_WARN("check rowkey whether distinct failed", K(ret),
                 K(upd_ctdef), K(upd_rtdef), K(dml_op.get_spec().rows_));
      } else if (!is_distinct) {
        is_skipped = true;
      }
    }
    if (OB_SUCC(ret) && !is_skipped) {
      bool is_filtered = false;
      //first, check assignment column whether matched column type
      if (OB_FAIL(check_column_type(upd_ctdef.new_row_,
                                    upd_rtdef.cur_row_num_,
                                    upd_ctdef.assign_columns_,
                                    dml_op))) {
        LOG_WARN("check column type failed", K(ret));
      } else if (OB_ISNULL(my_session = dml_op.get_exec_ctx().get_my_session())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("session is NULL", K(ret));
      } else if (OB_FAIL(check_nested_sql_legality(dml_op.get_exec_ctx(), upd_ctdef.dupd_ctdef_.index_tid_))) {
        LOG_WARN("failed to check stmt table", K(ret), K(ref_table_id));
      } else if (OB_FAIL(TriggerHandle::init_param_rows(
          dml_op.get_eval_ctx(), upd_ctdef.trig_ctdef_, upd_rtdef.trig_rtdef_))) {
        LOG_WARN("failed to handle before trigger", K(ret));
      } else if (OB_FAIL(TriggerHandle::do_handle_before_row(
          dml_op, upd_ctdef.das_base_ctdef_, upd_ctdef.trig_ctdef_, upd_rtdef.trig_rtdef_))) {
        LOG_WARN("failed to handle before trigger", K(ret));
      }
      if (OB_FAIL(ret)) {
      } else if (has_instead_of_trg) {
        is_skipped = true;
      } else if (OB_FAIL(check_row_null(upd_ctdef.new_row_,
                                        dml_op.get_eval_ctx(),
                                        upd_rtdef.cur_row_num_,
                                        upd_ctdef.assign_columns_,
                                        upd_ctdef.dupd_ctdef_.is_ignore_,
                                        dml_op))) {
        LOG_WARN("check row null failed", K(ret), K(upd_ctdef), K(upd_rtdef));
      } else if (OB_FAIL(check_row_whether_changed(upd_ctdef, upd_rtdef, dml_op.get_eval_ctx()))) {
        LOG_WARN("check row whether changed failed", K(ret), K(upd_ctdef), K(upd_rtdef));
      } else if (OB_UNLIKELY(!upd_rtdef.is_row_changed_)) {
        //do nothing
      } else if (OB_FAIL(ForeignKeyHandle::do_handle(dml_op, upd_ctdef, upd_rtdef))) {
        LOG_WARN("do handle row for update op failed", K(ret), K(upd_ctdef), K(upd_rtdef));
      } else if (OB_FAIL(filter_row_for_view_check(upd_ctdef.view_check_exprs_, dml_op.get_eval_ctx(), is_filtered))) {
        LOG_WARN("filter row for view check exprs failed", K(ret));
      } else if (OB_UNLIKELY(is_filtered)) {
        ret = OB_ERR_CHECK_OPTION_VIOLATED;
        LOG_WARN("view check option violated", K(ret));
      } else if (OB_FAIL(filter_row_for_check_cst(upd_ctdef.check_cst_exprs_, dml_op.get_eval_ctx(), is_filtered))) {
        LOG_WARN("filter row for check cst failed", K(ret));
      } else if (OB_UNLIKELY(is_filtered)) {
        if (is_mysql_mode() && upd_ctdef.dupd_ctdef_.is_ignore_) {
          is_skipped = true;
          LOG_USER_WARN(OB_ERR_CHECK_CONSTRAINT_VIOLATED);
          LOG_WARN("check constraint violated, skip this row", K(ret));
        } else {
          ret = OB_ERR_CHECK_CONSTRAINT_VIOLATED;
          LOG_WARN("row is filtered by check filters, running is stopped", K(ret));
        }
      }
    }
  } else {
    //for global index, only check whether the updated row is changed
    if (OB_FAIL(check_row_whether_changed(upd_ctdef, upd_rtdef, dml_op.get_eval_ctx()))) {
      LOG_WARN("check row whether changed failed", K(ret), K(upd_ctdef), K(upd_rtdef));
    }
  }

  if (OB_FAIL(ret) && dml_op.is_error_logging_ && should_catch_err(ret) && !has_instead_of_trg) {
    dml_op.err_log_rt_def_.first_err_ret_ = ret;
    // cover the err_ret  by design
    ret = OB_SUCCESS;
    //todo @kaizhan.dkz expr.set_null() must skip const expr
    for (int64_t i = 0; OB_SUCC(ret) && i < upd_ctdef.full_row_.count(); ++i) {
      ObExpr *expr = upd_ctdef.full_row_.at(i);
      ObDatum *datum = nullptr;
      if (OB_FAIL(expr->eval(dml_op.get_eval_ctx(), datum))) {
        if (should_catch_err(ret) && !(IS_CONST_TYPE(expr->type_))) {
          expr->locate_datum_for_write(dml_op.get_eval_ctx()).set_null();
          expr->set_evaluated_flag(dml_op.get_eval_ctx());
          ret = OB_SUCCESS;
        }
      }
    }
  }
  ret = (ret == OB_SUCCESS ? dml_op.err_log_rt_def_.first_err_ret_ : ret);
  LOG_DEBUG("process update row", K(ret), K(is_skipped), K(upd_ctdef), K(upd_rtdef),
            "old_row", ROWEXPR2STR(dml_op.get_eval_ctx(), upd_ctdef.old_row_),
            "new_row", ROWEXPR2STR(dml_op.get_eval_ctx(), upd_ctdef.new_row_));
  return ret;
}

int ObDMLService::insert_row(const ObInsCtDef &ins_ctdef,
                             ObInsRtDef &ins_rtdef,
                             const ObDASTabletLoc *tablet_loc,
                             ObDMLRtCtx &dml_rtctx)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_dml_tablet_validity(dml_rtctx,
                                        *tablet_loc,
                                        ins_ctdef.new_row_,
                                        ins_ctdef,
                                        ins_rtdef))) {
    LOG_WARN("check insert row tablet validity failed", K(ret));
  } else if (OB_FAIL(insert_row(ins_ctdef.das_ctdef_,
                                ins_rtdef.das_rtdef_,
                                tablet_loc,
                                dml_rtctx,
                                ins_ctdef.new_row_))) {
    LOG_WARN("insert row to das failed", K(ret));
  }
  return ret;
}

int ObDMLService::insert_row(const ObDASInsCtDef &ins_ctdef,
                             ObDASInsRtDef &ins_rtdef,
                             const ObDASTabletLoc *tablet_loc,
                             ObDMLRtCtx &dml_rtctx,
                             const ExprFixedArray &new_row)
{
  return write_row_to_das_op<DAS_OP_TABLE_INSERT>(ins_ctdef, ins_rtdef, tablet_loc, dml_rtctx, new_row);
}

int ObDMLService::delete_row(const ObDelCtDef &del_ctdef,
                             ObDelRtDef &del_rtdef,
                             const ObDASTabletLoc *tablet_loc,
                             ObDMLRtCtx &dml_rtctx)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_dml_tablet_validity(dml_rtctx,
                                        *tablet_loc,
                                        del_ctdef.old_row_,
                                        del_ctdef,
                                        del_rtdef))) {
    LOG_WARN("check old row tablet validity failed", K(ret));
  } else if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_DELETE>(del_ctdef.das_ctdef_,
                                                              del_rtdef.das_rtdef_,
                                                              tablet_loc,
                                                              dml_rtctx,
                                                              del_ctdef.old_row_))) {
    LOG_WARN("delete old row from das failed", K(ret));
  }
  return ret;
}
int ObDMLService::lock_row(const ObLockCtDef &lock_ctdef,
                           ObLockRtDef &lock_rtdef,
                           const ObDASTabletLoc *tablet_loc,
                           ObDMLRtCtx &dml_rtctx)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_dml_tablet_validity(dml_rtctx,
                                        *tablet_loc,
                                        lock_ctdef.old_row_,
                                        lock_ctdef,
                                        lock_rtdef))) {
    LOG_WARN("check old row tablet validity failed", K(ret));
  } else if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_LOCK>(lock_ctdef.das_ctdef_,
                                                lock_rtdef.das_rtdef_,
                                                tablet_loc,
                                                dml_rtctx,
                                                lock_ctdef.old_row_))) {
    LOG_WARN("lock row to das failed", K(ret));
  }
  return ret;
}

int ObDMLService::lock_row(const ObDASLockCtDef &dlock_ctdef,
                           ObDASLockRtDef &dlock_rtdef,
                           const ObDASTabletLoc *tablet_loc,
                           ObDMLRtCtx &das_rtctx,
                           const ExprFixedArray &old_row)
{
  return write_row_to_das_op<DAS_OP_TABLE_LOCK>(dlock_ctdef,
                                                dlock_rtdef,
                                                tablet_loc,
                                                das_rtctx,
                                                old_row);
}

int ObDMLService::update_row(const ObUpdCtDef &upd_ctdef,
                             ObUpdRtDef &upd_rtdef,
                             const ObDASTabletLoc *old_tablet_loc,
                             const ObDASTabletLoc *new_tablet_loc,
                             ObDMLRtCtx &dml_rtctx)
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(dml_rtctx.get_exec_ctx());
  if (OB_FAIL(check_dml_tablet_validity(dml_rtctx,
                                        *old_tablet_loc,
                                        upd_ctdef.old_row_,
                                        upd_ctdef,
                                        upd_rtdef))) {
    LOG_WARN("check update old row tablet validity failed", K(ret));
  } else if (OB_FAIL(check_dml_tablet_validity(dml_rtctx,
                                               *new_tablet_loc,
                                               upd_ctdef.new_row_,
                                               upd_ctdef,
                                               upd_rtdef))) {
    LOG_WARN("check update new row tablet validity failed", K(ret));
  } else if (OB_UNLIKELY(!upd_rtdef.is_row_changed_)) {
    //old row is equal to new row, only need to lock row
    if (OB_ISNULL(upd_rtdef.dlock_rtdef_)) {
      ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
      if (OB_FAIL(init_das_lock_rtdef_for_update(dml_rtctx, upd_ctdef, upd_rtdef))) {
        LOG_WARN("init das lock rtdef failed", K(ret), K(upd_ctdef), K(upd_rtdef));
      } else {
        upd_rtdef.dlock_rtdef_->for_upd_wait_time_ = plan_ctx->get_ps_timeout_timestamp();
      }
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_LOCK>(*upd_ctdef.dlock_ctdef_,
                                                         *upd_rtdef.dlock_rtdef_,
                                                         old_tablet_loc,
                                                         dml_rtctx,
                                                         upd_ctdef.old_row_))) {
        LOG_WARN("write row to das op failed", K(ret), K(upd_ctdef), K(upd_rtdef));
      }
    }
  } else if (OB_UNLIKELY(old_tablet_loc != new_tablet_loc)) {
    //the updated row may be moved across partitions
    if (OB_LIKELY(!upd_ctdef.multi_ctdef_->is_enable_row_movement_)) {
      ret = OB_ERR_UPD_CAUSE_PART_CHANGE;
      LOG_WARN("the updated row is moved across partitions", K(ret),
               KPC(old_tablet_loc), KPC(new_tablet_loc));
    } else if (OB_ISNULL(upd_rtdef.ddel_rtdef_)) {
      if (OB_FAIL(init_das_del_rtdef_for_update(dml_rtctx, upd_ctdef, upd_rtdef))) {
        LOG_WARN("init das delete rtdef failed", K(ret), K(upd_ctdef), K(upd_rtdef));
      } else if (OB_FAIL(init_das_ins_rtdef_for_update(dml_rtctx, upd_ctdef, upd_rtdef))) {
        LOG_WARN("init das insert rtdef failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      //because of this bug: https://work.aone.alibaba-inc.com/issue/31915604
      //if the updated row is moved across partitions, we must delete old row at first
      //and then store new row to a temporary buffer,
      //only when all old rows have been deleted, new rows can be inserted
      if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_DELETE>(*upd_ctdef.ddel_ctdef_,
                                                           *upd_rtdef.ddel_rtdef_,
                                                           old_tablet_loc,
                                                           dml_rtctx,
                                                           upd_ctdef.old_row_))) {
        LOG_WARN("delete row to das op failed", K(ret), K(upd_ctdef), K(upd_rtdef));
      } else if (upd_ctdef.is_heap_table_ &&
          OB_FAIL(set_update_hidden_pk(dml_rtctx.get_eval_ctx(),
                                       upd_ctdef,
                                       new_tablet_loc->tablet_id_))) {
        LOG_WARN("update across partitions fail to set new_hidden_pk", K(ret), KPC(new_tablet_loc));
      } else if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_INSERT>(*upd_ctdef.dins_ctdef_,
                                                                  *upd_rtdef.dins_rtdef_,
                                                                  new_tablet_loc,
                                                                  dml_rtctx,
                                                                  upd_ctdef.new_row_))) {
        LOG_WARN("insert row to das op failed", K(ret), K(upd_ctdef), K(upd_rtdef));
      } else {
        LOG_DEBUG("update pkey changed", K(ret), KPC(old_tablet_loc), KPC(new_tablet_loc),
                  "old row", ROWEXPR2STR(dml_rtctx.get_eval_ctx(), upd_ctdef.old_row_),
                  "new row", ROWEXPR2STR(dml_rtctx.get_eval_ctx(), upd_ctdef.new_row_));
      }
    }
  } else if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_UPDATE>(upd_ctdef.dupd_ctdef_,
                                                              upd_rtdef.dupd_rtdef_,
                                                              old_tablet_loc,
                                                              dml_rtctx,
                                                              upd_ctdef.full_row_))) {
    LOG_WARN("write row to das op failed", K(ret), K(upd_ctdef), K(upd_rtdef));
  } else {
    LOG_DEBUG("update pkey not changed", K(ret), KPC(old_tablet_loc),
              "old row", ROWEXPR2STR(dml_rtctx.get_eval_ctx(), upd_ctdef.old_row_),
              "new row", ROWEXPR2STR(dml_rtctx.get_eval_ctx(), upd_ctdef.new_row_));
  }
  return ret;
}

int ObDMLService::delete_row(const ObDASDelCtDef &das_del_ctdef,
                             ObDASDelRtDef &das_del_rtdef,
                             const ObDASTabletLoc *tablet_loc,
                             ObDMLRtCtx &das_rtctx,
                             const ExprFixedArray &old_row)
{
  return write_row_to_das_op<DAS_OP_TABLE_DELETE>(das_del_ctdef,
                                                  das_del_rtdef,
                                                  tablet_loc,
                                                  das_rtctx,
                                                  old_row);
}

int ObDMLService::init_dml_param(const ObDASDMLBaseCtDef &base_ctdef,
                                 ObDASDMLBaseRtDef &base_rtdef,
                                 transaction::ObTxReadSnapshot &snapshot,
                                 ObIAllocator &das_alloc,
                                 storage::ObDMLBaseParam &dml_param)
{
  int ret = OB_SUCCESS;
  dml_param.timeout_ = base_rtdef.timeout_ts_;
  dml_param.schema_version_ = base_ctdef.schema_version_;
  dml_param.is_total_quantity_log_ = base_ctdef.is_total_quantity_log_;
  dml_param.tz_info_ = &base_ctdef.tz_info_;
  dml_param.sql_mode_ = base_rtdef.sql_mode_;
  dml_param.table_param_ = &base_ctdef.table_param_;
  dml_param.tenant_schema_version_ = base_rtdef.tenant_schema_version_;
  dml_param.encrypt_meta_ = &base_ctdef.encrypt_meta_;
  dml_param.prelock_ = base_rtdef.prelock_;
  dml_param.is_batch_stmt_ = base_ctdef.is_batch_stmt_;
  dml_param.dml_allocator_ = &das_alloc;
  dml_param.snapshot_ = snapshot;
  return ret;
}

int ObDMLService::init_das_dml_rtdef(ObDMLRtCtx &dml_rtctx,
                                     const ObDASDMLBaseCtDef &das_ctdef,
                                     ObDASDMLBaseRtDef &das_rtdef,
                                     const ObDASTableLocMeta *loc_meta)
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(dml_rtctx.get_exec_ctx());
  ObSQLSessionInfo *my_session = GET_MY_SESSION(dml_rtctx.get_exec_ctx());
  ObDASCtx &das_ctx = dml_rtctx.get_exec_ctx().get_das_ctx();
  uint64_t table_loc_id = das_ctdef.table_id_;
  uint64_t ref_table_id = das_ctdef.index_tid_;
  das_rtdef.timeout_ts_ = plan_ctx->get_ps_timeout_timestamp();
  das_rtdef.prelock_ = my_session->get_prelock();
  das_rtdef.tenant_schema_version_ = plan_ctx->get_tenant_schema_version();
  das_rtdef.sql_mode_ = my_session->get_sql_mode();
  if (OB_ISNULL(das_rtdef.table_loc_ = dml_rtctx.op_.get_input()->get_table_loc())) {
    das_rtdef.table_loc_ = das_ctx.get_table_loc_by_id(table_loc_id, ref_table_id);
    if (OB_ISNULL(das_rtdef.table_loc_)) {
      if (OB_ISNULL(loc_meta)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("loc meta is null", K(ret), K(table_loc_id), K(ref_table_id), K(das_ctx.get_table_loc_list()));
      } else if (OB_FAIL(das_ctx.extended_table_loc(*loc_meta, das_rtdef.table_loc_))) {
        LOG_WARN("extended table location failed", K(ret), KPC(loc_meta));
      }
    }
  }
  //related local index tablet_id pruning only can be used in local plan or remote plan(all operator
  //use the same das context),
  //because the distributed plan will transfer tablet_id through exchange operator,
  //but the related tablet_id map can not be transfered by exchange operator,
  //unused related pruning in distributed plan's dml operator,
  //we will use get_all_tablet_and_object_id() to build the related tablet_id map when
  //dml operator's table loc was inited
  if (OB_SUCC(ret) && !das_ctdef.table_param_.get_data_table().is_index_table()) {
    const ObDASTableLocMeta *final_loc_meta = das_rtdef.table_loc_->loc_meta_;
    if (!final_loc_meta->related_table_ids_.empty() &&
        dml_rtctx.op_.get_spec().plan_->is_distributed_plan()) {
      ObDASTabletMapper tablet_mapper;
      ObArray<ObTabletID> tablet_ids;
      ObArray<ObObjectID> partition_ids;
      if (OB_FAIL(das_ctx.get_das_tablet_mapper(ref_table_id, tablet_mapper,
                                                &final_loc_meta->related_table_ids_))) {
        LOG_WARN("get das tablet mapper failed", K(ret));
      } else if (OB_FAIL(tablet_mapper.get_all_tablet_and_object_id(tablet_ids, partition_ids))) {
        LOG_WARN("build related tablet_id map failed", K(ret), KPC(final_loc_meta));
      }
    }
  }
  return ret;
}

int ObDMLService::init_trigger_for_insert(
  ObDMLRtCtx &dml_rtctx,
  const ObInsCtDef &ins_ctdef,
  ObInsRtDef &ins_rtdef,
  ObIArray<ObExpr*> &clear_exprs)
{
  int ret = OB_SUCCESS;
  if (!ins_ctdef.is_primary_index_ || 0 >= ins_ctdef.trig_ctdef_.tg_args_.count()) {
    // nothing
    LOG_DEBUG("debug non-primary key insert trigger for merge", K(ret));
  } else if (OB_FAIL(TriggerHandle::init_trigger_params(dml_rtctx, ins_ctdef.trig_ctdef_.tg_event_,
                          ins_ctdef.trig_ctdef_, ins_rtdef.trig_rtdef_))) {
    LOG_WARN("failed to init trigger params", K(ret));
  } else {
    append(clear_exprs, ins_ctdef.trig_ctdef_.new_row_exprs_);
    LOG_DEBUG("debug insert trigger for merge", K(ret));
  }
  return ret;
}

int ObDMLService::init_ins_rtdef(
  ObDMLRtCtx &dml_rtctx,
  ObInsRtDef &ins_rtdef,
  const ObInsCtDef &ins_ctdef,
  ObIArray<ObExpr*> &clear_exprs)
{
  int ret = OB_SUCCESS;
  dml_rtctx.get_exec_ctx().set_dml_event(ObDmlEventType::DE_INSERTING);
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(ins_ctdef.multi_ctdef_);
  if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                 ins_ctdef.das_ctdef_,
                                 ins_rtdef.das_rtdef_,
                                 loc_meta))) {
    LOG_WARN("failed to init das dml rtdef", K(ret));
  } else if (OB_FAIL(init_related_das_rtdef(dml_rtctx, ins_ctdef.related_ctdefs_, ins_rtdef.related_rtdefs_))) {
    LOG_WARN("init related das ctdef failed", K(ret));
  } else if (OB_FAIL(init_trigger_for_insert(dml_rtctx, ins_ctdef, ins_rtdef, clear_exprs))) {
    LOG_WARN("failed to init trigger for insert", K(ret));
  } else {
    ins_rtdef.das_rtdef_.related_ctdefs_ = &ins_ctdef.related_ctdefs_;
    ins_rtdef.das_rtdef_.related_rtdefs_ = &ins_rtdef.related_rtdefs_;
  }
  return ret;
}

int ObDMLService::init_trigger_for_delete(
  ObDMLRtCtx &dml_rtctx, const ObDelCtDef &del_ctdef, ObDelRtDef &del_rtdef)
{
  int ret = OB_SUCCESS;
  if (del_ctdef.is_primary_index_ && 0 < del_ctdef.trig_ctdef_.tg_args_.count()) {
    OZ(TriggerHandle::init_trigger_params(dml_rtctx, del_ctdef.trig_ctdef_.tg_event_,
                                del_ctdef.trig_ctdef_, del_rtdef.trig_rtdef_));
    LOG_DEBUG("debug delete trigger init", K(ret), K(del_ctdef.is_primary_index_),
      K(del_ctdef.trig_ctdef_.tg_args_.count()));
  }
  return ret;
}

int ObDMLService::init_del_rtdef(ObDMLRtCtx &dml_rtctx,
                                 ObDelRtDef &del_rtdef,
                                 const ObDelCtDef &del_ctdef)
{
  int ret = OB_SUCCESS;
  dml_rtctx.get_exec_ctx().set_dml_event(ObDmlEventType::DE_DELETING);
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(del_ctdef.multi_ctdef_);
  if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                 del_ctdef.das_ctdef_,
                                 del_rtdef.das_rtdef_,
                                 loc_meta))) {
    LOG_WARN("failed to init das dml rfdef", K(ret));
  } else if (OB_FAIL(init_related_das_rtdef(dml_rtctx, del_ctdef.related_ctdefs_, del_rtdef.related_rtdefs_))) {
    LOG_WARN("init related das ctdef failed", K(ret));
  } else if (OB_FAIL(init_trigger_for_delete(dml_rtctx, del_ctdef, del_rtdef))) {
    LOG_WARN("failed to init trigger for delete", K(ret));
  } else {
    del_rtdef.das_rtdef_.related_ctdefs_ = &del_ctdef.related_ctdefs_;
    del_rtdef.das_rtdef_.related_rtdefs_ = &del_rtdef.related_rtdefs_;
  }


  if (OB_SUCC(ret)) {
    ObTableModifyOp &dml_op = dml_rtctx.op_;
    const uint64_t del_table_id = del_ctdef.das_base_ctdef_.index_tid_;
    ObExecContext *root_ctx = nullptr;
    if (OB_FAIL(dml_op.get_exec_ctx().get_root_ctx(root_ctx))) {
      LOG_WARN("failed to get root exec ctx", K(ret));
    } else if (OB_ISNULL(root_ctx)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the root exec ctx is nullptr", K(ret));
    } else {
      DASDelCtxList& del_ctx_list = root_ctx->get_das_ctx().get_das_del_ctx_list();
      if (!ObDMLService::is_nested_dup_table(del_table_id, del_ctx_list) && T_DISTINCT_NONE != del_ctdef.distinct_algo_) {
        DmlRowkeyDistCtx del_ctx;
        del_ctx.table_id_ = del_table_id;
        if (OB_FAIL(ObDMLService::create_rowkey_check_hashset(dml_op.get_spec().rows_, root_ctx, del_ctx.deleted_rows_))) {
          LOG_WARN("Failed to create hash set", K(ret));
        } else if (OB_FAIL(del_ctx_list.push_back(del_ctx))) {
          LOG_WARN("failed to push del ctx to list", K(ret));
        } else {
          del_rtdef.se_rowkey_dist_ctx_ = del_ctx.deleted_rows_;
        }
      } else if (T_DISTINCT_NONE != del_ctdef.distinct_algo_ &&
                 OB_FAIL(ObDMLService::get_nested_dup_table_ctx(del_table_id, del_ctx_list, del_rtdef.se_rowkey_dist_ctx_))) {
        LOG_WARN("failed to get nested duplicate delete table ctx for fk nested session", K(ret));
      } else if (dml_op.is_fk_nested_session() && OB_FAIL(ObDMLService::get_nested_dup_table_ctx(del_table_id,
                                                                del_ctx_list,
                                                                del_rtdef.se_rowkey_dist_ctx_))) {
        LOG_WARN("failed to get nested duplicate delete table ctx for fk nested session", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLService::init_trigger_for_update(
  ObDMLRtCtx &dml_rtctx,
  const ObUpdCtDef &upd_ctdef,
  ObUpdRtDef &upd_rtdef,
  ObTableModifyOp &dml_op,
  ObIArray<ObExpr*> &clear_exprs)
{
  UNUSED(dml_op);
  int ret = OB_SUCCESS;
  if (!upd_ctdef.is_primary_index_ || 0 >= upd_ctdef.trig_ctdef_.tg_args_.count()) {
    // nothing
    LOG_DEBUG("debug non-primary key update trigger for merge", K(ret));
  } else if (OB_FAIL(TriggerHandle::init_trigger_params(dml_rtctx, upd_ctdef.trig_ctdef_.tg_event_,
                          upd_ctdef.trig_ctdef_, upd_rtdef.trig_rtdef_))) {
    LOG_WARN("failed to init trigger params", K(ret));
  } else {
    ObSEArray<ObExpr *, 4> new_exprs;
    for (int64_t i = 0; i < upd_ctdef.trig_ctdef_.new_row_exprs_.count() && OB_SUCC(ret); ++i) {
      if (upd_ctdef.trig_ctdef_.new_row_exprs_.at(i) != upd_ctdef.trig_ctdef_.old_row_exprs_.at(i)) {
        OZ(new_exprs.push_back(upd_ctdef.trig_ctdef_.new_row_exprs_.at(i)));
      }
    }
    if (OB_SUCC(ret)) {
      append(clear_exprs, new_exprs);
    }
    LOG_DEBUG("debug update trigger for merge", K(ret));
  }
  return ret;
}

int ObDMLService::init_upd_rtdef(
  ObDMLRtCtx &dml_rtctx,
  ObUpdRtDef &upd_rtdef,
  const ObUpdCtDef &upd_ctdef,
  ObIArray<ObExpr*> &clear_exprs)
{
  int ret = OB_SUCCESS;
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(upd_ctdef.multi_ctdef_);
  dml_rtctx.get_exec_ctx().set_dml_event(ObDmlEventType::DE_UPDATING);
  if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                 upd_ctdef.dupd_ctdef_,
                                 upd_rtdef.dupd_rtdef_,
                                 loc_meta))) {
    LOG_WARN("failed to init das dml rfdef", K(ret));
  } else if (OB_FAIL(init_related_das_rtdef(dml_rtctx, upd_ctdef.related_upd_ctdefs_, upd_rtdef.related_upd_rtdefs_))) {
    LOG_WARN("init related das ctdef failed", K(ret));
  } else if (OB_FAIL(init_trigger_for_update(dml_rtctx, upd_ctdef, upd_rtdef, dml_rtctx.op_, clear_exprs))) {
    LOG_WARN("failed to init trigger for update", K(ret));
  } else {
    upd_rtdef.dupd_rtdef_.related_ctdefs_ = &upd_ctdef.related_upd_ctdefs_;
    upd_rtdef.dupd_rtdef_.related_rtdefs_ = &upd_rtdef.related_upd_rtdefs_;
    dml_rtctx.get_exec_ctx().set_update_columns(&upd_ctdef.assign_columns_);
  }

  if (OB_SUCC(ret) && T_DISTINCT_NONE != upd_ctdef.distinct_algo_) {
    ObTableModifyOp &dml_op = dml_rtctx.op_;
    if (OB_FAIL(create_rowkey_check_hashset(dml_op.get_spec().rows_,
                                            &dml_op.get_exec_ctx(),
                                            upd_rtdef.se_rowkey_dist_ctx_))) {
      LOG_WARN("failed to create distinct check hash set", K(ret));
    }
  }
  return ret;
}

int ObDMLService::init_das_ins_rtdef_for_update(ObDMLRtCtx &dml_rtctx,
                                                const ObUpdCtDef &upd_ctdef,
                                                ObUpdRtDef &upd_rtdef)
{
  int ret = OB_SUCCESS;
  ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(upd_ctdef.multi_ctdef_);
  if (OB_FAIL(ObDASTaskFactory::alloc_das_rtdef(DAS_OP_TABLE_INSERT,
                                                allocator,
                                                upd_rtdef.dins_rtdef_))) {
    LOG_WARN("create das delete rtdef failed", K(ret));
  } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                        *upd_ctdef.dins_ctdef_,
                                        *upd_rtdef.dins_rtdef_,
                                        loc_meta))) {
    LOG_WARN("init das insert rtdef failed", K(ret));
  } else if (OB_FAIL(init_related_das_rtdef(dml_rtctx, upd_ctdef.related_ins_ctdefs_, upd_rtdef.related_ins_rtdefs_))) {
    LOG_WARN("init related das insert ctdef failed", K(ret));
  } else {
    upd_rtdef.dins_rtdef_->related_ctdefs_ = &upd_ctdef.related_ins_ctdefs_;
    upd_rtdef.dins_rtdef_->related_rtdefs_ = &upd_rtdef.related_ins_rtdefs_;
  }
  return ret;
}

int ObDMLService::init_das_del_rtdef_for_update(ObDMLRtCtx &dml_rtctx,
                                                const ObUpdCtDef &upd_ctdef,
                                                ObUpdRtDef &upd_rtdef)
{
  int ret = OB_SUCCESS;
  ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(upd_ctdef.multi_ctdef_);
  if (OB_FAIL(ObDASTaskFactory::alloc_das_rtdef(DAS_OP_TABLE_DELETE,
                                                allocator,
                                                upd_rtdef.ddel_rtdef_))) {
    LOG_WARN("create das delete rtdef failed", K(ret));
  } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                        *upd_ctdef.ddel_ctdef_,
                                        *upd_rtdef.ddel_rtdef_,
                                        loc_meta))) {
    LOG_WARN("init das dml rtdef failed", K(ret), K(upd_ctdef), K(upd_rtdef));
  } else if (OB_FAIL(init_related_das_rtdef(dml_rtctx, upd_ctdef.related_del_ctdefs_, upd_rtdef.related_del_rtdefs_))) {
    LOG_WARN("init related das ctdef failed", K(ret));
  } else {
    upd_rtdef.ddel_rtdef_->related_ctdefs_ = &upd_ctdef.related_del_ctdefs_;
    upd_rtdef.ddel_rtdef_->related_rtdefs_ = &upd_rtdef.related_del_rtdefs_;
  }
  return ret;
}

int ObDMLService::init_das_lock_rtdef_for_update(ObDMLRtCtx &dml_rtctx,
                                                 const ObUpdCtDef &upd_ctdef,
                                                 ObUpdRtDef &upd_rtdef)
{
  int ret = OB_SUCCESS;
  ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(upd_ctdef.multi_ctdef_);
  if (OB_FAIL(ObDASTaskFactory::alloc_das_rtdef(DAS_OP_TABLE_LOCK,
                                                allocator,
                                                upd_rtdef.dlock_rtdef_))) {
    LOG_WARN("create das lock rtdef failed", K(ret));
  } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                        *upd_ctdef.dlock_ctdef_,
                                        *upd_rtdef.dlock_rtdef_,
                                        loc_meta))) {
    LOG_WARN("init das dml rtdef failed", K(ret), K(upd_ctdef), K(upd_rtdef));
  }
  return ret;
}

int ObDMLService::init_lock_rtdef(ObDMLRtCtx &dml_rtctx,
                                  const ObLockCtDef &lock_ctdef,
                                  ObLockRtDef &lock_rtdef,
                                  int64_t wait_ts)
{
  lock_rtdef.das_rtdef_.for_upd_wait_time_ = wait_ts;
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(lock_ctdef.multi_ctdef_);
  return init_das_dml_rtdef(dml_rtctx, lock_ctdef.das_ctdef_, lock_rtdef.das_rtdef_, loc_meta);
}

template <int N>
int ObDMLService::write_row_to_das_op(const ObDASDMLBaseCtDef &ctdef,
                                      ObDASDMLBaseRtDef &rtdef,
                                      const ObDASTabletLoc *tablet_loc,
                                      ObDMLRtCtx &dml_rtctx,
                                      const ExprFixedArray &row)
{
  int ret = OB_SUCCESS;
  bool need_retry = false;
  typedef typename das_reg::ObDASOpTypeTraits<N>::DASCtDef CtDefType;
  typedef typename das_reg::ObDASOpTypeTraits<N>::DASRtDef RtDefType;
  typedef typename das_reg::ObDASOpTypeTraits<N>::DASOp OpType;
  OB_ASSERT(typeid(ctdef) == typeid(CtDefType));
  OB_ASSERT(typeid(rtdef) == typeid(RtDefType));
  do {
    bool buffer_full = false;
    need_retry = false;
    //1. find das dml op
    OpType *dml_op = nullptr;
    if (OB_UNLIKELY(!dml_rtctx.das_ref_.has_das_op(tablet_loc, dml_op))) {
      if (OB_FAIL(dml_rtctx.das_ref_.prepare_das_task(tablet_loc, dml_op))) {
        LOG_WARN("prepare das task failed", K(ret), K(N));
      } else {
        dml_op->set_das_ctdef(static_cast<const CtDefType*>(&ctdef));
        dml_op->set_das_rtdef(static_cast<RtDefType*>(&rtdef));
        rtdef.table_loc_->is_writing_ = true;
      }
      if (OB_SUCC(ret) &&
          rtdef.related_ctdefs_ != nullptr && !rtdef.related_ctdefs_->empty()) {
        if (OB_FAIL(add_related_index_info(*tablet_loc,
                                           *rtdef.related_ctdefs_,
                                           *rtdef.related_rtdefs_,
                                           *dml_op))) {
          LOG_WARN("add related index info failed", K(ret));
        }
      }
    }
    //2. try add row to das dml buffer
    if (OB_SUCC(ret)) {
      int64_t simulate_row_cnt = - EVENT_CALL(EventTable::EN_DAS_DML_BUFFER_OVERFLOW);
      if (OB_UNLIKELY(simulate_row_cnt > 0 && dml_op->get_row_cnt() >= simulate_row_cnt)) {
        buffer_full = true;
      } else if (OB_FAIL(dml_op->write_row(row, dml_rtctx.get_eval_ctx(), buffer_full))) {
        LOG_WARN("insert row to das dml op buffer failed", K(ret), K(ctdef), K(rtdef));
      }
      LOG_DEBUG("write row to das op", K(ret), K(buffer_full), "op_type", N,
                "table_id", ctdef.table_id_, "index_tid", ctdef.index_tid_,
                "row", ROWEXPR2STR(dml_rtctx.get_eval_ctx(), row));
    }
    //3. if buffer is full, flush das task, and retry to add row
    if (OB_SUCC(ret) && buffer_full) {
      need_retry = true;
      if (REACH_COUNT_INTERVAL(10)) { // print log per 10 times.
        LOG_INFO("DAS write buffer full, ", K(dml_op->get_row_cnt()), K(dml_rtctx.get_das_alloc().used()));
      }
      if (OB_UNLIKELY(dml_rtctx.need_non_sub_full_task())) {
        // 因为replace into 和 insert up在做try_insert时，需要返回duplicated row，
        // 所以写满的das task现在不能提交，先frozen das task list，
        // 等所有insert_row全部写完之后再统一提交
        dml_rtctx.das_ref_.set_frozen_node();
      } else {
        if (dml_rtctx.need_pick_del_task_first() &&
                   OB_FAIL(dml_rtctx.das_ref_.pick_del_task_to_first())) {
          LOG_WARN("fail to pick delete das task to first", K(ret));
        } else if (OB_FAIL(dml_rtctx.op_.submit_all_dml_task())) {
          LOG_WARN("submit all dml task failed", K(ret));
        }
      }
    }
  } while (OB_SUCC(ret) && need_retry);
  return ret;
}

int ObDMLService::add_related_index_info(const ObDASTabletLoc &tablet_loc,
                                         const DASDMLCtDefArray &related_ctdefs,
                                         DASDMLRtDefArray &related_rtdefs,
                                         ObIDASTaskOp &das_op)
{
  int ret = OB_SUCCESS;
  das_op.get_related_ctdefs().set_capacity(related_ctdefs.count());
  das_op.get_related_rtdefs().set_capacity(related_rtdefs.count());
  das_op.get_related_tablet_ids().set_capacity(related_ctdefs.count());
  for (int64_t i = 0; OB_SUCC(ret) && i < related_ctdefs.count(); ++i) {
    ObDASTabletLoc *index_tablet_loc = ObDASUtils::get_related_tablet_loc(
        const_cast<ObDASTabletLoc&>(tablet_loc), related_ctdefs.at(i)->index_tid_);
    if (OB_FAIL(das_op.get_related_ctdefs().push_back(related_ctdefs.at(i)))) {
      LOG_WARN("store related ctdef failed", K(ret), K(related_ctdefs), K(i));
    } else if (OB_FAIL(das_op.get_related_rtdefs().push_back(related_rtdefs.at(i)))) {
      LOG_WARN("store related rtdef failed", K(ret));
    } else if (OB_FAIL(das_op.get_related_tablet_ids().push_back(index_tablet_loc->tablet_id_))) {
      LOG_WARN("store related index tablet id failed", K(ret));
    }
  }
  return ret;
}

int ObDMLService::convert_exprs_to_stored_row(ObIAllocator &allocator,
                                              ObEvalCtx &eval_ctx,
                                              const ObExprPtrIArray &exprs,
                                              ObChunkDatumStore::StoredRow *&new_row)
{
  return ObChunkDatumStore::StoredRow::build(new_row, exprs, eval_ctx, allocator);
}

int ObDMLService::catch_violate_error(int err_ret,
                                      int64_t savepoint_no,
                                      ObDMLRtCtx &dml_rtctx,
                                      ObErrLogRtDef &err_log_rt_def,
                                      ObErrLogCtDef &error_logging_ctdef,
                                      ObErrLogService &err_log_service,
                                      ObDASOpType type)
{
  int ret = OB_SUCCESS;
  int rollback_ret = OB_SUCCESS;
  // 1. if there is no exception in the previous processing, then write this row to storage layer
  if (OB_SUCC(err_ret) &&
      err_log_rt_def.first_err_ret_ == OB_SUCCESS) {
    if (OB_FAIL(write_one_row_post_proc(dml_rtctx))) {
      if (OB_ERR_PRIMARY_KEY_DUPLICATE == ret) {
        err_log_rt_def.first_err_ret_ = ret;
        if (OB_SUCCESS != (rollback_ret = ObSqlTransControl::rollback_savepoint(dml_rtctx.get_exec_ctx(), savepoint_no))) {
          ret = rollback_ret;
          LOG_WARN("fail to rollback save point", K(rollback_ret));
        }
      } else {
        LOG_WARN("fail to insert row ", K(ret));
      }
    }
  }

  // 2. if cache some err, must write err info to error logging table
  // if need write error logging table, will cover error_code
  // 3. should_catch_err(ret) == true -> write_one_row_post_proc throw some err
  // 4. should_catch_err(err_ret) == true -> some error occur before write storage
  if (OB_SUCCESS == rollback_ret &&
      OB_SUCCESS != err_log_rt_def.first_err_ret_ &&
      (should_catch_err(ret) || should_catch_err(err_ret))) {
    if (OB_FAIL(err_log_service.insert_err_log_record(GET_MY_SESSION(dml_rtctx.get_exec_ctx()),
                                                      error_logging_ctdef,
                                                      err_log_rt_def,
                                                      type))) {
      LOG_WARN("fail to insert_err_log_record", K(ret));
    }
  }
  return ret;
}

int ObDMLService::write_one_row_post_proc(ObDMLRtCtx &dml_rtctx)
{
  int ret = OB_SUCCESS;
  int close_ret = OB_SUCCESS;
  if (dml_rtctx.das_ref_.has_task()) {
    if (OB_FAIL(dml_rtctx.das_ref_.execute_all_task())) {
      LOG_WARN("execute all update das task failed", K(ret));
    }

    // whether execute result is success or fail , must close all task
    if (OB_SUCCESS != (close_ret = (dml_rtctx.das_ref_.close_all_task()))) {
      LOG_WARN("close all das task failed", K(ret));
    } else {
      dml_rtctx.reuse();
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("must have das task");
  }

  ret = (ret == OB_SUCCESS ? close_ret : ret);
  return ret;
}

int ObDMLService::copy_heap_table_hidden_pk(ObEvalCtx &eval_ctx,
                                            const ObUpdCtDef &upd_ctdef)
{
  int ret = OB_SUCCESS;
  ObExpr *old_hidden_pk = upd_ctdef.old_row_.at(0);
  ObExpr *new_hidden_pk = upd_ctdef.new_row_.at(0);
  ObDatum *hidden_pk_datum = NULL;
  if (OB_ISNULL(old_hidden_pk) || OB_ISNULL(new_hidden_pk)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("expr is unexpected null", K(ret), KPC(old_hidden_pk), KPC(new_hidden_pk));
  } else if (!upd_ctdef.is_heap_table_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("is not heap_table", K(ret), K(upd_ctdef));
  } else if (new_hidden_pk->type_ != T_TABLET_AUTOINC_NEXTVAL) {
    LOG_TRACE("heap table not update the part_key", K(ret));
  } else if (OB_FAIL(old_hidden_pk->eval(eval_ctx, hidden_pk_datum))) {
    LOG_WARN("eval old_hidden_pk failed", K(ret), KPC(old_hidden_pk));
  } else if (OB_ISNULL(hidden_pk_datum)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("hidden_pk_datum is null", K(ret), KPC(old_hidden_pk));
  } else if (!old_hidden_pk->obj_meta_.is_uint64()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("hidden_pk must be uint64 type", K(ret), KPC(old_hidden_pk));
  } else {
    ObDatum &datum = new_hidden_pk->locate_datum_for_write(eval_ctx);
    datum.set_uint(hidden_pk_datum->get_uint());
    new_hidden_pk->get_eval_info(eval_ctx).evaluated_ = true;
  }
  return ret;
}

int ObDMLService::set_update_hidden_pk(ObEvalCtx &eval_ctx,
                                       const ObUpdCtDef &upd_ctdef,
                                       const ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;
  uint64_t autoinc_seq = 0;
  if (upd_ctdef.is_heap_table_ && upd_ctdef.is_primary_index_) {
    ObExpr *auto_inc_expr = upd_ctdef.new_row_.at(0);
    ObSQLSessionInfo *my_session = eval_ctx.exec_ctx_.get_my_session();
    uint64_t tenant_id = my_session->get_effective_tenant_id();
    if (OB_FAIL(get_heap_table_hidden_pk(tenant_id, tablet_id, autoinc_seq))) {
      LOG_WARN("fail to het hidden pk", K(ret), K(tablet_id), K(tenant_id));
    } else if (OB_ISNULL(auto_inc_expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("new_hidden_pk_expr is null", K(ret), K(upd_ctdef));
    } else if (auto_inc_expr->type_ != T_TABLET_AUTOINC_NEXTVAL) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the first expr is not tablet_auto_inc column", K(ret), KPC(auto_inc_expr));
    } else {
      ObDatum &datum = auto_inc_expr->locate_datum_for_write(eval_ctx);
      datum.set_uint(autoinc_seq);
      auto_inc_expr->get_eval_info(eval_ctx).evaluated_ = true;
    }
  }
  return ret;
}

int ObDMLService::get_heap_table_hidden_pk(uint64_t tenant_id,
                                           const ObTabletID &tablet_id,
                                           uint64_t &pk)
{
  int ret = OB_SUCCESS;
  uint64_t autoinc_seq = 0;
  ObTabletAutoincrementService &auto_inc = ObTabletAutoincrementService::get_instance();
  if (OB_FAIL(auto_inc.get_autoinc_seq(tenant_id, tablet_id, autoinc_seq))) {
    LOG_WARN("get_autoinc_seq fail", K(ret), K(tenant_id), K(tablet_id));
  } else {
    pk = autoinc_seq;
    LOG_TRACE("after get autoinc_seq", K(pk), K(tenant_id), K(tablet_id));
  }
  return ret;
}

int ObDMLService::set_heap_table_hidden_pk(const ObInsCtDef &ins_ctdef,
                                           const ObTabletID &tablet_id,
                                           ObEvalCtx &eval_ctx)
{
  int ret = OB_SUCCESS;
  uint64_t autoinc_seq = 0;
  if (ins_ctdef.is_heap_table_ && ins_ctdef.is_primary_index_) {
    ObSQLSessionInfo *my_session = eval_ctx.exec_ctx_.get_my_session();
    uint64_t tenant_id = my_session->get_effective_tenant_id();
    if (OB_FAIL(ObDMLService::get_heap_table_hidden_pk(tenant_id,
                                                       tablet_id,
                                                       autoinc_seq))) {
      LOG_WARN("fail to het hidden pk", K(ret), K(tablet_id), K(tenant_id));
    } else {
      ObExpr *auto_inc_expr = ins_ctdef.new_row_.at(0);
      if (auto_inc_expr->type_ != T_TABLET_AUTOINC_NEXTVAL) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the first expr is not tablet_auto_inc column", K(ret), KPC(auto_inc_expr));
      } else {
        ObDatum &datum = auto_inc_expr->locate_datum_for_write(eval_ctx);
        datum.set_uint(autoinc_seq);
        auto_inc_expr->get_eval_info(eval_ctx).evaluated_ = true;
      }
    }
  }
  return ret;
}

int ObDMLService::check_nested_sql_legality(ObExecContext &ctx, common::ObTableID ref_table_id)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = ctx.get_my_session();
  if (session->is_remote_session() && ctx.get_parent_ctx() != nullptr) {
    //in nested sql, and the sql is remote or distributed
    pl::ObPLContext *pl_ctx = ctx.get_parent_ctx()->get_pl_stack_ctx();
    if (pl_ctx == nullptr || !pl_ctx->in_autonomous()) {
      //this nested sql require transaction scheduler control
      //but the session is remote, means this sql executing without transaction scheduler control
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "Perform a DML operation inside a query or remote/distributed sql");
      LOG_WARN("check nested sql legality failed", K(ret), K(pl_ctx));
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(ObDASUtils::check_nested_sql_mutating(ref_table_id, ctx))) {
      LOG_WARN("check nested sql mutating failed", K(ret));
    }
  }
  return ret;
}

int ObDMLService::create_anonymous_savepoint(ObTxDesc &tx_desc, int64_t &savepoint)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObSqlTransControl::create_anonymous_savepoint(tx_desc, savepoint))) {
    LOG_WARN("create savepoint failed", K(ret));
  }
  return ret;
}

int ObDMLService::rollback_local_savepoint(ObTxDesc &tx_desc, int64_t savepoint, int64_t expire_ts)
{
  int ret = OB_SUCCESS;
  ObTransService *tx = MTL(transaction::ObTransService*);
  if (OB_FAIL(tx->rollback_to_implicit_savepoint(tx_desc, savepoint, expire_ts, nullptr))) {
    LOG_WARN("rollback to implicit local savepoint failed", K(ret));
  }
  return ret;
}

int ObDMLService::check_local_index_affected_rows(int64_t table_affected_rows,
                                                  int64_t index_affected_rows,
                                                  const ObDASDMLBaseCtDef &ctdef,
                                                  ObDASDMLBaseRtDef &rtdef,
                                                  const ObDASDMLBaseCtDef &related_ctdef,
                                                  ObDASDMLBaseRtDef &related_rtdef)
{
  int ret = OB_SUCCESS;
  if (GCONF.enable_defensive_check()) {
    if (table_affected_rows != index_affected_rows) {
      ret = OB_ERR_DEFENSIVE_CHECK;
      ObString func_name = ObString::make_string("check_local_index_affected_rows");
      LOG_USER_ERROR(OB_ERR_DEFENSIVE_CHECK, func_name.length(), func_name.ptr());
      SQL_DAS_LOG(ERROR, "Fatal Error!!! data table affected row is not match with index table", K(ret),
                  "table_affected_rows", table_affected_rows,
                  "index_affected_rows", index_affected_rows,
                  "table_ctdef", ctdef,
                  "index_ctdef", related_ctdef,
                  "table_rtdef", rtdef,
                  "index_rtdef", related_rtdef);
    }
  }
  return ret;
}

template <typename T>
const ObDASTableLocMeta *ObDMLService::get_table_loc_meta(const T *multi_ctdef)
{
  const ObDASTableLocMeta *loc_meta = nullptr;
  if (multi_ctdef != nullptr) {
    loc_meta = &(multi_ctdef->loc_meta_);
  }
  return loc_meta;
}

int ObDMLService::init_related_das_rtdef(ObDMLRtCtx &dml_rtctx,
                                         const DASDMLCtDefArray &das_ctdefs,
                                         DASDMLRtDefArray &das_rtdefs)
{
  int ret = OB_SUCCESS;
  if (!das_ctdefs.empty()) {
    ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
    if (OB_FAIL(das_rtdefs.allocate_array(allocator, das_ctdefs.count()))) {
      SQL_DAS_LOG(WARN, "create das insert rtdef array failed", K(ret));
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < das_ctdefs.count(); ++i) {
    ObDASTaskFactory &das_factory = dml_rtctx.get_exec_ctx().get_das_ctx().get_das_factory();
    ObDASBaseRtDef *das_rtdef = nullptr;
    if (OB_FAIL(das_factory.create_das_rtdef(das_ctdefs.at(i)->op_type_, das_rtdef))) {
      SQL_DAS_LOG(WARN, "create das insert rtdef failed", K(ret));
    } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                          *das_ctdefs.at(i),
                                          static_cast<ObDASDMLBaseRtDef&>(*das_rtdef),
                                          nullptr))) {
      SQL_DAS_LOG(WARN, "init das dml rtdef failed", K(ret));
    } else {
      das_rtdefs.at(i) = static_cast<ObDASDMLBaseRtDef*>(das_rtdef);
    }
  }
  return ret;
}

int ObDMLService::check_dml_tablet_validity(ObDMLRtCtx &dml_rtctx,
                                            const ObDASTabletLoc &tablet_loc,
                                            const ExprFixedArray &row,
                                            const ObDMLBaseCtDef &dml_ctdef,
                                            ObDMLBaseRtDef &dml_rtdef)
{
  int ret = OB_SUCCESS;
  if (GCONF.enable_strict_defensive_check()) {
    //only strict defensive check need to check tablet validity, otherwise ignore it
    ObTableLocation *tmp_location = nullptr;
    ObSEArray<ObTabletID, 1> tablet_ids;
    ObSEArray<ObObjectID, 1> partition_ids;
    uint64_t table_id = tablet_loc.loc_meta_->ref_table_id_;
    ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
    if (OB_ISNULL(dml_rtdef.check_location_)) {
      void *location_buf = allocator.alloc(sizeof(ObTableLocation));
      if (OB_ISNULL(location_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("allocate location buffer failed", K(ret));
      } else {
        dml_rtdef.check_location_ = new(location_buf) ObTableLocation(allocator);
        tmp_location = dml_rtdef.check_location_;
        ObSchemaGetterGuard *schema_guard = dml_rtctx.get_exec_ctx().get_sql_ctx()->schema_guard_;
        ObSqlSchemaGuard sql_schema_guard;
        sql_schema_guard.set_schema_guard(schema_guard);
        if (OB_FAIL(tmp_location->init_table_location_with_column_ids(sql_schema_guard,
                                                                      table_id,
                                                                      dml_ctdef.column_ids_,
                                                                      dml_rtctx.get_exec_ctx()))) {
          LOG_WARN("init table location with column ids failed", K(ret), K(dml_ctdef), K(table_id));
        }
      }
    } else {
      tmp_location = dml_rtdef.check_location_;
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(convert_exprs_to_row(row,
                                       dml_rtctx.get_eval_ctx(),
                                       dml_rtdef,
                                       dml_rtctx.get_exec_ctx().get_allocator()))) {
        LOG_WARN("check dml tablet validity", K(ret));
      } else if (OB_FAIL(tmp_location->calculate_tablet_id_by_row(dml_rtctx.get_exec_ctx(),
                                                                  table_id,
                                                                  dml_ctdef.column_ids_,
                                                                  *dml_rtdef.check_row_,
                                                                  tablet_ids,
                                                                  partition_ids))) {
        LOG_WARN("calculate tablet id by row failed", K(ret));
      } else if (tablet_ids.count() != 1 || tablet_loc.tablet_id_ != tablet_ids.at(0)) {
        ret = OB_ERR_DEFENSIVE_CHECK;
        ObString func_name = ObString::make_string("check_dml_tablet_validity");
        LOG_USER_ERROR(OB_ERR_DEFENSIVE_CHECK, func_name.length(), func_name.ptr());
        LOG_ERROR("Fatal Error!!! Catch a defensive error!", K(ret),
                  K(tablet_loc), K(tablet_ids),
                  KPC(dml_rtdef.check_row_), KPC(dml_rtdef.check_location_));
      }
    }
  }
  return ret;
}

int ObDMLService::convert_exprs_to_row(const ExprFixedArray &exprs,
                                       ObEvalCtx &eval_ctx,
                                       ObDMLBaseRtDef &dml_rtdef,
                                       ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(dml_rtdef.check_row_)) {
    if (OB_FAIL(ob_create_row(allocator, exprs.count(), dml_rtdef.check_row_))) {
      LOG_WARN("create current row failed", K(ret));
    }
  }
  for (int i = 0; OB_SUCC(ret) && i < exprs.count(); ++i) {
    ObDatum *datum = nullptr;
    if (OB_FAIL(exprs.at(i)->eval(eval_ctx, datum))) {
      LOG_WARN("get column datum failed", K(ret));
    } else if (OB_FAIL(datum->to_obj(dml_rtdef.check_row_->cells_[i], exprs.at(i)->obj_meta_))) {
      LOG_WARN("expr datum to current row failed", K(ret));
    }
  }
  return ret;
}

bool ObDMLService::is_nested_dup_table(const uint64_t table_id,  DASDelCtxList& del_ctx_list)
{
  bool ret = false;
  DASDelCtxList::iterator iter = del_ctx_list.begin();
  for (; !ret && iter != del_ctx_list.end(); iter++) {
    DmlRowkeyDistCtx del_ctx = *iter;
    if (del_ctx.table_id_ == table_id) {
      ret = true;
    }
  }
  return ret;
}

int ObDMLService::get_nested_dup_table_ctx(const uint64_t table_id,  DASDelCtxList& del_ctx_list, SeRowkeyDistCtx* &rowkey_dist_ctx)
{
  int ret = OB_SUCCESS;
  bool find = false;
  DASDelCtxList::iterator iter = del_ctx_list.begin();
  for (; !find && iter != del_ctx_list.end(); iter++) {
    DmlRowkeyDistCtx del_ctx = *iter;
    if (del_ctx.table_id_ == table_id) {
      find = true;
      rowkey_dist_ctx = del_ctx.deleted_rows_;
    }
  }
  return ret;
}
}  // namespace sql
}  // namespace oceanbase
