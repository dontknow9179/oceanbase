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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "lib/utility/utility.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/rewrite/ob_transform_utils.h"
#include "sql/optimizer/ob_logical_operator.h"
#include "sql/parser/parse_malloc.h"
#include "sql/ob_sql_context.h"
#include "sql/rewrite/ob_equal_analysis.h"
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "common/ob_smart_call.h"
using namespace oceanbase::sql;
using namespace oceanbase::common;
using namespace oceanbase::share::schema;

int TransposeItem::InPair::assign(const TransposeItem::InPair &other)
{
  int ret = OB_SUCCESS;
  if (this == &other) {
    //do nothing
  } else if (OB_FAIL(exprs_.assign(other.exprs_))) {
    LOG_WARN("assign searray failed", K(other), K(ret));
  } else if (OB_FAIL(column_names_.assign(other.column_names_))) {
    LOG_WARN("assign searray failed", K(other), K(ret));
  } else {
    pivot_expr_alias_ = other.pivot_expr_alias_;
  }
  return ret;
}

int TransposeItem::assign(const TransposeItem &other)
{
  int ret = OB_SUCCESS;
  if (this == &other) {
    //do nothing
  } else if (OB_FAIL(for_columns_.assign(other.for_columns_))) {
    LOG_WARN("assign searray failed", K(other), K(ret));
  } else if (OB_FAIL(in_pairs_.assign(other.in_pairs_))) {
    LOG_WARN("assign searray failed", K(other), K(ret));
  } else if (OB_FAIL(unpivot_columns_.assign(other.unpivot_columns_))) {
    LOG_WARN("assign searray failed", K(other), K(ret));
  } else {
    aggr_pairs_.reset();
    old_column_count_ = other.old_column_count_;
    is_unpivot_ = other.is_unpivot_;
    is_incude_null_ = other.is_incude_null_;
    alias_name_ = other.alias_name_;
  }
  return ret;
}

int TransposeItem::deep_copy(ObIRawExprCopier &expr_copier,
                             const TransposeItem &other)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(assign(other))) {
    LOG_WARN("assign failed", K(other), K(ret));
  }

  for (int64_t i = 0; i < in_pairs_.count() && OB_SUCC(ret); ++i) {
    InPair &in_pair = in_pairs_.at(i);
    in_pair.exprs_.reuse();
    if (OB_FAIL(expr_copier.copy(other.in_pairs_.at(i).exprs_,
                                 in_pair.exprs_))) {
      LOG_WARN("deep copy expr failed", K(ret));
    }
  }
  return ret;
}

OB_SERIALIZE_MEMBER(ObUnpivotInfo,
                    old_column_count_,
                    unpivot_column_count_,
                    for_column_count_,
                    is_include_null_);

int SemiInfo::deep_copy(ObIRawExprCopier &expr_copier, const SemiInfo &other)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(left_table_ids_.assign(other.left_table_ids_))) {
    LOG_WARN("failed to assign left table ids", K(ret));
  } else if (OB_FAIL(expr_copier.copy(other.semi_conditions_,
                                      semi_conditions_))) {
    LOG_WARN("failed to copy semi condition exprs", K(ret));
  } else {
    join_type_ = other.join_type_;
    semi_id_ = other.semi_id_;
    right_table_id_ = other.right_table_id_;
  }
  return ret;
}

int FromItem::deep_copy(const FromItem &other)
{
  int ret = OB_SUCCESS;
  table_id_ = other.table_id_;
  link_table_id_ = other.link_table_id_;
  is_joined_ = other.is_joined_;
  return ret;
}

bool JoinedTable::same_as(const JoinedTable &other) const
{
  bool bret = true;
  if (TableItem::JOINED_TABLE != type_
      || TableItem::JOINED_TABLE != other.type_
      || NULL == left_table_
      || NULL == right_table_
      || NULL == other.left_table_
      || NULL == other.right_table_) {
    bret = false;
  } else {
    if (table_id_ != other.table_id_
        || joined_type_ != other.joined_type_
        || join_conditions_.count() != other.join_conditions_.count()) {
      bret = false;
    } else if (left_table_->type_ != other.left_table_->type_
               || right_table_->type_ != other.right_table_->type_) {
      bret = false;
    } else if (TableItem::JOINED_TABLE == left_table_->type_) {
      const JoinedTable *left_table = static_cast<JoinedTable*>(left_table_);
      const JoinedTable *other_left_table = static_cast<JoinedTable*>(other.left_table_);
      bret = left_table->same_as(*other_left_table);
    } else {
      if (left_table_->table_id_ != other.left_table_->table_id_) {
        bret = false;
      }
    }
  }
  if (true == bret) {
    if (TableItem::JOINED_TABLE == right_table_->type_) {
      const JoinedTable *right_table = static_cast<JoinedTable*>(right_table_);
      const JoinedTable *other_right_table = static_cast<JoinedTable*>(other.right_table_);
      bret = right_table->same_as(*other_right_table);
      bret = right_table->same_as(*other_right_table);
    } else if (right_table_->table_id_ != other.right_table_->table_id_) {
      bret = false;
    }
  }
  if (true == bret) {
    for (int64_t i = 0; bret && i < join_conditions_.count(); ++i) {
      const ObRawExpr *join_condition = join_conditions_.at(i);
      const ObRawExpr *other_join_condition = other.join_conditions_.at(i);
      if (NULL == join_condition || NULL == other_join_condition) {
        bret = false;
      } else {
        bret = join_condition->same_as(*other_join_condition);
      }
    }
  }
  return bret;
}

int ColumnItem::deep_copy(ObIRawExprCopier &expr_copier,
                          const ColumnItem &other)
{
  int ret = OB_SUCCESS;
  column_id_ = other.column_id_;
  table_id_ = other.table_id_;
  column_name_ = other.column_name_;
  auto_filled_timestamp_ = other.auto_filled_timestamp_;
  base_tid_ = other.base_tid_;
  base_cid_ = other.base_cid_;
  ObRawExpr *temp_expr = NULL;
  if (OB_FAIL(expr_copier.copy(other.expr_, temp_expr))) {
    LOG_WARN("failed to copy column expr", K(ret));
  } else if (OB_ISNULL(temp_expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("null expr", K(ret));
  } else if (OB_UNLIKELY(ObRawExpr::EXPR_COLUMN_REF != temp_expr->get_expr_class())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected expr class", K(temp_expr->get_expr_class()), K(ret));
  } else {
    expr_ = static_cast<ObColumnRefRawExpr*>(temp_expr);
  }
  return ret;
}

int TableItem::deep_copy(ObIRawExprCopier &expr_copier,
                         const TableItem &other)
{
  int ret = OB_SUCCESS;
  table_id_ = other.table_id_;
  table_name_ = other.table_name_;
  alias_name_ = other.alias_name_;
  synonym_name_ = other.synonym_name_;
  synonym_db_name_ = other.synonym_db_name_;
  qb_name_ = other.qb_name_;
  type_ = other.type_;
  ref_id_ = other.ref_id_;
  is_system_table_ = other.is_system_table_;
  is_index_table_ = other.is_index_table_;
  is_view_table_ = other.is_view_table_;
  is_recursive_union_fake_table_ = other.is_recursive_union_fake_table_;
  cte_type_ = other.cte_type_;
  database_name_ = other.database_name_;
  for_update_ = other.for_update_;
  for_update_wait_us_ = other.for_update_wait_us_;
  skip_locked_ = other.skip_locked_;
  mock_id_ = other.mock_id_;
  node_ = other.node_; // should deep copy ? seems to be unnecessary
  flashback_query_type_ = other.flashback_query_type_;
  // dblink
  dblink_id_ = other.dblink_id_;
  dblink_name_ = other.dblink_name_;
  link_database_name_ = other.link_database_name_;
  // ddl related
  ddl_schema_version_ = other.ddl_schema_version_;
  ddl_table_id_ = other.ddl_table_id_;
  ref_query_ = other.ref_query_;
  if (OB_FAIL(expr_copier.copy(other.flashback_query_expr_,
                               flashback_query_expr_))) {
    LOG_WARN("failed to deep copy raw expr", K(ret));
  } else if (OB_FAIL(expr_copier.copy(other.function_table_expr_,
                                      function_table_expr_))) {
    LOG_WARN("failed to copy function table expr", K(ret));
  } else if (OB_FAIL(part_ids_.assign(other.part_ids_))) {
    LOG_WARN("failed to assign part ids", K(ret));
  } else if (OB_FAIL(part_names_.assign(other.part_names_))) {
    LOG_WARN("failed to assign part names", K(ret));
  }
  return ret;
}

int JoinedTable::deep_copy(ObIAllocator &allocator,
                           ObIRawExprCopier &expr_copier,
                           const JoinedTable &other)
{
  int ret = OB_SUCCESS;
  joined_type_ = other.joined_type_;
  if (OB_ISNULL(other.left_table_) || OB_ISNULL(other.right_table_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("null table item", K(other.left_table_), K(other.right_table_), K(ret));
  } else if (OB_FAIL(TableItem::deep_copy(expr_copier, other))) {
    LOG_WARN("deep copy table item failed", K(ret));
  } else if (OB_FAIL(single_table_ids_.assign(other.single_table_ids_))) {
    LOG_WARN("failed to assign single table ids", K(ret));
  } else if (OB_FAIL(expr_copier.copy(other.join_conditions_, join_conditions_))) {
    LOG_WARN("failed to copy join condition exprs", K(ret));
  } else {
    left_table_ = other.left_table_;
    right_table_ = other.right_table_;
  }
  if (OB_SUCC(ret) && left_table_->is_joined_table()) {
    JoinedTable *tmp_left = NULL;
    void *ptr = NULL;
    if (OB_ISNULL(ptr = allocator.alloc(sizeof(JoinedTable)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate memory for joined table", K(ret));
    } else {
      tmp_left = new (ptr) JoinedTable();
      if (OB_FAIL(tmp_left->deep_copy(allocator,
                                      expr_copier,
                                      static_cast<JoinedTable &>(*left_table_)))) {
        LOG_WARN("failed to deep copy left table", K(ret));
      } else {
        left_table_ = tmp_left;
      }
    }
  }
  if (OB_SUCC(ret) && right_table_->is_joined_table()) {
    JoinedTable *tmp_right = NULL;
    void *ptr = NULL;
    if (OB_ISNULL(ptr = allocator.alloc(sizeof(JoinedTable)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate memory for joined table", K(ret));
    } else {
      tmp_right = new (ptr) JoinedTable();
      if (OB_FAIL(tmp_right->deep_copy(allocator,
                                       expr_copier,
                                       static_cast<JoinedTable &>(*right_table_)))) {
        LOG_WARN("failed to deep copy right table", K(ret));
      } else {
        right_table_ = tmp_right;
      }
    }
  }
  return ret;
}

int ObDMLStmt::PartExprItem::deep_copy(ObIRawExprCopier &expr_copier,
                                       const PartExprItem &other)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(expr_copier.copy(other.part_expr_, part_expr_))) {
    LOG_WARN("failed to copy part expr", K(ret));
  } else if (OB_FAIL(expr_copier.copy(other.subpart_expr_, subpart_expr_))) {
    LOG_WARN("failed to copy subpart expr", K(ret));
  } else {
    table_id_ = other.table_id_;
    index_tid_ = other.index_tid_;
  }
  return ret;
}

ObDMLStmt::ObDMLStmt(stmt::StmtType type)
    : ObStmt(type),
      order_items_(),
      limit_count_expr_(NULL),
      limit_offset_expr_(NULL),
      limit_percent_expr_(NULL),
      has_fetch_(false),
      is_fetch_with_ties_(false),
      from_items_(),
      part_expr_items_(),
      joined_tables_(),
      stmt_hint_(),
      semi_infos_(),
      autoinc_params_(),
      is_calc_found_rows_(false),
      has_top_limit_(false),
      is_contains_assignment_(false),
      affected_last_insert_id_(false),
      has_part_key_sequence_(false),
      table_items_(),
      column_items_(),
      condition_exprs_(),
      deduced_exprs_(),
      pseudo_column_like_exprs_(),
      tables_hash_(),
      parent_namespace_stmt_(NULL),
      current_level_(0),
      subquery_exprs_(),
      transpose_item_(NULL),
      user_var_exprs_(),
      check_constraint_items_()
{
}

ObDMLStmt::~ObDMLStmt()
{
}

// tables come from table_items_
int ObDMLStmt::remove_from_item(ObIArray<TableItem*> &tables)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < tables.count(); i++) {
    if (OB_ISNULL(tables.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null", K(ret), K(tables));
    } else {
      ret = remove_from_item(tables.at(i)->table_id_);
    }
  }
  return ret;
}

int ObDMLStmt::remove_from_item(uint64_t tid)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < from_items_.count(); i++) {
    if (tid == from_items_.at(i).table_id_) {
      if (OB_FAIL(from_items_.remove(i))) {
        LOG_WARN("failed to remove from_items", K(ret));
      }
      break;
    }
  }
  return ret;
}

int ObDMLStmt::remove_semi_info(SemiInfo* info)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < semi_infos_.count(); i++) {
    if (info == semi_infos_.at(i)) {
      if (OB_FAIL(semi_infos_.remove(i))) {
        LOG_WARN("failed to remove from_items", K(ret));
      }
      break;
    }
  }
  return ret;
}

// ref_query in ObStmt only do pointer copy
int ObDMLStmt::assign(const ObDMLStmt &other)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObStmt::assign(other))) {
    LOG_WARN("failed to copy stmt", K(ret));
  } else if (OB_FAIL(table_items_.assign(other.table_items_))) {
    LOG_WARN("assign table items failed", K(ret));
  } else if (OB_FAIL(tables_hash_.assign(other.tables_hash_))) {
    LOG_WARN("assign table hash desc failed", K(ret));
  } else if (OB_FAIL(column_items_.assign(other.column_items_))) {
    LOG_WARN("assign column items failed", K(ret));
  } else if (OB_FAIL(condition_exprs_.assign(other.condition_exprs_))) {
    LOG_WARN("assign condition exprs failed", K(ret));
  } else if (OB_FAIL(deduced_exprs_.assign(other.deduced_exprs_))) {
    LOG_WARN("assign deduced exprs failed", K(ret));
  } else if (OB_FAIL(order_items_.assign(other.order_items_))) {
    LOG_WARN("assign order items failed", K(ret));
  } else if (OB_FAIL(from_items_.assign(other.from_items_))) {
    LOG_WARN("assign from items failed", K(ret));
  } else if (OB_FAIL(part_expr_items_.assign(other.part_expr_items_))) {
    LOG_WARN("assign part expr items failed", K(ret));
  } else if (OB_FAIL(joined_tables_.assign(other.joined_tables_))) {
    LOG_WARN("assign joined tables failed", K(ret));
  } else if (OB_FAIL(semi_infos_.assign(other.semi_infos_))) {
    LOG_WARN("assign semi infos failed", K(ret));
  } else if (OB_FAIL(stmt_hint_.assign(other.stmt_hint_))) {
    LOG_WARN("assign stmt hint failed", K(ret));
  } else if (OB_FAIL(subquery_exprs_.assign(other.subquery_exprs_))) {
    LOG_WARN("assign subquery exprs failed", K(ret));
  } else if (OB_FAIL(onetime_exprs_.assign(other.onetime_exprs_))) {
    LOG_WARN("assign onetime exprs failed", K(ret));
  } else if (OB_FAIL(pseudo_column_like_exprs_.assign(other.pseudo_column_like_exprs_))) {
    LOG_WARN("assgin pseudo column exprs fail", K(ret));
  } else if (OB_FAIL(autoinc_params_.assign(other.autoinc_params_))) {
    LOG_WARN("assign autoinc params fail", K(ret));
  } else if (OB_FAIL(nextval_sequence_ids_.assign(other.nextval_sequence_ids_))) {
    LOG_WARN("failed to assign sequence ids", K(ret));
  } else if (OB_FAIL(currval_sequence_ids_.assign(other.currval_sequence_ids_))) {
    LOG_WARN("failed to assign sequence ids", K(ret));
  } else if (OB_FAIL(user_var_exprs_.assign(other.user_var_exprs_))) {
    LOG_WARN("assign user var exprs fail", K(ret));
  } else if (OB_FAIL(check_constraint_items_.assign(other.check_constraint_items_))) {
    LOG_WARN("faield to assign check constraint items", K(ret));
  } else {
    parent_namespace_stmt_ = other.parent_namespace_stmt_;
    limit_count_expr_ = other.limit_count_expr_;
    limit_offset_expr_ = other.limit_offset_expr_;
    limit_percent_expr_ = other.limit_percent_expr_;
    has_fetch_ = other.has_fetch_;
    is_fetch_with_ties_ = other.is_fetch_with_ties_;
    current_level_ = other.current_level_;
    is_calc_found_rows_ = other.is_calc_found_rows_;
    has_top_limit_ = other.has_top_limit_;
    is_contains_assignment_ = other.is_contains_assignment_;
    affected_last_insert_id_ = other.affected_last_insert_id_;
    has_part_key_sequence_ = other.has_part_key_sequence_;
    transpose_item_ = other.transpose_item_;
  }
  return ret;
}

int ObDMLStmt::deep_copy(ObStmtFactory &stmt_factory,
                         ObRawExprFactory &expr_factory,
                         const ObDMLStmt &other)
{
  int ret = OB_SUCCESS;
  ObRawExprCopier expr_copier(expr_factory);
  if (OB_FAIL(deep_copy(stmt_factory,
                        expr_copier,
                        other))) {
    LOG_WARN("failed to deep copy stmt", K(ret));
  }
  return ret;
}


int ObDMLStmt::deep_copy(ObStmtFactory &stmt_factory,
                         ObRawExprCopier &expr_copier,
                         const ObDMLStmt &other)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObSelectStmt *, 4> orgi_child_stmts;
  if (OB_UNLIKELY(other.get_stmt_type() != get_stmt_type())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("stmt type does not match", K(ret));
  } else if (OB_FAIL(deep_copy_stmt_struct(stmt_factory.get_allocator(),
                                           expr_copier,
                                           other))) {
    LOG_WARN("failed to deep copy stmt struct", K(ret));
  } else if (OB_FAIL(get_child_stmts(orgi_child_stmts))) {
    LOG_WARN("failed to get child stmts", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < orgi_child_stmts.count(); ++i) {
    ObSelectStmt *copied_child_stmt = NULL;
    if (OB_ISNULL(orgi_child_stmts.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("child stmt is null", K(ret));
    } else if (OB_FAIL(stmt_factory.create_stmt(copied_child_stmt))) {
      LOG_WARN("failed to create child stmt", K(ret));
    } else if (OB_ISNULL(copied_child_stmt)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("child stmt is not created", K(ret));
    } else if (OB_FAIL(SMART_CALL(copied_child_stmt->deep_copy(
                                    stmt_factory,
                                    expr_copier,
                                    *orgi_child_stmts.at(i))))) {
      LOG_WARN("failed to deep copy stmt struct", K(ret));
    } else if (OB_FAIL(set_child_stmt(i, copied_child_stmt))) {
      LOG_WARN("failed to set child stmt", K(ret));
    }
  }
  return ret;
}

int ObDMLStmt::deep_copy_stmt_struct(ObIAllocator &allocator,
                                     ObRawExprCopier &expr_copier,
                                     const ObDMLStmt &other)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(get_stmt_type() != other.get_stmt_type())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("stmt type does not match", K(ret), K(get_stmt_type()), K(other.get_stmt_type()));
  } else if (OB_FAIL(ObStmt::deep_copy(other))) {
    LOG_WARN("failed to copy stmt", K(ret));
  } else if (OB_FAIL(deep_copy_stmt_objects<TableItem>(allocator,
                                                       expr_copier,
                                                       other.table_items_,
                                                       table_items_))) {
    LOG_WARN("failed to deep copy table items", K(ret));
  } else if (OB_FAIL(tables_hash_.assign(other.tables_hash_))) {
    LOG_WARN("assign table hash desc failed", K(ret));
  } else if (OB_FAIL(deep_copy_join_tables(allocator, expr_copier, other))) {
    LOG_WARN("failed to copy joined tables", K(ret));
  } else if (OB_FAIL(deep_copy_stmt_objects<SemiInfo>(allocator,
                                                      expr_copier,
                                                      other.semi_infos_,
                                                      semi_infos_))) {
    LOG_WARN("deep copy semi infos failed", K(ret));
  } else if (OB_FAIL(deep_copy_stmt_objects<ColumnItem>(expr_copier,
                                                        other.column_items_,
                                                        column_items_))) {
    LOG_WARN("deep copy column items failed", K(ret));
  } else if (OB_FAIL(expr_copier.copy(other.subquery_exprs_,
                                      subquery_exprs_))) {
    LOG_WARN("failed to copy subquery exprs", K(ret));
  } else if (OB_FAIL(expr_copier.copy(other.onetime_exprs_,
                                      onetime_exprs_))) {
    LOG_WARN("failed to copy onetime exprs", K(ret));
  } else if (OB_FAIL(expr_copier.copy(other.pseudo_column_like_exprs_,
                                      pseudo_column_like_exprs_))) {
    LOG_WARN("failed to copy pseudo column like exprs", K(ret));
  } else if (OB_FAIL(expr_copier.copy(other.condition_exprs_,
                                      condition_exprs_))) {
    LOG_WARN("failed to copy condition exprs", K(ret));
  } else if (OB_FAIL(expr_copier.copy(other.deduced_exprs_,
                                      deduced_exprs_))) {
    LOG_WARN("failed to copy deduced exprs", K(ret));
  } else if (OB_FAIL(deep_copy_stmt_objects<PartExprItem>(expr_copier,
                                                          other.part_expr_items_,
                                                          part_expr_items_))) {
    LOG_WARN("failed to deep copy part expr items", K(ret));
  } else if (OB_FAIL(expr_copier.copy(other.limit_count_expr_,
                                      limit_count_expr_))) {
    LOG_WARN("deep copy limit count expr failed", K(ret));
  } else if (OB_FAIL(expr_copier.copy(other.limit_offset_expr_,
                                      limit_offset_expr_))) {
    LOG_WARN("deep copy limit offset expr failed", K(ret));
  } else if (OB_FAIL(expr_copier.copy(other.limit_percent_expr_,
                                      limit_percent_expr_))) {
    LOG_WARN("deep copy limit percent expr failed", K(ret));
  } else if (OB_FAIL(expr_copier.copy(other.user_var_exprs_,
                                      user_var_exprs_))) {
    LOG_WARN("deep copy user var exprs failed", K(ret));
  } else if (OB_FAIL(deep_copy_stmt_objects<CheckConstraintItem>(expr_copier,
                                                                 other.check_constraint_items_,
                                                                 check_constraint_items_))) {
    LOG_WARN("failed to deep copy related part expr arrays", K(ret));
  } else if (OB_FAIL(from_items_.assign(other.from_items_))) {
    LOG_WARN("assign from items failed", K(ret));
  } else if (OB_FAIL(stmt_hint_.assign(other.stmt_hint_))) {
    LOG_WARN("assign stmt hint failed", K(ret));
  } else if (OB_FAIL(autoinc_params_.assign(other.autoinc_params_))) {
    LOG_WARN("assign autoinc params failed", K(ret));
  } else if (OB_FAIL(nextval_sequence_ids_.assign(other.nextval_sequence_ids_))) {
    LOG_WARN("failed to assign sequence ids", K(ret));
  } else if (OB_FAIL(currval_sequence_ids_.assign(other.currval_sequence_ids_))) {
    LOG_WARN("failed to assign sequence ids", K(ret));
  } else {
    parent_namespace_stmt_ = other.parent_namespace_stmt_;
    current_level_ = other.current_level_;
    is_calc_found_rows_ = other.is_calc_found_rows_;
    has_top_limit_ = other.has_top_limit_;
    is_contains_assignment_ = other.is_contains_assignment_;
    affected_last_insert_id_ = other.affected_last_insert_id_;
    has_part_key_sequence_ = other.has_part_key_sequence_;
    has_fetch_ = other.has_fetch_;
    is_fetch_with_ties_ = other.is_fetch_with_ties_;
  }
  if (OB_SUCC(ret)) {
    TransposeItem *tmp = NULL;
    if (OB_FAIL(deep_copy_stmt_object<TransposeItem>(allocator,
                                                     expr_copier,
                                                     other.transpose_item_,
                                                     tmp))) {
      LOG_WARN("failed to deep copy transpose item", K(ret));
    } else {
      transpose_item_ = tmp;
    }
  }
  return ret;
}

int ObDMLStmt::get_child_table_id_recurseive(
    common::ObIArray<share::schema::ObObjectStruct> &object_ids,
    const int64_t object_limit_count/*OB_MAX_TABLE_NUM_PER_STMT*/) const
{
  int ret = OB_SUCCESS;
  bool is_stack_overflow = false;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("check stack overflow failed", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("too deep recursive", K(ret));
  }

  bool is_finish = false;
  for (int64_t i = 0; OB_SUCC(ret) && !is_finish && i < get_table_size(); ++i) {
    const sql::TableItem *tmp_table = NULL;
    if (OB_ISNULL(tmp_table = get_table_item(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table item is null", K(ret));
    } else if (tmp_table->is_basic_table()) {
      ObObjectStruct tmp_struct(share::schema::ObObjectType::TABLE, tmp_table->ref_id_);
      if (OB_FAIL(object_ids.push_back(tmp_struct))) {
        LOG_WARN("failed to push_back tmp_table->ref_id_", KPC(tmp_table), K(ret));
      } else if (OB_UNLIKELY(object_ids.count() >= object_limit_count)) {
        is_finish = true;
        LOG_DEBUG("arrieve limit count", KPC(this), K(object_limit_count));
      } else {
        LOG_DEBUG("succ push object_ids", KPC(tmp_table));
      }
    }
  }

  //try complex table
  if (OB_SUCC(ret) && !is_finish && get_table_size() != object_ids.count()) {
    ObArray<ObSelectStmt*> child_stmts;
    if (OB_FAIL(get_child_stmts(child_stmts))) {
      LOG_WARN("get child stmt failed", K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < child_stmts.count(); ++i) {
        const ObSelectStmt *sub_stmt = child_stmts.at(i);
        if (OB_ISNULL(sub_stmt)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("Sub-stmt is NULL", K(ret));
        } else if (OB_FAIL(SMART_CALL(sub_stmt->get_child_table_id_recurseive(object_ids,
                                                                            object_limit_count)))) {
          LOG_WARN("failed to get_child_table_id_with_cte_", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObDMLStmt::get_child_table_id_count_recurseive(
    int64_t &object_ids_cnt, const int64_t object_limit_count/*OB_MAX_TABLE_NUM_PER_STMT*/) const
{
  int ret = OB_SUCCESS;
  bool is_stack_overflow = false;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("check stack overflow failed", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("too deep recursive", K(ret));
  }

  bool is_finish = false;
  for (int64_t i = 0; OB_SUCC(ret) && !is_finish && i < get_table_size(); ++i) {
    const sql::TableItem *tmp_table = NULL;
    if (OB_ISNULL(tmp_table = get_table_item(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table item is null", K(ret));
    } else if (tmp_table->is_basic_table()) {
      if (OB_UNLIKELY(++object_ids_cnt >= object_limit_count)) {
        is_finish = true;
        LOG_DEBUG("arrieve limit count", KPC(tmp_table), K(object_limit_count), K(object_ids_cnt));
      }
    }
  }

  //try complex table
  if (OB_SUCC(ret) && !is_finish && get_table_size() != object_ids_cnt) {
    ObArray<ObSelectStmt*> child_stmts;
    if (OB_FAIL(get_child_stmts(child_stmts))) {
      LOG_WARN("get child stmt failed", K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < child_stmts.count(); ++i) {
        const ObSelectStmt *sub_stmt = child_stmts.at(i);
        if (OB_ISNULL(sub_stmt)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("Sub-stmt is NULL", K(ret));
        } else if (OB_FAIL(SMART_CALL(sub_stmt->get_child_table_id_count_recurseive(object_ids_cnt,
                                                                             object_limit_count)))) {
          LOG_WARN("failed to get_child_table_id_count_recurseive", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObDMLStmt::extract_column_expr(const ObIArray<ColumnItem> &column_items,
                                   ObIArray<ObColumnRefRawExpr*> &column_exprs) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_items.count(); i++) {
    if (OB_ISNULL(column_items.at(i).expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("null column expr", K(ret));
    } else if (OB_FAIL(column_exprs.push_back(column_items.at(i).expr_))) {
      LOG_WARN("failed to push back column expr", K(ret));
    } else { /*do nothing*/ }
  }
  return ret;
}

int ObDMLStmt::copy_and_replace_stmt_expr(ObRawExprCopier &copier)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObRawExprPointer, 4> relation_pointers;
  if (OB_FAIL(get_relation_exprs(relation_pointers))) {
    LOG_WARN("failed to get relation pointers", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < relation_pointers.count(); ++i) {
    ObRawExprPointer &expr_ptr = relation_pointers.at(i);
    ObRawExpr *expr = NULL;
    if (OB_FAIL(expr_ptr.get(expr))) {
      LOG_WARN("faield to get expr", K(ret), K(expr));
    } else if (OB_FAIL(copier.copy_on_replace(expr, expr))) {
      LOG_WARN("failed to copy on replace expr", K(ret));
    } else if (OB_FAIL(expr_ptr.set(expr))) {
      LOG_WARN("failed to update expr", K(ret));
    }
  }
  return ret;
}

int ObDMLStmt::replace_inner_stmt_expr(const ObIArray<ObRawExpr *> &other_exprs,
                                       const ObIArray<ObRawExpr *> &new_exprs)
{
  int ret = OB_SUCCESS;
  // replace order items
  for (int64_t i = 0; OB_SUCC(ret) && i < order_items_.count(); i++) {
    if (OB_ISNULL(order_items_.at(i).expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("null expr", K(ret));
    } else if (OB_FAIL(ObTransformUtils::replace_expr(other_exprs,
                                                      new_exprs,
                                                      order_items_.at(i).expr_))) {
      LOG_WARN("failed to replace column expr", K(ret));
    } else { /*do nothing*/ }
  }
  // replace table items
  for (int64_t i = 0; OB_SUCC(ret) && i < table_items_.count(); i++) {
    if (OB_ISNULL(table_items_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (NULL != table_items_.at(i)->function_table_expr_ &&
               OB_FAIL(ObTransformUtils::replace_expr(other_exprs,
                                                      new_exprs,
                                                      table_items_.at(i)->function_table_expr_))) {
      LOG_WARN("failed to replace expr", K(ret));
    } else { /*do nothing*/ }
  }
  // replace join table items
  for (int64_t i = 0; OB_SUCC(ret) && i < joined_tables_.count(); i++) {
    if (OB_ISNULL(joined_tables_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("null join tables", K(ret));
    } else if (OB_FAIL(replace_expr_for_joined_table(other_exprs,
                                                     new_exprs,
                                                     *joined_tables_.at(i)))) {
      LOG_WARN("failed to replace join condition exprs", K(ret));
    } else { /*do nothing*/ }
  }
  // replace semi info
  for (int64_t i = 0; OB_SUCC(ret) && i < semi_infos_.count(); i++) {
    if (OB_ISNULL(semi_infos_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("null semi info", K(ret));
    } else if (OB_FAIL(ObTransformUtils::replace_exprs(other_exprs, new_exprs,
                                                       semi_infos_.at(i)->semi_conditions_))) {
      LOG_WARN("failed to replace semi conditions exprs", K(ret));
    }
  }
  // replace function table expr
  for (int64_t i = 0; OB_SUCC(ret) && i < get_table_items().count(); ++i) {
    TableItem *table = get_table_item(i);
    if (OB_ISNULL(table)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table item is null", K(ret));
    } else if (!table->is_function_table()) {
      //do nothing
    } else if (OB_FAIL(ObTransformUtils::replace_expr(other_exprs, new_exprs,
                                                      table->function_table_expr_))) {
      LOG_WARN("failed to replace function table exprs", K(ret));
    } else { /* Do nothing */ }
  }
  // replace other exprs
  if (OB_SUCC(ret)) {
    if (OB_FAIL(ObTransformUtils::replace_exprs(other_exprs,
                                                new_exprs,
                                                condition_exprs_))) {
      LOG_WARN("failed to replace condition exprs", K(ret));
    } else if (OB_FAIL(ObTransformUtils::replace_exprs(other_exprs,
                                                       new_exprs,
                                                       deduced_exprs_))) {
      LOG_WARN("failed to replace deduced exprs", K(ret));
    } else if (OB_FAIL(ObTransformUtils::replace_expr(other_exprs,
                                                      new_exprs,
                                                      limit_count_expr_))) {
      LOG_WARN("failed to replace column expr", K(ret));
    } else if (OB_FAIL(ObTransformUtils::replace_expr(other_exprs,
                                                      new_exprs,
                                                      limit_offset_expr_))) {
      LOG_WARN("failed to replace column expr", K(ret));
    } else if (OB_FAIL(ObTransformUtils::replace_expr(other_exprs,
                                                      new_exprs,
                                                      limit_percent_expr_))) {
      LOG_WARN("failed to replace column expr", K(ret));
    } else if (OB_FAIL(ObTransformUtils::replace_exprs(other_exprs,
                                                       new_exprs,
                                                       onetime_exprs_))) {
      LOG_WARN("failed to replace onetime exprs", K(ret));
    } else { /*do nothing*/ }
  }
  //replace part expr items
  for (int64_t i = 0; OB_SUCC(ret) && i < part_expr_items_.count(); i++) {
    if (OB_FAIL(ObTransformUtils::replace_expr(other_exprs,
                                               new_exprs,
                                               part_expr_items_.at(i).part_expr_))) {
      LOG_WARN("failed to replace column expr", K(ret));
    } else if (OB_FAIL(ObTransformUtils::replace_expr(other_exprs,
                                                      new_exprs,
                                                      part_expr_items_.at(i).subpart_expr_))) {
      LOG_WARN("failed to replace column expr", K(ret));
    } else { /*do nothing*/ }
  }
  //replace check constraint items
  for (int64_t i = 0; OB_SUCC(ret) && i < check_constraint_items_.count(); i++) {
    if (OB_FAIL(ObTransformUtils::replace_exprs(other_exprs,
                                                new_exprs,
                                                check_constraint_items_.at(i).check_constraint_exprs_))) {
      LOG_WARN("failed to replace column expr", K(ret));
    } else { /*do nothing*/ }
  }
  // replace dependent expr
  for (int64_t i = 0; OB_SUCC(ret) && i < column_items_.count(); i++) {
    if (OB_ISNULL(column_items_.at(i).expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("null column expr", K(ret));
    } else if (NULL != column_items_.at(i).expr_->get_dependant_expr()) {
      ObRawExpr *temp_expr = column_items_.at(i).expr_->get_dependant_expr();
      if (OB_FAIL(ObTransformUtils::replace_expr(other_exprs,
                                                 new_exprs,
                                                 temp_expr))) {
        LOG_WARN("failed to replace expr", K(ret));
      } else {
        column_items_.at(i).expr_->set_dependant_expr(temp_expr);
      }
    }

    if (OB_SUCC(ret) && NULL != column_items_.at(i).default_value_expr_) {
      ObRawExpr *temp_expr = column_items_.at(i).default_value_expr_;
      if (OB_FAIL(ObTransformUtils::replace_expr(other_exprs,
                                                 new_exprs,
                                                 temp_expr))) {
        LOG_WARN("failed to replace expr", K(ret));
      } else {
        column_items_.at(i).default_value_expr_ = temp_expr;
      }
    } else { /*do nothing*/ }
  }

  if (NULL != transpose_item_) {
    for (int64_t i = 0; i < transpose_item_->in_pairs_.count() && OB_SUCC(ret); ++i) {
      TransposeItem::InPair &in_pair = const_cast<TransposeItem::InPair &>(transpose_item_->in_pairs_.at(i));
      for (int64_t j = 0; j < in_pair.exprs_.count() && OB_SUCC(ret); ++j) {
        if (OB_FAIL(ObTransformUtils::replace_expr(other_exprs,
                                                   new_exprs,
                                                   in_pair.exprs_.at(j)))) {
          LOG_WARN("failed to replace expr", K(ret));
        }
      }
    }
  }

  if (OB_SUCC(ret)) {
    ObSEArray<ObSelectStmt *, 4> subqueries;
    if (OB_FAIL(get_child_stmts(subqueries))) {
      LOG_WARN("failed to get subqueries", K(ret));
    }
    // replace exprs in child stmt
    for (int64_t i = 0; OB_SUCC(ret) && i < subqueries.count(); i++) {
      if (OB_ISNULL(subqueries.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null stmt", K(ret));
      } else if (OB_FAIL(SMART_CALL(subqueries.at(i)->replace_inner_stmt_expr(
                                      other_exprs, new_exprs)))) {
        LOG_WARN("failed to repalce expr in child stmt", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::replace_expr_for_joined_table(const ObIArray<ObRawExpr *> &other_exprs,
                                             const ObIArray<ObRawExpr *> &new_exprs,
                                             JoinedTable &joined_table)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(joined_table.left_table_) || OB_ISNULL(joined_table.right_table_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("null table item", K(ret), K(joined_table.left_table_),
        K(joined_table.right_table_));
  } else if (OB_FAIL(ObTransformUtils::replace_exprs(other_exprs,
                                                     new_exprs,
                                                     joined_table.join_conditions_))) {
    LOG_WARN("failed to replace join condition exprs", K(ret));
  } else if (joined_table.left_table_->is_joined_table() &&
             OB_FAIL(SMART_CALL(replace_expr_for_joined_table(other_exprs,
                                          new_exprs,
                                          static_cast<JoinedTable&>(*joined_table.left_table_))))) {
    LOG_WARN("failed to replace expr for joined table", K(ret));
  } else if (joined_table.right_table_->is_joined_table() &&
             OB_FAIL(SMART_CALL(replace_expr_for_joined_table(other_exprs,
                                        new_exprs,
                                        static_cast<JoinedTable&>(*joined_table.right_table_))))) {
    LOG_WARN("failed to replace expr for joined table", K(ret));
  } else { /*do nothing*/ }
  return ret;
}

int ObDMLStmt::deep_copy_join_tables(ObIAllocator &allocator,
                                     ObIRawExprCopier &expr_copier,
                                     const ObDMLStmt &other)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < other.joined_tables_.count(); ++i) {
    JoinedTable *table = NULL;
    void *ptr = NULL;
    if (OB_ISNULL(other.joined_tables_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("joined table is invalid", K(ret));
    } else if (OB_ISNULL(ptr = allocator.alloc(sizeof(JoinedTable)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("falied to allocate memory", K(ret), K(ptr));
    } else {
      table = new (ptr) JoinedTable();
      if (OB_FAIL(table->deep_copy(allocator, expr_copier, *other.joined_tables_.at(i)))) {
        LOG_WARN("faield to deep copy joined table", K(ret));
      } else if (OB_FAIL(construct_join_table(other, *other.joined_tables_.at(i), *table))) {
        LOG_WARN("failed to construct joined table", K(ret));
      } else if (OB_FAIL(joined_tables_.push_back(table))) {
        LOG_WARN("failed to add joined table", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::construct_join_table(const ObDMLStmt &other_stmt,
                                    const JoinedTable &other,
                                    JoinedTable &current)
{
  int ret = OB_SUCCESS;
  int64_t idx_left = -1;
  int64_t idx_right = -1;
  if (OB_UNLIKELY(table_items_.count() != other_stmt.table_items_.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table item count should be the same", K(table_items_.count()),
        K(other_stmt.table_items_.count()), K(ret));
  } else if (OB_ISNULL(other.left_table_) || OB_ISNULL(other.right_table_) ||
             OB_ISNULL(current.left_table_) || OB_ISNULL(current.right_table_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("null table item", K(other.left_table_), K(other.right_table_),
        K(current.left_table_), K(current.right_table_), K(ret));
  } else if (OB_UNLIKELY(other.left_table_->type_ != current.left_table_->type_) ||
             OB_UNLIKELY(other.right_table_->type_ != current.right_table_->type_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("should have the same table item type", K(other.left_table_->type_),
        K(other.right_table_->type_), K(current.left_table_->type_),
        K(current.right_table_->type_), K(ret));
  } else { /*do nothing*/ }
  // replace left table item
  if (OB_SUCC(ret)) {
    if (other.left_table_->is_joined_table()) {
      if (OB_FAIL(SMART_CALL(construct_join_table(other_stmt,
                                               static_cast<JoinedTable&>(*other.left_table_),
                                               static_cast<JoinedTable&>(*current.left_table_))))) {
        LOG_WARN("failed to replace joined table item", K(ret));
      } else { /*do nothing*/}
    } else {
      if (OB_FAIL(other_stmt.get_table_item_idx(other.left_table_, idx_left))
          || (-1 == idx_left)) {
        LOG_WARN("failed to get table item", K(ret), K(idx_left));
      } else {
        current.left_table_ = table_items_.at(idx_left);
      }
    }
  }
  // replace right table item
  if (OB_SUCC(ret)) {
    if (other.right_table_->is_joined_table()) {
      if (OB_FAIL(construct_join_table(other_stmt,
                                       static_cast<JoinedTable&>(*other.right_table_),
                                       static_cast<JoinedTable&>(*current.right_table_)))) {
        LOG_WARN("failed to replace joined table", K(ret));
      } else { /*do nothing*/ }
    } else {
      if  (OB_FAIL(other_stmt.get_table_item_idx(other.right_table_, idx_right))
          || (-1 == idx_right)) {
        LOG_WARN("failed to get table item", K(idx_right), K(ret));
      } else {
        current.right_table_ = table_items_.at(idx_right);
      }
    }
  }
  return ret;
}

/*
 * for or-expansion transformation
 * todo: do not update semi id in semi info now
 */
int ObDMLStmt::update_stmt_table_id(const ObDMLStmt &other)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(other.table_items_.count() != table_items_.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("should have equal table item count",
        K(table_items_.count()), K(other.table_items_.count()), K(ret));
  } else if (OB_UNLIKELY(other.joined_tables_.count() != joined_tables_.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("should have equal joined table count", K(joined_tables_.count()),
        K(other.joined_tables_.count()), K(ret));
  } else if (OB_UNLIKELY(subquery_exprs_.count() != other.subquery_exprs_.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("should have equal subquery exprs", K(subquery_exprs_.count()),
        K(other.subquery_exprs_.count()), K(ret));
  } else { /*do nothing*/ }
  // recursively update table id for child statements
  for (int64_t i = 0; OB_SUCC(ret) && i < table_items_.count(); i++) {
    if (OB_ISNULL(table_items_.at(i)) ||
        OB_ISNULL(other.table_items_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("null table item", K(table_items_.at(i)),
          K(other.table_items_.at(i)), K(ret));
    } else if (table_items_.at(i)->is_generated_table() &&
               other.table_items_.at(i)->is_generated_table() &&
               NULL != table_items_.at(i)->ref_query_ &&
               NULL != other.table_items_.at(i)->ref_query_ &&
               OB_FAIL(table_items_.at(i)->ref_query_->update_stmt_table_id(
                       *other.table_items_.at(i)->ref_query_))) {
      LOG_WARN("failed to update table id for generated table", K(ret));
    } else { /*do nothing*/ }
  }
  //recursively update table id for subquery exprs
  for (int64_t i = 0; OB_SUCC(ret) && i < subquery_exprs_.count(); i++) {
    if (OB_ISNULL(subquery_exprs_.at(i)) ||
        OB_ISNULL(subquery_exprs_.at(i)->get_ref_stmt()) ||
        OB_ISNULL(other.subquery_exprs_.at(i)) ||
        OB_ISNULL(other.subquery_exprs_.at(i)->get_ref_stmt())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("null point error", K(subquery_exprs_.at(i)), K(other.subquery_exprs_.at(i)),
          K(subquery_exprs_.at(i)->get_ref_stmt()), K(other.subquery_exprs_.at(i)->get_ref_stmt()),
          K(ret));
    } else if (OB_FAIL(subquery_exprs_.at(i)->get_ref_stmt()->update_stmt_table_id(
                       *other.subquery_exprs_.at(i)->get_ref_stmt()))) {
      LOG_WARN("failed to update table id for subquery exprs", K(ret));
    } else { /*do nothing*/ }
  }
  // reset tables hash
  if (OB_SUCC(ret)) {
    tables_hash_.reset();
    if (OB_FAIL(set_table_bit_index(common::OB_INVALID_ID))) {
      LOG_WARN("failed to set table bit index", K(ret));
    } else { /*do nothing*/ }
  }
  // reset table id from column items
  if (OB_SUCC(ret) && &other != this) {
    for (int64_t i = 0; OB_SUCC(ret) && i < column_items_.count(); i++) {
      if (OB_ISNULL(column_items_.at(i).expr_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null column expr", K(ret));
      } else {
        column_items_.at(i).expr_->get_relation_ids().reset();
      }
    }
  }
  // update table item id
  for (int64_t i = 0; OB_SUCC(ret) && i < other.table_items_.count(); i++) {
    if (OB_ISNULL(table_items_.at(i)) || OB_ISNULL(other.table_items_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("null table item", K(table_items_.at(i)),
          K(other.table_items_.at(i)), K(ret));
    } else if (OB_FAIL(update_table_item_id(other,
                                            *other.table_items_.at(i),
                                            true,
                                            *table_items_.at(i)))) {
      LOG_WARN("failed to update table id for table item", K(ret));
    } else { /*do nothing*/ }
  }
  // update joined table id
  for (int64_t i = 0; OB_SUCC(ret) && i < other.joined_tables_.count(); i++) {
    if (OB_ISNULL(other.joined_tables_.at(i)) ||
        OB_ISNULL(joined_tables_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("null joined table", K(other.joined_tables_.at(i)),
          K(joined_tables_.at(i)), K(ret));
    } else if (OB_FAIL(update_table_item_id_for_joined_table(other,
                                                             *other.joined_tables_.at(i),
                                                             *joined_tables_.at(i)))) {
      LOG_WARN("failed to update table id for joined table", K(ret));
    } else { /*do nothing*/ }
  }
  return ret;
}

// set empty qb_name of table as cur stmt qb_name
int ObDMLStmt::set_table_item_qb_name()
{
  int ret = OB_SUCCESS;
  ObString qb_name;
  if (OB_FAIL(get_qb_name(qb_name))) {
    LOG_WARN("fail to get qb_name", K(ret), K(get_stmt_id()));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < table_items_.count(); ++i) {
      if (OB_ISNULL(table_items_.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else if (!table_items_.at(i)->qb_name_.empty()) {
        /* do nothing */
      } else {
        table_items_.at(i)->qb_name_ = qb_name;
      }
    }
  }
  return ret;
}

int ObDMLStmt::adjust_qb_name(ObIAllocator *allocator,
                              const ObString &src_qb_name,
                              const ObIArray<uint32_t> &src_hash_val,
                              int64_t *sub_num /* default = NULL */)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(allocator) || OB_ISNULL(get_query_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret), K(allocator), K(get_query_ctx()));
  } else if (OB_FAIL(get_query_ctx()->get_query_hint_for_update().adjust_qb_name_for_stmt(
                                                                      *allocator, *this,
                                                                      src_qb_name,
                                                                      src_hash_val,
                                                                      sub_num))) {
    LOG_WARN("failed to adjust hint for new stmt", K(ret));
  }
  return ret;
}

int ObDMLStmt::adjust_statement_id(ObIAllocator *allocator,
                                   const ObString &src_qb_name,
                                   const ObIArray<uint32_t> &src_hash_val,
                                   int64_t *sub_num /* default = NULL */)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(set_stmt_id())) {
    LOG_WARN("failed to set stmt id", K(ret));
  } else if (OB_FAIL(adjust_qb_name(allocator, src_qb_name, src_hash_val, sub_num))) {
    LOG_WARN("failed to adjust qb name", K(ret));
  }
  return ret;
}

// only used after deep copy stmt and origin stmt both exits in query.
// such as: or expansion / full outer join expand / temp table expand
int ObDMLStmt::recursive_adjust_statement_id(ObIAllocator *allocator,
                                             const ObIArray<uint32_t> &src_hash_val,
                                             int64_t sub_num)

{
  int ret = OB_SUCCESS;
  ObSEArray<ObSelectStmt*, 4> child_stmts;
  ObString cur_qb_name;
  if (OB_FAIL(get_qb_name(cur_qb_name))) {
    LOG_WARN("failed to get qb name", K(ret));
  } else if (OB_FAIL(adjust_statement_id(allocator, cur_qb_name, src_hash_val, &sub_num))) {
    LOG_WARN("fail to adjust statement id", K(ret));
  } else if (OB_FAIL(get_child_stmts(child_stmts))) {
    LOG_WARN("get child stmt failed", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < child_stmts.count(); ++i) {
      if (OB_ISNULL(child_stmts.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_ERROR("sub stmt is null", K(ret));
      } else if (OB_FAIL(SMART_CALL(child_stmts.at(i)->recursive_adjust_statement_id(allocator,
                                                                                     src_hash_val,
                                                                                     sub_num)))) {
        LOG_WARN("fail to adjust statement id", K(ret), K(i));
      }
    }
  }
  return ret;
}

int ObDMLStmt::get_stmt_by_stmt_id(int64_t stmt_id, ObDMLStmt *&stmt)
{
  int ret = OB_SUCCESS;
  stmt = NULL;
  if (get_stmt_id() == stmt_id) {
    stmt = this;
  } else {
    ObSEArray<ObSelectStmt*, 4> child_stmts;
    if (OB_FAIL(get_child_stmts(child_stmts))) {
      LOG_WARN("failed to get child stmts", K(ret));
    }
    for (int64_t i = 0; NULL == stmt && OB_SUCC(ret) && i < child_stmts.count(); i++) {
      if (OB_ISNULL(child_stmts.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else if (OB_FAIL(SMART_CALL(child_stmts.at(i)->get_stmt_by_stmt_id(stmt_id, stmt)))) {
        LOG_WARN("failed to get stmt by stmt id", K(ret));
      }
    }
  }
  // find stmt by stmt id in temp table stmt
  for (int64_t i = 0; NULL == stmt && OB_SUCC(ret) && i < table_items_.count(); i++) {
    if (OB_ISNULL(table_items_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (!table_items_.at(i)->is_temp_table()) {
      /* do nothing */
    } else if (OB_ISNULL(table_items_.at(i)->ref_query_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (OB_FAIL(SMART_CALL(table_items_.at(i)->ref_query_->get_stmt_by_stmt_id(stmt_id,
                                                                                      stmt)))) {
      LOG_WARN("failed to get stmt by stmt id", K(ret));
    }
  }
  return ret;
}

/**
 * @brief ObSelectStmt::adjust_subquery_stmt_parent
 * replace parent_namespace_stmt_ field for all descendants
 */
int ObDMLStmt::adjust_subquery_stmt_parent(const ObDMLStmt *old_parent, ObDMLStmt *new_parent)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObSelectStmt*, 4> subqueries;
  bool is_stack_overflow = false;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("check stack overflow failed", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("stack is overflow", K(ret), K(is_stack_overflow));
  } else if (OB_FAIL(get_child_stmts(subqueries))) {
    LOG_WARN("failed to get child stmt", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < subqueries.count(); ++i) {
    if (OB_ISNULL(subqueries.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("child stmt is null", K(ret), K(i));
    } else if (subqueries.at(i)->parent_namespace_stmt_ != old_parent) {
      // grand_child_stmt's parent can only be child_stmt or child_stmt's parent
      // since child_stmt's parent is not new_parent
      // grand_child_stmt's parent would never be new_parent
    } else if (OB_FAIL(SMART_CALL(subqueries.at(i)->adjust_subquery_stmt_parent(old_parent,
                                                                                new_parent)))) {
      LOG_WARN("failed to adjust stmt parent", K(ret));
    } else {
      subqueries.at(i)->parent_namespace_stmt_ = new_parent;
    }
  }
  return ret;
}

int ObDMLStmt::update_table_item_id_for_joined_table(const ObDMLStmt &other_stmt,
                                                     const JoinedTable &other,
                                                     JoinedTable &current)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(other.left_table_) || OB_ISNULL(other.right_table_) ||
      OB_ISNULL(current.left_table_) || OB_ISNULL(current.right_table_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("null table item", K(other.left_table_), K(other.right_table_),
        K(current.left_table_), K(current.right_table_), K(ret));
  } else if (OB_FAIL(update_table_item_id(other_stmt, other, false, current))) {
    LOG_WARN("failed to update table id", K(ret));
  } else if (other.left_table_->is_joined_table() &&
             current.left_table_->is_joined_table() &&
             OB_FAIL(update_table_item_id_for_joined_table(other_stmt,
                                                           static_cast<JoinedTable&>(*other.left_table_),
                                                           static_cast<JoinedTable&>(*current.left_table_)))) {
    LOG_WARN("failed to update table id", K(ret));
  } else if (other.right_table_->is_joined_table() &&
             current.right_table_->is_joined_table() &&
             OB_FAIL(update_table_item_id_for_joined_table(other_stmt,
                                                           static_cast<JoinedTable&>(*other.right_table_),
                                                           static_cast<JoinedTable&>(*current.right_table_)))) {
    LOG_WARN("failed to update table id", K(ret));
  } else { /*do nothing*/ }
  return ret;
}

int ObDMLStmt::update_table_item_id(const ObDMLStmt &other,
                                    const TableItem &old_item,
                                    const bool has_bit_index,
                                    TableItem &new_item)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(query_ctx_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("null query ctx", K(ret));
  // } else if (OB_FAIL(get_qb_name(new_item.qb_name_))) {
  //   LOG_WARN("fail to get qb_name", K(ret), K(get_stmt_id()));
  // do not update table item qb name
  } else {
    uint64_t old_table_id = old_item.table_id_;
    uint64_t new_table_id = query_ctx_->available_tb_id_--;
    int32_t old_bit_id = OB_INVALID_INDEX;
    int32_t new_bit_id = OB_INVALID_INDEX;
    new_item.table_id_ = new_table_id;
    if (TableItem::TableType::BASE_TABLE == new_item.type_) {
      new_item.type_ = TableItem::TableType::ALIAS_TABLE;
      new_item.alias_name_ = ObString::make_string("");
    }
    if (has_bit_index) {
      if (OB_FAIL(set_table_bit_index(new_table_id))) {
        LOG_WARN("failed to set table bit index", K(ret));
      } else if (&new_item == &old_item) {
        /* do nothing */
      } else if (OB_UNLIKELY(OB_INVALID_INDEX == (new_bit_id = get_table_bit_index(new_table_id)))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to get table index id", K(ret));
      } else if (OB_UNLIKELY(OB_INVALID_INDEX == (old_bit_id = other.get_table_bit_index(old_table_id)))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to get table index id", K(ret));
      } else { /*do nothing*/ }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(ObTransformUtils::update_table_id_for_from_item(other.from_items_,
                                                                  old_table_id, new_table_id,
                                                                  from_items_))) {
        LOG_WARN("failed to update table id for from item", K(ret));
      } else if (OB_FAIL(ObTransformUtils::update_table_id_for_joined_tables(other.joined_tables_,
                                                                             old_table_id, new_table_id,
                                                                             joined_tables_))) {
        LOG_WARN("failed to update table id for joined tables", K(ret));
      } else if (OB_FAIL(ObTransformUtils::update_table_id_for_part_item(other.part_expr_items_,
                                                                         old_table_id, new_table_id,
                                                                         part_expr_items_))) {
        LOG_WARN("failed to update table id for part item", K(ret));
      } else if (OB_FAIL(ObTransformUtils::update_table_id_for_check_constraint_items(other.check_constraint_items_,
                                                                                      old_table_id,
                                                                                      new_table_id,
                                                                                      check_constraint_items_))) {
        LOG_WARN("failed to update table id for part array", K(ret));
      } else if (OB_FAIL(ObTransformUtils::update_table_id_for_semi_info(other.semi_infos_,
                                                                         old_table_id, new_table_id,
                                                                         semi_infos_))) {
        LOG_WARN("failed to update table id for semi_info", K(ret));
      } else if (OB_FAIL(ObTransformUtils::update_table_id_for_column_item(other.column_items_,
                                                                           old_table_id, new_table_id,
                                                                           old_bit_id, new_bit_id,
                                                                           column_items_))) {
        LOG_WARN("failed to update table id for view table id", K(ret));
      } else if (OB_FAIL(ObTransformUtils::update_table_id_for_pseudo_columns(other.pseudo_column_like_exprs_,
                                                                              old_table_id, new_table_id,
                                                                              old_bit_id, new_bit_id,
                                                                              pseudo_column_like_exprs_))) {
        LOG_WARN("failed to update table id for view table id", K(ret));
      } else { /*do nothing*/ }
    }
  }
  return ret;
}

int ObDMLStmt::inner_get_relation_exprs(RelExprCheckerBase &expr_checker)
{
  int ret = OB_SUCCESS;
  // add flashback expr in from scope
  if (!expr_checker.is_ignore(RelExprCheckerBase::FROM_SCOPE)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < table_items_.count(); ++i) {
      if (OB_ISNULL(table_items_.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("table item is null", K(ret));
      } else if (NULL != table_items_.at(i)->flashback_query_expr_ &&
                 OB_FAIL(expr_checker.add_expr(table_items_.at(i)->flashback_query_expr_))) {
        LOG_WARN("failed to add flash back query expr", K(ret));
      } else if (NULL != table_items_.at(i)->function_table_expr_ &&
                 OB_FAIL(expr_checker.add_expr(table_items_.at(i)->function_table_expr_))) {
        LOG_WARN("failed to add function table expr", K(ret));
      }
    }
  }
  //add order exprs
  if (OB_SUCC(ret) && !expr_checker.is_ignore(RelExprCheckerBase::ORDER_SCOPE)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < order_items_.count(); ++i) {
      OrderItem &order_item = order_items_.at(i);
      if (OB_FAIL(expr_checker.add_expr(order_item.expr_))) {
        LOG_WARN("add relation expr failed", K(ret));
      }
    }
  }
  //add join table expr
  if (OB_SUCC(ret) && !expr_checker.is_ignore(RelExprCheckerBase::JOIN_CONDITION_SCOPE)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < joined_tables_.count(); ++i) {
      if (OB_ISNULL(joined_tables_.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null joined table", K(ret));
      } else if (OB_FAIL(get_join_condition_expr(*joined_tables_.at(i), expr_checker))) {
        LOG_WARN("get join table condition expr failed", K(ret));
      }
    }
  }

  //add semi condition expr
  if (OB_SUCC(ret) && !expr_checker.is_ignore(RelExprCheckerBase::JOIN_CONDITION_SCOPE)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < semi_infos_.count(); ++i) {
      if (OB_ISNULL(semi_infos_.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null semi info", K(ret));
      } else if (OB_FAIL(expr_checker.add_exprs((semi_infos_.at(i)->semi_conditions_)))) {
        LOG_WARN("failed to add semi condition exprs", K(ret));
      }
    }
  }
  //add function table expr
  if (OB_SUCC(ret) && !expr_checker.is_ignore(RelExprCheckerBase::JOIN_CONDITION_SCOPE)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < get_table_items().count(); ++i) {
      TableItem *table = get_table_item(i);
      if (OB_ISNULL(table)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("table item is null", K(ret));
      } else if (!table->is_function_table()) {
        //do nothing
      } else if (OB_FAIL(expr_checker.add_expr(table->function_table_expr_))) {
        LOG_WARN("add function table expr failed", K(ret));
      } else { /* Do nothing */ }
    }
  }

  if (OB_SUCC(ret) && !expr_checker.is_ignore(RelExprCheckerBase::LIMIT_SCOPE)) {
    if (limit_count_expr_ != NULL &&
        OB_FAIL(expr_checker.add_expr(limit_count_expr_))) {
      LOG_WARN("add limit count expr failed", K(ret));
    } else if (limit_offset_expr_ != NULL &&
               OB_FAIL(expr_checker.add_expr(limit_offset_expr_))) {
      LOG_WARN("add limit offset expr failed", K(ret));
    } else if (limit_percent_expr_ != NULL &&
               OB_FAIL(expr_checker.add_expr(limit_percent_expr_))) {
      LOG_WARN("add limit percent expr failed", K(ret));
    } else { /*do nothing*/ }
  }

  if (OB_SUCC(ret) && !expr_checker.is_ignore(RelExprCheckerBase::WHERE_SCOPE)) {
    if (OB_FAIL(expr_checker.add_exprs(condition_exprs_))) {
      LOG_WARN("add condition exprs to relation exprs failed", K(ret));
    } else if (OB_FAIL(expr_checker.add_exprs(deduced_exprs_))) {
      LOG_WARN("add deduced exprs to relation exprs failed", K(ret));
    } else { /*do nothing*/ }
  }

  if (OB_SUCC(ret) && !expr_checker.is_ignore(-1)) {
    // add dependant expr of virtual columns if get all relation exprs (has no ignore)
    FOREACH_CNT_X(ci, column_items_, OB_SUCC(ret)) {
      if (NULL != ci->expr_->get_dependant_expr()) {
        OZ(expr_checker.add_expr(ci->expr_->get_dependant_expr()));
      }
    }
  }
  return ret;
}

int ObDMLStmt::get_join_condition_expr(JoinedTable &join_table, RelExprCheckerBase &expr_checker) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(expr_checker.add_exprs(join_table.join_conditions_))) {
    LOG_WARN("add join condition to relexpr checker failed", K(ret));
  } else {
    if (NULL != join_table.left_table_ && join_table.left_table_->is_joined_table()) {
      JoinedTable &left_table = static_cast<JoinedTable&>(*join_table.left_table_);
      if (OB_FAIL(SMART_CALL(get_join_condition_expr(left_table, expr_checker)))) {
        LOG_WARN ("get left join table condition expr failed", K(ret));
      }
    }
    if (OB_SUCC(ret) && join_table.right_table_ != NULL && join_table.right_table_->is_joined_table()) {
      JoinedTable &right_table = static_cast<JoinedTable&>(*join_table.right_table_);
      if (OB_FAIL(SMART_CALL(get_join_condition_expr(right_table, expr_checker)))) {
        LOG_WARN("get right join table condition expr failed", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::replace_expr_in_stmt(ObRawExpr *from, ObRawExpr *to)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObRawExpr*, 1> froms;
  ObSEArray<ObRawExpr*, 1> tos;
  OZ(froms.push_back(from));
  OZ(tos.push_back(to));
  OZ(replace_inner_stmt_expr(froms, tos));
  return ret;
}

int ObDMLStmt::replace_expr_in_joined_table(JoinedTable &joined_table, ObRawExpr *from, ObRawExpr *to)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < joined_table.join_conditions_.count(); ++i) {
    ObRawExpr *&expr = joined_table.join_conditions_.at(i);
    if (expr == from) {
      expr = to;
    }
  }
  if (OB_FAIL(ret)) {
    //do nothing
  } else {
    if (NULL != joined_table.left_table_ && joined_table.left_table_->is_joined_table()) {
      JoinedTable &left_table = static_cast<JoinedTable&>(*joined_table.left_table_);
      if (OB_FAIL(SMART_CALL(replace_expr_in_joined_table(left_table, from, to)))) {
        LOG_WARN ("replace expr in joined table failed", K(ret));
      }
    }
    if (OB_SUCC(ret) && joined_table.right_table_ != NULL && joined_table.right_table_->is_joined_table()) {
      JoinedTable &right_table = static_cast<JoinedTable&>(*joined_table.right_table_);
      if (OB_FAIL(SMART_CALL(replace_expr_in_joined_table(right_table, from, to)))) {
        LOG_WARN("replace expr in joined table failed", K(ret));
      }
    }
  }
  return ret;
}

bool ObDMLStmt::has_subquery() const
{
  return subquery_exprs_.count() > 0;
}

int ObDMLStmt::get_table_function_exprs(ObIArray<ObRawExpr *> &table_func_exprs) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < table_items_.count(); ++i) {
    if (OB_ISNULL(table_items_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table item is null", K(ret));
    } else if (!table_items_.at(i)->is_function_table()) {
      // do nothing
    } else if (OB_FAIL(table_func_exprs.push_back(table_items_.at(i)->function_table_expr_))) {
      LOG_WARN("failed to push back table func expr", K(ret));
    }
  }
  return ret;
}

int ObDMLStmt::remove_part_expr_items(ObIArray<uint64_t> &table_ids)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < table_ids.count(); ++i) {
    ret = remove_part_expr_items(table_ids.at(i));
  }
  return ret;
}

int ObDMLStmt::remove_part_expr_items(uint64_t table_id)
{
  int ret = OB_SUCCESS;
  for (int64_t i = part_expr_items_.count() - 1; OB_SUCC(ret) && i >= 0; --i) {
    if (table_id == part_expr_items_.at(i).table_id_) {
      if (OB_FAIL(part_expr_items_.remove(i))) {
        LOG_WARN("fail to remove part expr item", K(table_id), K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::get_part_expr_items(ObIArray<uint64_t> &table_ids,
                                   ObIArray<PartExprItem> &part_items)
{
  int ret = OB_SUCCESS;
  part_items.reuse();
  for (int64_t i = 0; OB_SUCC(ret) && i < table_ids.count(); ++i) {
    for (int64_t j = 0; OB_SUCC(ret) && j < part_expr_items_.count(); ++j) {
      if (table_ids.at(i) != part_expr_items_.at(j).table_id_) {
        // do nothing
      } else if (OB_FAIL(part_items.push_back(part_expr_items_.at(j)))) {
        LOG_WARN("failed to push back part expr item", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::get_part_expr_items(uint64_t table_id, ObIArray<PartExprItem> &part_items)
{
  int ret = OB_SUCCESS;
  part_items.reuse();
  for (int64_t i = 0; OB_SUCC(ret) && i < part_expr_items_.count(); ++i) {
    if (table_id != part_expr_items_.at(i).table_id_) {
      // do nothing
    } else if (OB_FAIL(part_items.push_back(part_expr_items_.at(i)))) {
      LOG_WARN("failed to push back part expr item", K(ret));
    }
  }
  return ret;
}

int ObDMLStmt::set_part_expr_items(ObIArray<PartExprItem> &part_items)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < part_items.count(); ++i) {
    if (OB_FAIL(set_part_expr_item(part_items.at(i)))) {
      if (OB_LIKELY(OB_ENTRY_EXIST == ret)) {
        ret = OB_SUCCESS;
      } else {
        LOG_WARN("failed to set part expr item", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::set_part_expr_item(PartExprItem &part_item)
{
  int ret = OB_SUCCESS;
  bool found = false;
  for (int64_t i = 0; !found && i < part_expr_items_.count(); i ++) {
    if (part_item.table_id_ == part_expr_items_.at(i).table_id_
        && part_item.index_tid_ == part_expr_items_.at(i).index_tid_) {
      found = true;
    }
  }
  if (found) {
    ret = OB_ENTRY_EXIST;
  } else {
    if (OB_FAIL(part_expr_items_.push_back(part_item))) {
      SQL_RESV_LOG(WARN, "push back part expr item failed", K(ret));
    }
  }

  return ret;
}

ObRawExpr* ObDMLStmt::get_part_expr(const uint64_t table_id, uint64_t index_tid) const
{
  ObRawExpr *expr = NULL;
  bool found = false;
  for (int64_t i = 0; !found && i < part_expr_items_.count(); ++i) {
    if (table_id == part_expr_items_.at(i).table_id_
        && index_tid == part_expr_items_.at(i).index_tid_) {
      expr = part_expr_items_.at(i).part_expr_;
      found = true;
    }
  }
  if (OB_ISNULL(expr) && query_ctx_ != nullptr) {
    //local index use the same part expr with data table, so if the index_tid is local index,
    //we will use its data table id to find the part expr
    int ret = OB_SUCCESS;
    const ObTableSchema *table_schema = nullptr;
    OZ(query_ctx_->sql_schema_guard_.get_table_schema(index_tid, table_schema));
    if (OB_SUCC(ret) && table_schema != nullptr && table_schema->is_storage_local_index_table()) {
      expr = get_part_expr(table_id, table_schema->get_data_table_id());
      LOG_TRACE("get part expr by data table id", K(table_id), K(index_tid),
                K(table_schema->get_data_table_id()), KPC(expr));
    }
  }
  return expr;
}

ObRawExpr* ObDMLStmt::get_subpart_expr(const uint64_t table_id, uint64_t index_tid) const
{
  ObRawExpr *expr = NULL;
  bool found = false;
  for (int64_t i = 0; !found && i < part_expr_items_.count(); ++i) {
    if (table_id == part_expr_items_.at(i).table_id_
        && index_tid == part_expr_items_.at(i).index_tid_) {
      expr = part_expr_items_.at(i).subpart_expr_;
      found = true;
    }
  }
  if (OB_ISNULL(expr) && query_ctx_ != nullptr) {
    //local index use the same subpart expr with data table, so if the index_tid is local index,
    //we will use its data table id to find the subpart expr
    int ret = OB_SUCCESS;
    const ObTableSchema *table_schema = nullptr;
    OZ(query_ctx_->sql_schema_guard_.get_table_schema(index_tid, table_schema));
    if (OB_SUCC(ret) && table_schema != nullptr && table_schema->is_storage_local_index_table()) {
      expr = get_subpart_expr(table_id, table_schema->get_data_table_id());
      LOG_TRACE("get subpart expr by data table id", K(table_id), K(index_tid),
                K(table_schema->get_data_table_id()), KPC(expr));
    }
  }
  return expr;
}

int ObDMLStmt::set_part_expr(uint64_t table_id, uint64_t index_tid, ObRawExpr *part_expr, ObRawExpr *subpart_expr)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < part_expr_items_.count(); ++i) {
    if (table_id == part_expr_items_.at(i).table_id_
        && index_tid == part_expr_items_.at(i).index_tid_) {
      ret = OB_ERR_TABLE_EXIST;
      SQL_RESV_LOG(WARN, "table part expr exists", K(table_id));
    }
  }
  if (OB_SUCC(ret)) {
    PartExprItem part_item;
    part_item.table_id_ = table_id;
    part_item.index_tid_ = index_tid;
    part_item.part_expr_ = part_expr;
    part_item.subpart_expr_ = subpart_expr;
    if (OB_FAIL(part_expr_items_.push_back(part_item))) {
      SQL_RESV_LOG(WARN, "push back part expr item failed", K(ret));
    }
  }
  return ret;
}

JoinedTable *ObDMLStmt::get_joined_table(uint64_t table_id) const
{
  JoinedTable *joined_table = NULL;
  bool found = false;
  for (int64_t i = 0; !found && i < joined_tables_.count(); ++i) {
    JoinedTable *cur_table = joined_tables_.at(i);
    if (NULL != cur_table) {
      if (cur_table->table_id_ == table_id) {
        joined_table = cur_table;
        found = true;
      }
    }
  }
  return joined_table;
}

int ObDMLStmt::pull_all_expr_relation_id_and_levels()
{
  int ret = OB_SUCCESS;
  ObArray<ObRawExpr*> relation_exprs;
  ObArray<ObSelectStmt*> view_stmts;
  if (OB_FAIL(get_relation_exprs(relation_exprs))) {
    LOG_WARN("get relation exprs failed", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < relation_exprs.count(); ++i) {
    ObRawExpr *expr = relation_exprs.at(i);
    if (OB_ISNULL(expr)) {
      ret  = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(expr->pull_relation_id_and_levels(get_current_level()))) {
      LOG_WARN("pull expr relation ids failed", K(ret), K(*expr));
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(get_from_subquery_stmts(view_stmts))) {
      LOG_WARN("get from subquery stmts failed", K(ret));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < view_stmts.count(); ++i) {
      ObSelectStmt *view_stmt = view_stmts.at(i);
      if (OB_ISNULL(view_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("view_stmt is null");
      } else if (OB_FAIL(SMART_CALL(view_stmt->pull_all_expr_relation_id_and_levels()))) {
        LOG_WARN("pull view stmt all expr relation id and levels failed", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::get_order_exprs(ObIArray<ObRawExpr*> &order_exprs) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < order_items_.count(); i++) {
    if (OB_ISNULL(order_items_.at(i).expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (OB_FAIL(order_exprs.push_back(order_items_.at(i).expr_))) {
      LOG_WARN("failed to push back exprs", K(ret));
    } else { /*do nothing*/ }
  }
  return ret;
}

int ObDMLStmt::formalize_stmt(ObSQLSessionInfo *session_info)
{
  int ret = OB_SUCCESS;
  ObArray<ObSelectStmt*> view_stmts;
  if (OB_ISNULL(session_info)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else if (OB_FAIL(formalize_relation_exprs(session_info))) {
    LOG_WARN("failed to formalize relation exprs", K(ret));
  } else if (OB_FAIL(adjust_subquery_exec_params(session_info))) {
    LOG_WARN("failed to adjust subquery exec params", K(ret));
  } else if (OB_FAIL(get_from_subquery_stmts(view_stmts))) {
    LOG_WARN("get from subquery stmts failed", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < view_stmts.count(); ++i) {
      ObSelectStmt *view_stmt = view_stmts.at(i);
      if (OB_ISNULL(view_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("view_stmt is null", K(ret));
      } else if (OB_FAIL(SMART_CALL(view_stmt->formalize_stmt(session_info)))) {
        LOG_WARN("formalize view stmt failed", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::formalize_relation_exprs(ObSQLSessionInfo *session_info)
{

  int ret = OB_SUCCESS;
  ObSEArray<ObRawExpr *, 4> relation_exprs;
  if (OB_ISNULL(session_info)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else if (OB_FAIL(get_relation_exprs(relation_exprs))) {
    LOG_WARN("get relation exprs failed", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < relation_exprs.count(); ++i) {
      ObRawExpr *expr = relation_exprs.at(i);
      if (OB_ISNULL(expr)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("expr is NULL", K(ret));
      } else if (OB_FAIL(expr->formalize(session_info))) {
        LOG_WARN("failed to formalize expr", K(ret));
      } else if (OB_FAIL(expr->pull_relation_id_and_levels(get_current_level()))) {
        LOG_WARN("pull expr relation ids failed", K(ret), K(*expr));
      } else if (OB_FAIL(expr->extract_info())) {
        // zhanyue todo: adjust this.
        // Add IS_JOIN_COND flag need use expr relation_ids, here call extract_info() again.
        LOG_WARN("failed to extract info", K(*expr));
      }
    }
  }
  return ret;
}

int ObDMLStmt::adjust_subquery_exec_params(ObSQLSessionInfo *session_info)
{
  int ret = OB_SUCCESS;
  bool is_happened = false;
  if (OB_ISNULL(session_info)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < subquery_exprs_.count(); ++i) {
      ObQueryRefRawExpr *query_expr = subquery_exprs_.at(i);
      ObSelectStmt *subquery = NULL;
      if (OB_ISNULL(query_expr) || OB_ISNULL(subquery = query_expr->get_ref_stmt())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret), K(query_expr));
      } else if (OB_FAIL(remove_const_exec_param(subquery, session_info, is_happened))) {
        LOG_WARN("failed to remove const exec param", K(ret));
      } else if (is_happened) {
        ObSEArray<ObExecParamRawExpr *, 4> non_const_params;
        bool has_const_exec_param = false;
        for (int64_t j = 0; OB_SUCC(ret) && j < query_expr->get_exec_params().count(); ++j) {
          ObExecParamRawExpr *param_expr = query_expr->get_exec_param(j);
          if (OB_ISNULL(param_expr) || OB_ISNULL(param_expr->get_ref_expr())) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("get unexpected null", K(ret), K(param_expr));
          } else if (param_expr->get_ref_expr()->is_const_expr()) {
            has_const_exec_param = true;
          } else if (OB_FAIL(non_const_params.push_back(param_expr))) {
            LOG_WARN("failed to add removed params", K(ret));
          }
        }
        if (OB_SUCC(ret) && has_const_exec_param &&
            OB_FAIL(query_expr->get_exec_params().assign(non_const_params))) {
          LOG_WARN("failed to assign new params", K(ret));
        }
    }
    }
  }
  return ret;
}

int ObDMLStmt::remove_const_exec_param(ObDMLStmt *stmt, ObSQLSessionInfo *session_info, bool &is_happened)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObRawExprPointer, 4> exprs;
  is_happened = false;
  if (OB_ISNULL(stmt) || OB_ISNULL(session_info)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("stmt is null", K(ret), K(stmt));
  } else if (stmt->get_current_level() <= 0) {
    // do nothing
  } else if (OB_FAIL(stmt->get_relation_exprs(exprs))) {
    LOG_WARN("failed to get exprs", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < exprs.count(); ++i) {
    bool tmp_happened = false;
    ObRawExprPointer &expr_ptr = exprs.at(i);
    ObRawExpr *expr = NULL;
    if (OB_FAIL(expr_ptr.get(expr))) {
      LOG_WARN("failed to get expr", K(ret));
    } else if (OB_FAIL(do_remove_const_exec_param(expr, tmp_happened))) {
      LOG_WARN("failed to remove const exec param", K(ret));
    } else if (!tmp_happened) {
      // do nothing
    } else if (expr->formalize(session_info)) {
      LOG_WARN("failed to formalize expr", K(ret));
    } else if (OB_FAIL(expr_ptr.set(expr))) {
      LOG_WARN("failed to update expr", K(ret));
    } else {
      is_happened = true;
      LOG_TRACE("succeed to remove const exec param", K(ret), K(tmp_happened), K(*expr));
    }
  }
  return ret;
}

int ObDMLStmt::do_remove_const_exec_param(ObRawExpr *&expr, bool &is_happened)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("expr is null", K(ret), K(expr));
  } else if (expr->is_exec_param_expr()) {
    ObExecParamRawExpr *exec_param = static_cast<ObExecParamRawExpr *>(expr);
    ObRawExpr *ref_expr = exec_param->get_ref_expr();
    if (OB_ISNULL(ref_expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ref expr is null", K(ret));
    } else if (ref_expr->is_const_expr() &&
               !ref_expr->has_flag(CNT_ONETIME)) {
      // if the ref expr is a const expr but is also a onetime expr
      // then it is no need to remove it
      expr = ref_expr;
      is_happened = true;
      if (expr->has_flag(CNT_DYNAMIC_PARAM)) {
        if (OB_FAIL(SMART_CALL(do_remove_const_exec_param(expr, is_happened)))) {
          LOG_WARN("failed to remove const exec param", K(ret));
        }
      }
    }
  } else if (expr->has_flag(CNT_DYNAMIC_PARAM)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); ++i) {
      if (OB_FAIL(SMART_CALL(do_remove_const_exec_param(expr->get_param_expr(i),
                                                        is_happened)))) {
        LOG_WARN("failed to remove const exec param", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::formalize_stmt_expr_reference()
{
  int ret = OB_SUCCESS;
  ObSEArray<ObRawExpr*, 32> stmt_exprs;
  ObSEArray<ObSelectStmt*, 32> child_stmts;
  if (OB_FAIL(clear_sharable_expr_reference())) {
    LOG_WARN("failed to clear sharable expr reference", K(ret));
  } else if (OB_FAIL(get_relation_exprs(stmt_exprs))) {
    LOG_WARN("get relation exprs failed", K(ret));
  } else if (OB_FAIL(get_child_stmts(child_stmts))) {
    LOG_WARN("failed to get child stmts", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < stmt_exprs.count(); i++) {
      if (OB_ISNULL(stmt_exprs.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else if (OB_FAIL(set_sharable_expr_reference(*stmt_exprs.at(i)))) {
        LOG_WARN("failed to set sharable expr reference", K(ret));
      } else { /*do nothing*/ }
    }
    // table function column expr should not be removed
    for (int64_t i = 0; OB_SUCC(ret) && i < column_items_.count(); i++) {
      TableItem *table_item = NULL;
      ColumnItem &column_item = column_items_.at(i);
      if (OB_ISNULL(column_item.expr_) ||
          OB_ISNULL(table_item = get_table_item_by_id(column_item.table_id_))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(column_item.expr_), K(table_item), K(ret));
      } else if (table_item->is_function_table() ||
                 table_item->for_update_ ||
                 is_hierarchical_query()) {
        if (OB_FAIL(set_sharable_expr_reference(*column_item.expr_))) {
          LOG_WARN("failed to set sharable exprs reference", K(ret));
        }
      } else { /*do nothing*/ }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < child_stmts.count(); ++i) {
      ObSelectStmt *stmt = child_stmts.at(i);
      if (OB_ISNULL(stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("stmt is null", K(ret));
      } else if (OB_FAIL(SMART_CALL(stmt->formalize_stmt_expr_reference()))) {
        LOG_WARN("failed to formalize stmt reference", K(ret));
      } else { /*do nothing*/ }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(remove_useless_sharable_expr())) {
        LOG_WARN("failed to remove useless sharable expr", K(ret));
      } else if (OB_FAIL(check_pseudo_column_valid())) {
        LOG_WARN("failed to check pseudo column", K(ret));
      } else { /*do nothing*/ }
    }
  }
  return ret;
}

int ObDMLStmt::check_pseudo_column_valid()
{
  int ret = OB_SUCCESS;
  const ObIArray<ObRawExpr*> &pseudo_cols = get_pseudo_column_like_exprs();
  ObRawExpr *expr = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && i < pseudo_cols.count(); i++) {
    if (OB_ISNULL(expr = pseudo_cols.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get null expr", K(ret));
    } else {
      switch (expr->get_expr_type()) {
        case T_ORA_ROWSCN: {
          ObPseudoColumnRawExpr *ora_rowscn = static_cast<ObPseudoColumnRawExpr*>(expr);
          const TableItem *table = NULL;
          if (OB_UNLIKELY(get_current_level() != ora_rowscn->get_expr_level())
              || OB_ISNULL(table = get_table_item_by_id(ora_rowscn->get_table_id()))
              || OB_UNLIKELY(!table->is_basic_table())) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("failed to find basic table for ora_rowscn", K(ret), K(table), K(*expr));
          }
          break;
        }
        default:  /* do nothing for other type pseudo column like expr */
          break;
      }
    }
  }
  return ret;
}

int ObDMLStmt::set_sharable_expr_reference(ObRawExpr &expr)
{
  int ret = OB_SUCCESS;
  if (expr.is_column_ref_expr() || expr.is_aggr_expr() ||
      expr.is_win_func_expr() || expr.is_query_ref_expr() ||
      ObRawExprUtils::is_pseudo_column_like_expr(expr)) {
    expr.set_explicited_reference();
    if (expr.is_column_ref_expr()) {
      ObColumnRefRawExpr &column_expr = static_cast<ObColumnRefRawExpr&>(expr);
      if (NULL == get_column_item(column_expr.get_table_id(),
                                  column_expr.get_column_id())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("column item does not exists in the current stmt", K(ret), K(column_expr));
      } else if (NULL != column_expr.get_dependant_expr() &&
                 OB_FAIL(SMART_CALL(set_sharable_expr_reference(*column_expr.get_dependant_expr())))) {
        LOG_WARN("failed to set sharable expr", K(ret), K(column_expr));
      }
    } else if (T_ORA_ROWSCN == expr.get_expr_type() &&
               !ObRawExprUtils::find_expr(get_pseudo_column_like_exprs(), &expr)) {
      // now check ora_rowscn pseudo column only,
      //  some preudo column like T_PSEUDO_GROUP_PARAM and T_CTE_SEARCH_COLUMN can not get
      //  from pseudo_column_like_exprs_, after adjust them run case:
      //  with_clause.recursive_oracle_search_oracle
      //  update.multi_stmt_explain_update_base
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to find pseudo column", K(ret), K(expr));
    } else if (expr.is_query_ref_expr() &&
               !ObRawExprUtils::find_expr(get_subquery_exprs(), &expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("query ref expr does not exist in the stmt", K(ret), K(expr));
    } else if (is_select_stmt() &&
               OB_FAIL(static_cast<ObSelectStmt *>(this)->check_aggr_and_winfunc(expr))) {
      LOG_WARN("failed to check aggr and winfunc validity", K(ret));
    }
  } else if (expr.has_flag(IS_ONETIME)) {
    ObExecParamRawExpr &exec_param = static_cast<ObExecParamRawExpr &>(expr);
    if (OB_ISNULL(exec_param.get_ref_expr())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("exec param expr is null", K(ret));
    } else if (OB_FAIL(SMART_CALL(set_sharable_expr_reference(*exec_param.get_ref_expr())))) {
      LOG_WARN("failed to set sharable expr reference", K(ret));
    }
  }
  if (OB_SUCC(ret) &&
      (expr.has_flag(CNT_COLUMN) || expr.has_flag(CNT_AGG) ||
       expr.has_flag(CNT_WINDOW_FUNC) || expr.has_flag(CNT_SUB_QUERY) ||
       expr.has_flag(CNT_ROWNUM) || expr.has_flag(CNT_SEQ_EXPR) ||
       expr.has_flag(CNT_PSEUDO_COLUMN) || expr.has_flag(CNT_ONETIME))) {
    for (int64_t i = 0; OB_SUCC(ret) && i < expr.get_param_count(); i++) {
      if (OB_ISNULL(expr.get_param_expr(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else if (OB_FAIL(SMART_CALL(set_sharable_expr_reference(*expr.get_param_expr(i))))) {
        LOG_WARN("failed to set sharable expr", K(ret), K(expr));
      } else { /*do nothing*/ }
    }
  }
  return ret;
}

int ObDMLStmt::remove_useless_sharable_expr()
{
  int ret = OB_SUCCESS;
  for (int64_t i = column_items_.count() - 1; OB_SUCC(ret) && i >= 0; i--) {
    bool is_referred = false;
    ObColumnRefRawExpr *expr = NULL;
    if (OB_ISNULL(expr = column_items_.at(i).expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (expr->is_explicited_reference() || expr->is_rowkey_column()) {
      /*do nothing*/
    } else if (OB_FAIL(is_referred_by_partitioning_expr(expr,
                                                        is_referred))) {
      LOG_WARN("failed to check whether is referred by partitioning expr", K(ret));
    } else if (is_referred) {
      /*do nothing*/
    } else if (OB_FAIL(column_items_.remove(i))) {
      LOG_WARN("failed to remove column item", K(ret));
    } else {
      LOG_TRACE("succeed to remove column items", K(expr), K(lbt()));
    }
  }
  for (int64_t i = subquery_exprs_.count() - 1; OB_SUCC(ret) && i >= 0; i--) {
    ObQueryRefRawExpr *expr = NULL;
    if (OB_ISNULL(expr = subquery_exprs_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (expr->is_explicited_reference() || expr->is_cursor()) {
      /*do nothing*/
    } else if (OB_FAIL(subquery_exprs_.remove(i))) {
      LOG_WARN("failed to remove subquery expr", K(ret));
    } else {
      LOG_TRACE("succeed to remove subquery exprs", K(*expr));
    }
  }
  for (int64_t i = pseudo_column_like_exprs_.count() - 1; OB_SUCC(ret) && i >= 0; i--) {
    ObRawExpr *expr = NULL;
    if (OB_ISNULL(expr = pseudo_column_like_exprs_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (expr->is_explicited_reference()) {
      // do nothing
    } else if (OB_FAIL(pseudo_column_like_exprs_.remove(i))) {
      LOG_WARN("failed to remove pseudo column like exprs", K(ret));
    } else {
      LOG_TRACE("succeed to remove pseudo column like exprs", K(*expr));
    }
  }
  return ret;
}

int ObDMLStmt::is_referred_by_partitioning_expr(const ObRawExpr *expr,
                                                bool &is_referred)
{
  int ret = OB_SUCCESS;
  is_referred = false;
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else {
    ObSEArray<ObRawExpr*, 16> part_exprs;
    for (int64_t i = 0; OB_SUCC(ret) && i < part_expr_items_.count(); i++) {
      ObRawExpr *part_expr = part_expr_items_.at(i).part_expr_;
      ObRawExpr *subpart_expr = part_expr_items_.at(i).subpart_expr_;
      if (NULL != part_expr && OB_FAIL(part_exprs.push_back(part_expr))) {
        LOG_WARN("failed to push back exprs", K(ret));
      } else if (NULL != subpart_expr && OB_FAIL(part_exprs.push_back(subpart_expr))) {
        LOG_WARN("failed to push back exprs", K(ret));
      } else { /*do nothing*/ }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < check_constraint_items_.count(); i++) {
      if (OB_FAIL(append(part_exprs, check_constraint_items_.at(i).check_constraint_exprs_))) {
        LOG_WARN("failed to append exprs", K(ret));
      } else {/*do nothing*/}
    }
    if (OB_SUCC(ret)) {
      ObSEArray<ObRawExpr*, 16> column_exprs;
      if (OB_FAIL(ObRawExprUtils::extract_column_exprs(part_exprs, column_exprs))) {
        LOG_WARN("failed to extract column exprs", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < column_exprs.count(); i++) {
          ObColumnRefRawExpr *column_expr = NULL;
          if (OB_ISNULL(column_exprs.at(i))) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("get unexpected null", K(ret));
          } else if (OB_UNLIKELY(!column_exprs.at(i)->is_column_ref_expr())) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("get unexpected expr type", K(ret));
          } else if (FALSE_IT(column_expr = static_cast<ObColumnRefRawExpr*>(column_exprs.at(i)))) {
            /*do nothing*/
          } else if (NULL != column_expr->get_dependant_expr() &&
                     OB_FAIL(part_exprs.push_back(column_expr->get_dependant_expr()))) {
            LOG_WARN("failed to push back expr", K(ret));
          } else { /*do nothing*/ }
        }
      }
    }
    if (OB_SUCC(ret)) {
      is_referred = ObOptimizerUtil::is_sub_expr(expr, part_exprs);
    }
  }
  return ret;
}

int ObDMLStmt::clear_sharable_expr_reference()
{
  int ret = OB_SUCCESS;
  ObRawExpr *expr = NULL;
  // for column items
  for (int64_t i = 0; OB_SUCC(ret) && i < column_items_.count(); i++) {
    if (OB_ISNULL(expr = column_items_.at(i).expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else {
      expr->clear_explicited_referece();
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < subquery_exprs_.count(); i++) {
    if (OB_ISNULL(subquery_exprs_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else {
      subquery_exprs_.at(i)->clear_explicited_referece();
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < pseudo_column_like_exprs_.count(); i++) {
    if (OB_ISNULL(pseudo_column_like_exprs_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else {
      pseudo_column_like_exprs_.at(i)->clear_explicited_referece();
    }
  }
  return ret;
}

int ObDMLStmt::get_from_subquery_stmts(ObIArray<ObSelectStmt*> &child_stmts) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < get_table_size(); ++i) {
    const TableItem *table_item = get_table_item(i);
    if (OB_ISNULL(table_item)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table_item is null", K(i));
    } else if (table_item->is_generated_table()) {// to remove temp table
      if (OB_FAIL(child_stmts.push_back(table_item->ref_query_))) {
        LOG_WARN("adjust parent namespace stmt failed", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::get_subquery_stmts(common::ObIArray<ObSelectStmt*> &child_stmts) const
{
  int ret = OB_SUCCESS;
  for (int64_t j = 0; OB_SUCC(ret) && j < get_subquery_expr_size(); ++j) {
    ObQueryRefRawExpr *subquery_ref = subquery_exprs_.at(j);
    if (OB_ISNULL(subquery_ref) ||
        OB_ISNULL(subquery_ref->get_ref_stmt())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("subquery reference is null", K(subquery_ref));
    } else if (OB_FAIL(child_stmts.push_back(subquery_ref->get_ref_stmt()))) {
      LOG_WARN("stored subquery reference stmt failed", K(ret));
    }
  }
  return ret;
}

int ObDMLStmt::get_table_item(const ObSQLSessionInfo *session_info,
                              const ObString &database_name,
                              const ObString &object_name,
                              const TableItem *&table_item) const
{
  int ret = OB_SUCCESS;
  int64_t num = table_items_.count();
  bool is_found = false;
  for (int64_t i = 0; OB_SUCC(ret) && i < num; i++) {
    if (OB_ISNULL(table_items_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table items is null", K(i));
    } else {
      const TableItem &item = *(table_items_.at(i));
      bool is_db_found = true;
      //database_name is not empty, so must compare database name
      //if database_name is empty, means that the column does not specify the database name,
      //so we must find in all table
      if (OB_FAIL(ObResolverUtils::name_case_cmp(session_info, database_name, item.database_name_,
                                                 OB_TABLE_NAME_CLASS, is_db_found))) {
        LOG_WARN("fail to compare db names", K(database_name), K(object_name), K(item), K(ret));
      } else if (is_db_found) {
        const ObString &src_table_name = item.get_object_name();
        if (OB_FAIL(ObResolverUtils::name_case_cmp(session_info, object_name, src_table_name, OB_TABLE_NAME_CLASS, is_found))) {
          LOG_WARN("fail to compare names", K(database_name), K(object_name), K(item), K(ret));
        } else if (is_found) {
          table_item = &item;
          break;
        }
      }
    }
  }
  if (OB_SUCC(ret) && !is_found) {
    ret = OB_ERR_UNKNOWN_TABLE;
  }
  return ret;
}

//get all table in current namespace, maybe table come from different database
int ObDMLStmt::get_all_table_item_by_tname(const ObSQLSessionInfo *session_info,
                                           const ObString &db_name,
                                           const ObString &table_name,
                                           ObIArray<const TableItem*> &table_items) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < table_items_.count(); ++i) {
    const TableItem *item = table_items_.at(i);
    if (OB_ISNULL(item)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table item is null");
    } else {
      const ObString &tname = item->alias_name_.length() > 0 ? item->alias_name_ : item->table_name_;
      bool is_equal = true;
      if (!db_name.empty()) {
        if (OB_FAIL(ObResolverUtils::name_case_cmp(session_info, db_name, item->database_name_, OB_TABLE_NAME_CLASS, is_equal))) {
          LOG_WARN("fail to compare names", K(table_name), K(item), K(ret));
        }
      }
      if (is_equal && OB_FAIL(ObResolverUtils::name_case_cmp(session_info, table_name, tname, OB_TABLE_NAME_CLASS, is_equal))) {
        LOG_WARN("fail to compare names", K(table_name), K(item), K(ret));
      } else if (is_equal) {
        if (OB_FAIL(table_items.push_back(item))) {
          LOG_WARN("push back table item failed", K(ret));
        }
      }
    }
  }
  return ret;
}

bool ObDMLStmt::is_semi_left_table(const uint64_t table_id)
{
  bool bret = false;
  int64_t num = semi_infos_.count();
  for (int64_t i = 0; !bret && i < num; ++i) {
    if (semi_infos_.at(i) != NULL) {
      bret = ObOptimizerUtil::find_item(semi_infos_.at(i)->left_table_ids_, table_id);
    }
  }
  return bret;
}

SemiInfo *ObDMLStmt::get_semi_info_by_id(const uint64_t semi_id)
{
  SemiInfo *semi_info = NULL;
  int64_t num = semi_infos_.count();
  for (int64_t i = 0; i < num; ++i) {
    if (semi_infos_.at(i) != NULL && semi_infos_.at(i)->semi_id_ == semi_id) {
      semi_info = semi_infos_.at(i);
      break;
    }
  }
  return semi_info;
}

// get a table in joined tables, table can be a basic/generate table or a joined table.
int ObDMLStmt::get_general_table_by_id(uint64_t table_id, TableItem *&table)
{
  int ret = OB_SUCCESS;
  table = get_table_item_by_id(table_id);
  for (int64_t i = 0; NULL == table && i < joined_tables_.count(); ++i) {
    ret = get_general_table_by_id(joined_tables_.at(i), table_id, table);
  }
  if (NULL == table) {
    LOG_WARN("get null table", K(table_id), K(*this));
  }
  return ret;
}

int ObDMLStmt::get_general_table_by_id(TableItem *cur_table,
                                                    const uint64_t table_id,
                                                    TableItem *&table)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(cur_table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null", K(ret), K(cur_table));
  } else if (table_id == cur_table->table_id_) {
    table = cur_table;
  } else if (cur_table->is_joined_table()) {
    JoinedTable *joined_table = static_cast<JoinedTable*>(cur_table);
    if (OB_FAIL(SMART_CALL(get_general_table_by_id(joined_table->left_table_,
                                                                table_id, table)))) {
      LOG_WARN("failed to get table item by id", K(ret));
    } else if (NULL != table) {
      /* do nothing */
    } else if (OB_FAIL(SMART_CALL(get_general_table_by_id(joined_table->right_table_,
                                                                       table_id, table)))) {
      LOG_WARN("failed to get table item by id", K(ret));
    }
  }
  return ret;
}

TableItem *ObDMLStmt::get_table_item_by_id(uint64_t table_id) const
{
  TableItem *table_item = NULL;
  int64_t num = table_items_.count();
  for (int64_t i = 0; i < num; ++i) {
    if (table_items_.at(i) != NULL && table_items_.at(i)->table_id_ == table_id) {
      table_item = table_items_.at(i);
      break;
    }
  }
  return table_item;
}

int ObDMLStmt::get_table_item_by_id(ObIArray<uint64_t> &table_ids,
                                    ObIArray<TableItem*> &tables)
{
  int ret = OB_SUCCESS;
  int64_t num = table_items_.count();
  for (int64_t i = 0; i < num; ++i) {
    if (!has_exist_in_array(table_ids, table_items_.at(i)->table_id_)) {
      /*do nothing*/
    } else if (OB_ISNULL(table_items_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table item is null", K(ret));
    } else if (OB_FAIL(tables.push_back(table_items_.at(i)))) {
      LOG_WARN("failed to push back table", K(ret));
    }
  }
  return ret;
}

TableItem* ObDMLStmt::get_table_item(const FromItem item)
{
  TableItem *ret = NULL;
  if (item.is_joined_) {
    ret = get_joined_table(item.table_id_);
  } else {
    ret = get_table_item_by_id(item.table_id_);
  }
  return ret;
}

const TableItem* ObDMLStmt::get_table_item(const FromItem item) const
{
  const TableItem *ret = NULL;
  if (item.is_joined_) {
    ret = get_joined_table(item.table_id_);
  } else {
    ret = get_table_item_by_id(item.table_id_);
  }
  return ret;
}

TableItem *ObDMLStmt::create_table_item(ObIAllocator &allocator)
{
  TableItem *table_item = NULL;
  void *ptr = NULL;
  if (NULL == (ptr = allocator.alloc(sizeof(TableItem)))) {
    LOG_WARN("alloc table item failed");
  } else {
    table_item = new(ptr) TableItem();
  }
  return table_item;
}

int ObDMLStmt::add_table_item(const ObSQLSessionInfo *session_info, TableItem *table_item)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table_item)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table item is null");
  } else {
    const TableItem *old_item = NULL;
    const ObString &object_name = table_item->get_object_name();
    if (OB_FAIL(get_table_item(session_info, table_item->database_name_, object_name, old_item))) {
      if (OB_ERR_UNKNOWN_TABLE == ret) {
        ret = OB_SUCCESS;
      } else {
        LOG_WARN("get table item failed", K(ret), K(object_name), K_(table_item->database_name));
      }
    } else {
      ret = OB_ERR_NONUNIQ_TABLE;
      LOG_USER_ERROR(OB_ERR_NONUNIQ_TABLE, object_name.length(), object_name.ptr());
      LOG_WARN("table_item existed", K(ret), KPC(table_item), KPC(old_item));
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(table_items_.push_back(table_item))) {
        LOG_WARN("push back table items failed", K(ret));
      } else if (OB_FAIL(set_table_bit_index(table_item->table_id_))) {
        LOG_WARN("set table bit index failed", K(ret), K(*table_item));
      }
    }
  }
  LOG_DEBUG("finish to add table item", K(*table_item), K(tables_hash_), KPC(table_item->ref_query_),
                                        K(common::lbt()));
  return ret;
}

int ObDMLStmt::add_table_item(const ObSQLSessionInfo *session_info, TableItem *table_item, bool &have_same_table_name)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table_item)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table item is null");
  } else if (OB_UNLIKELY(table_item->alias_name_.length() > OB_MAX_USER_TABLE_NAME_LENGTH_ORACLE
              && is_oracle_mode() && !table_item->is_index_table_)) {
    ret = OB_ERR_TOO_LONG_IDENT;
    LOG_WARN("table alias name too long", K(ret), KPC(table_item));
  } else {
    const TableItem *old_item = NULL;
    const ObString &object_name = table_item->get_object_name();
    if (OB_FAIL(get_table_item(session_info, table_item->database_name_, object_name, old_item))) {
      if (OB_ERR_UNKNOWN_TABLE == ret) {
        ret = OB_SUCCESS;
      } else {
        LOG_WARN("get table item failed", K(ret), K(object_name), K_(table_item->database_name));
      }
    } else if (lib::is_oracle_mode()) {//oracle allow have the same name two tables
      have_same_table_name |= table_item->is_basic_table();
    } else {
      ret = OB_ERR_NONUNIQ_TABLE;
      LOG_USER_ERROR(OB_ERR_NONUNIQ_TABLE, object_name.length(), object_name.ptr());
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(table_items_.push_back(table_item))) {
        LOG_WARN("push back table items failed", K(ret));
      } else if (OB_FAIL(set_table_bit_index(table_item->table_id_))) {
        LOG_WARN("set table bit index failed", K(ret));
      }
    }
  }
  LOG_DEBUG("finish to add table item", K(*table_item), K(tables_hash_), K(common::lbt()));
  return ret;
}

int ObDMLStmt::generate_view_name(ObIAllocator &allocator, ObString &view_name, bool is_temp)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  const uint64_t OB_MAX_SUBQUERY_NAME_LENGTH = 64;
  const char *SUBQUERY_VIEW = is_temp ? "TEMP" : "VIEW";
  char buf[OB_MAX_SUBQUERY_NAME_LENGTH];
  int64_t buf_len = OB_MAX_SUBQUERY_NAME_LENGTH;
  if (OB_FAIL(BUF_PRINTF("%s", SUBQUERY_VIEW))) {
    LOG_WARN("append name to buf error", K(ret));
  } else if (OB_FAIL(append_id_to_view_name(buf, OB_MAX_SUBQUERY_NAME_LENGTH, pos, is_temp))) {
    LOG_WARN("append name to buf error", K(ret));
  } else {
    ObString generate_name(pos, buf);
    if (OB_FAIL(ob_write_string(allocator, generate_name, view_name))) {
      LOG_WARN("failed to write string", K(ret));
    }
  }
  return ret;
}

int ObDMLStmt::generate_func_table_name(ObIAllocator &allocator, ObString &table_name)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  const uint64_t OB_MAX_SUBQUERY_NAME_LENGTH = 64;
  const char *SUBQUERY_VIEW = "FUNC_TABLE";
  char buf[OB_MAX_SUBQUERY_NAME_LENGTH];
  int64_t buf_len = OB_MAX_SUBQUERY_NAME_LENGTH;
  if (OB_FAIL(BUF_PRINTF("%s", SUBQUERY_VIEW))) {
    LOG_WARN("append name to buf error", K(ret));
  } else if (OB_FAIL(append_id_to_view_name(buf, OB_MAX_SUBQUERY_NAME_LENGTH, pos, false))) {
    LOG_WARN("append name to buf error", K(ret));
  } else {
    ObString generate_name(pos, buf);
    if (OB_FAIL(ob_write_string(allocator, generate_name, table_name))) {
      LOG_WARN("failed to write string", K(ret));
    }
  }
  return ret;
}

int ObDMLStmt::generate_anonymous_view_name(ObIAllocator &allocator, ObString &view_name)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  const uint64_t OB_MAX_SUBQUERY_NAME_LENGTH = 64;
  const char *SUBQUERY_VIEW = "ANONYMOUS_VIEW";
  char buf[OB_MAX_SUBQUERY_NAME_LENGTH];
  int64_t buf_len = OB_MAX_SUBQUERY_NAME_LENGTH;
  if (OB_FAIL(BUF_PRINTF("%s", SUBQUERY_VIEW))) {
    LOG_WARN("append name to buf error", K(ret));
  } else if (OB_FAIL(append_id_to_view_name(buf, OB_MAX_SUBQUERY_NAME_LENGTH, pos, false, true))) {
    LOG_WARN("append name to buf error", K(ret));
  } else {
    ObString generate_name(pos, buf);
    if (OB_FAIL(ob_write_string(allocator, generate_name, view_name))) {
      LOG_WARN("failed to write string", K(ret));
    }
  }
  return ret;
}

int ObDMLStmt::append_id_to_view_name(char *buf, int64_t buf_len, int64_t &pos, bool is_temp, bool is_anonymous)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(buf)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Buf should not be NULL", K(ret));
  } else if (pos >= buf_len) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("Buf size not enough", K(ret));
  } else {
    bool find_unique = false;
    int64_t old_pos = pos;
    do {
      pos = old_pos;
      int64_t id = is_anonymous ?
                   get_query_ctx()->get_anonymous_view_id() :
                   (is_temp ?
                   get_query_ctx()->get_temp_table_id() :
                   get_query_ctx()->get_new_subquery_id());
      if (OB_FAIL(BUF_PRINTF("%ld", id))) {
        LOG_WARN("Append idx to stmt_name error", K(ret));
      } else {
        bool find_dup = false;
        for (int64_t i = 0; !find_dup && i < table_items_.count(); ++i) {
          const ObString &tname = table_items_.at(i)->alias_name_.length() > 0 ?
              table_items_.at(i)->alias_name_ : table_items_.at(i)->table_name_;
          if (0 == tname.case_compare(buf)) {
            find_dup = true;
          } else {}
        }
        find_unique = !find_dup;
      }
    } while(!find_unique && OB_SUCC(ret));
  }
  return ret;
}

int ObDMLStmt::get_table_item_idx(const TableItem *child_table, int64_t &idx) const
{
  int ret = OB_SUCCESS;
  idx = -1;
  bool found = false;
  for (int64_t i = 0; OB_SUCC(ret) && !found && i < get_table_size(); i++) {
    if (child_table == get_table_item(i)) {
      idx = i;
      found = true;
    }
  }
  return ret;
}

int ObDMLStmt::get_table_item_idx(const uint64_t table_id, int64_t &idx) const
{
  int ret = OB_SUCCESS;
  idx = -1;
  bool found = false;
  for (int64_t i = 0; OB_SUCC(ret) && !found && i < get_table_size(); i++) {
    if (OB_ISNULL(get_table_item(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null", K(ret));
    } else if (table_id == get_table_item(i)->table_id_) {
      idx = i;
      found = true;
    }
  }
  return ret;
}

int ObDMLStmt::remove_table_item(const ObIArray<TableItem *> &table_items)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < table_items.count(); i++) {
    ret = remove_table_item(table_items.at(i));
  }
  return ret;
}

int ObDMLStmt::remove_table_item(const TableItem *ti)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < get_table_size(); i++) {
    if (ti == get_table_item(i)) {
      if (OB_FAIL(table_items_.remove(i))) {
        LOG_WARN("fail to remove table item", K(ret));
      } else {
        LOG_DEBUG("succ to remove_table_item", KPC(ti), K(lbt()));
        break;
      }
    }
  }
  return ret;
}

int ObDMLStmt::get_joined_item_idx(const TableItem *child_table, int64_t &idx) const
{
  int ret = OB_SUCCESS;
  idx = -1;
  bool is_found = false;
  for (int64_t i = 0; !is_found && i < joined_tables_.count(); i++) {
    if (child_table == joined_tables_.at(i)) {
      idx = i;
      is_found = true;
    } else { /*do nothing*/ }
  }
  return ret;
}

int ObDMLStmt::get_column_ids(uint64_t table_id, ObSqlBitSet<> &column_ids)const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_items_.count(); i++) {
    if (table_id == column_items_.at(i).table_id_) {
      if (OB_FAIL(column_ids.add_member(column_items_.at(i).column_id_))) {
        LOG_WARN("failed to add member", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::get_column_items(ObIArray<uint64_t> &table_ids,
                                ObIArray<ColumnItem> &column_items) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < table_ids.count(); i++) {
    ret = get_column_items(table_ids.at(i), column_items);
  }
  return ret;
}

int ObDMLStmt::get_column_items(uint64_t table_id, ObIArray<ColumnItem> &column_items) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_items_.count(); i++) {
    if (table_id == column_items_.at(i).table_id_) {
      ret = column_items.push_back(column_items_.at(i));
    }
  }
  return ret;
}

int ObDMLStmt::get_column_exprs(ObIArray<ObColumnRefRawExpr *> &column_exprs) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_items_.count(); ++i) {
    ret = column_exprs.push_back(column_items_.at(i).expr_);
  }
  return ret;
}

int ObDMLStmt::get_column_exprs(ObIArray<ObRawExpr *> &column_exprs) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_items_.count(); ++i) {
    ret = column_exprs.push_back(column_items_.at(i).expr_);
  }
  return ret;
}

int ObDMLStmt::get_view_output(const TableItem &table,
                               ObIArray<ObRawExpr *> &select_list,
                               ObIArray<ObRawExpr *> &column_list) const
{
  int ret = OB_SUCCESS;
  ObSEArray<ObColumnRefRawExpr *, 4> columns;
  if (OB_UNLIKELY(!table.is_generated_table()) ||
      OB_ISNULL(table.ref_query_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the table is not a generated table", K(ret));
  } else if (OB_FAIL(get_column_exprs(table.table_id_, columns))) {
    LOG_WARN("failed to get column exprs", K(ret));
  }
  // a output column of the ref_query_ may be pruned if it is not used
  // hence, we do not directly get column exprs (from the dml stmt)
  // and select exprs (from the ref_query_)
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    ObColumnRefRawExpr *col = columns.at(i);
    int64_t idx = OB_INVALID_INDEX;
    if (OB_ISNULL(col)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("column expr is null", K(ret));
    } else {
      idx = col->get_column_id() - OB_APP_MIN_COLUMN_ID;
      if (OB_UNLIKELY(idx < 0 || idx >= table.ref_query_->get_select_item_size())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid index", K(ret), K(idx));
      } else if (OB_FAIL(select_list.push_back(table.ref_query_->get_select_item(idx).expr_))) {
        LOG_WARN("failed to push back select expr", K(ret));
      } else if (OB_FAIL(column_list.push_back(col))) {
        LOG_WARN("failed to push back column expr", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::get_ddl_view_output(const TableItem &table,
                               ObIArray<ObRawExpr *> &column_list) const
{
  int ret = OB_SUCCESS;
  ObSEArray<ObColumnRefRawExpr *, 4> columns;
  if (OB_UNLIKELY(!table.is_generated_table()) ||
      OB_ISNULL(table.ref_query_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the table is not a generated table", K(ret));
  } else if (OB_FAIL(get_column_exprs(table.table_id_, columns))) {
    LOG_WARN("failed to get column exprs", K(ret));
  } else if (OB_FAIL(append(column_list, columns))) {
    LOG_WARN("failed to append columns", K(ret));
  }
  return ret;
}

int32_t ObDMLStmt::get_table_bit_index(uint64_t table_id) const
{
  int64_t idx = tables_hash_.get_idx(table_id, OB_INVALID_ID);
  return static_cast<int32_t>(idx);
}

int ObDMLStmt::set_table_bit_index(uint64_t table_id)
{
  return tables_hash_.add_column_desc(table_id, OB_INVALID_ID);
}

int ObDMLStmt::relids_to_table_ids(const ObSqlBitSet<> &table_set,
                                   ObIArray<uint64_t> &table_ids) const
{
  int ret = OB_SUCCESS;
  TableItem *table = NULL;
  int64_t idx = OB_INVALID_INDEX;
  for (int64_t i = 0; OB_SUCC(ret) && i < table_items_.count(); ++i) {
    if (OB_ISNULL(table = table_items_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table item is null", K(ret));
    } else if (OB_UNLIKELY((idx = get_table_bit_index(table->table_id_)) == OB_INVALID_INDEX)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get table item invalid idx", K(idx), K(table->table_id_));
    } else if (table_set.has_member(idx)) {
      ret = table_ids.push_back(table->table_id_);
    }
  }
  return ret;
}

int ObDMLStmt::get_table_rel_ids(const TableItem &target,
                                 ObSqlBitSet<> &table_set) const
{
  int ret = OB_SUCCESS;
  if (target.is_joined_table()) {
    const JoinedTable &cur_table = static_cast<const JoinedTable &>(target);
    for (int64_t i = 0; OB_SUCC(ret) && i < cur_table.single_table_ids_.count(); ++i) {
      if (OB_FAIL(table_set.add_member(get_table_bit_index(cur_table.single_table_ids_.at(i))))) {
        LOG_WARN("failed to add member", K(ret), K(cur_table.single_table_ids_.at(i)));
      }
    }
  } else if (OB_FAIL(table_set.add_member(get_table_bit_index(target.table_id_)))) {
    LOG_WARN("failed to add member", K(ret), K(target.table_id_));
  }
  return ret;
}

int ObDMLStmt::get_table_rel_ids(const ObIArray<uint64_t> &table_ids,
                                 ObSqlBitSet<> &table_set) const
{
  int ret = OB_SUCCESS;
  int32_t idx = OB_INVALID_INDEX;
  for (int64_t i = 0; OB_SUCC(ret) && i < table_ids.count(); ++i) {
    idx = get_table_bit_index(table_ids.at(i));
    if (OB_UNLIKELY(OB_INVALID_INDEX == idx)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpect idx", K(ret));
    } else if (OB_FAIL(table_set.add_member(idx))) {
      LOG_WARN("failed to add members", K(ret));
    }
  }
  return ret;
}

int ObDMLStmt::get_table_rel_ids(const uint64_t table_id,
                                 ObSqlBitSet<> &table_set) const
{
  int ret = OB_SUCCESS;
  int32_t idx = get_table_bit_index(table_id);
  if (OB_UNLIKELY(OB_INVALID_INDEX == idx)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpect idx", K(ret));
  } else if (OB_FAIL(table_set.add_member(idx))) {
    LOG_WARN("failed to add members", K(ret));
  }
  return ret;
}

int ObDMLStmt::get_table_rel_ids(const ObIArray<TableItem*> &tables,
                                 ObSqlBitSet<> &table_set) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < tables.count(); ++i) {
    if (OB_ISNULL(tables.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpect null", K(ret));
    } else if (OB_FAIL(get_table_rel_ids(*tables.at(i), table_set))) {
      LOG_WARN("failed to get table rel ids", K(ret));
    }
  }
  return ret;
}

int ObDMLStmt::get_from_tables(ObRelIds &table_set) const
{
  int ret = OB_SUCCESS;
  ObSqlBitSet<> tmp_table_set;
  if (OB_FAIL(get_from_tables(tmp_table_set))) {
    LOG_WARN("failed to get from tables", K(ret));
  } else if (OB_FAIL(table_set.add_members(tmp_table_set))) {
    LOG_WARN("failed to add members", K(ret));
  }
  return ret;
}

int ObDMLStmt::get_from_tables(ObSqlBitSet<> &table_set) const
{
  int ret = OB_SUCCESS;
  int32_t bit_id = OB_INVALID_INDEX;
  for (int64_t i = 0; OB_SUCC(ret) && i < from_items_.count(); ++i) {
    if (from_items_.at(i).is_joined_) {
      const JoinedTable *table = get_joined_table(from_items_.at(i).table_id_);
      if (OB_ISNULL(table)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to get joined table", K(ret));
      }
      for (int64_t j = 0; OB_SUCC(ret) && j < table->single_table_ids_.count(); ++j) {
        uint64_t table_id = table->single_table_ids_.at(j);
        if (OB_UNLIKELY(OB_INVALID_INDEX == (bit_id = get_table_bit_index(table_id)))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("invalid table bit index", K(ret), K(table_id), K(bit_id));
        } else if (OB_FAIL(table_set.add_member(bit_id))) {
          LOG_WARN("failed to add member", K(ret));
        }
      }
    } else if (OB_UNLIKELY(OB_INVALID_INDEX ==
                                  (bit_id = get_table_bit_index(from_items_.at(i).table_id_)))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid table bit index", K(ret), K(from_items_.at(i).table_id_), K(bit_id));
    } else if (OB_FAIL(table_set.add_member(bit_id))) {
      LOG_WARN("failed to add member", K(ret));
    }
  }
  return ret;
}

ColumnItem *ObDMLStmt::get_column_item(uint64_t table_id, const ObString &col_name)
{
  ColumnItem *item = NULL;
  common::ObCollationType cs_type = common::CS_TYPE_UTF8MB4_GENERAL_CI;
  if (lib::is_oracle_mode()) {
    cs_type = common::CS_TYPE_UTF8MB4_BIN;
  }
  for (int64_t i = 0; i < column_items_.count(); ++i) {
    if (table_id == column_items_[i].table_id_
        && (0 == ObCharset::strcmp(cs_type, col_name, column_items_[i].column_name_))) {
      item = &column_items_.at(i);
      break;
    }
  }
  return item;
}

ColumnItem *ObDMLStmt::get_column_item(uint64_t table_id, uint64_t column_id)
{
  ColumnItem *item = NULL;
  for (int64_t i = 0; i < column_items_.count(); ++i) {
    if (table_id == column_items_[i].table_id_
        && column_id == column_items_[i].column_id_) {
      item = &column_items_.at(i);
      break;
    }
  }
  return item;
}

int ObDMLStmt::add_column_item(ObIArray<ColumnItem> &column_items)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_items.count(); i++) {
    if (OB_FAIL(add_column_item(column_items.at(i)))) {
      LOG_WARN("failed to add column item", K(ret));
    } else { /*do nothing*/ }
  }
  return ret;
}

int ObDMLStmt::add_column_item(ColumnItem &column_item)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(column_item.expr_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("column item isn't init");
  } else {
    column_item.expr_->set_explicited_reference();
    column_item.expr_->set_expr_level(current_level_);
    column_item.expr_->get_relation_ids().reuse();
    if (OB_FAIL(column_item.expr_->add_relation_id(get_table_bit_index(column_item.expr_->get_table_id())))) {
      LOG_WARN("add relation id to expr failed", K(ret));
    } else if (OB_FAIL(column_items_.push_back(column_item))) {
      LOG_WARN("push back column item failed", K(ret));
    } else {
      LOG_DEBUG("add_column_item", K(column_item), KP(this), KPC(column_item.expr_), K(column_items_.count()));
    }
  }
  return ret;
}

int ObDMLStmt::remove_column_item(uint64_t table_id, uint64_t column_id)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_items_.count(); i++) {
    if (table_id == column_items_.at(i).table_id_
        && column_id == column_items_.at(i).column_id_) {
      if (OB_FAIL(column_items_.remove(i))) {
        LOG_WARN("failed to remove column_items", K(ret));
      }
      break;
    }
  }
  return ret;
}

int ObDMLStmt::remove_column_item(ObIArray<uint64_t> &table_ids)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < table_ids.count(); ++i) {
    ret = remove_column_item(table_ids.at(i));
  }
  return ret;
}

int ObDMLStmt::remove_column_item(uint64_t table_id)
{
  int ret = OB_SUCCESS;
  for (int64_t i = column_items_.count() - 1; OB_SUCC(ret) && i >= 0; i--) {
    if (table_id == column_items_.at(i).table_id_) {
      if (OB_FAIL(column_items_.remove(i))) {
        LOG_WARN("failed to remove column item", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::remove_column_item(const ObRawExpr *column_expr)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_items_.count(); i++) {
    if (column_expr == column_items_.at(i).expr_) {
      if (OB_FAIL(column_items_.remove(i))) {
        LOG_WARN("failed to remove column_items", K(ret));
      }
      break;
    }
  }
  return ret;
}

int ObDMLStmt::remove_column_item(const ObIArray<ObRawExpr *> &column_exprs)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_exprs.count(); i++) {
    ret = remove_column_item(column_exprs.at(i));
  }
  return ret;
}


int ObDMLStmt::remove_joined_table_item(const ObIArray<JoinedTable*> &tables)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < tables.count(); i++) {
    ret = remove_joined_table_item(tables.at(i));
  }
  return ret;
}

int ObDMLStmt::remove_joined_table_item(const JoinedTable *joined_table)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < joined_tables_.count(); i++) {
    if (joined_table == joined_tables_.at(i)) {
      if (OB_FAIL(joined_tables_.remove(i))) {
        LOG_WARN("failed to remove joined table item", K(ret));
      }
      break;
    }
  }
  return ret;
}

int ObDMLStmt::add_subquery_ref(ObQueryRefRawExpr *query_ref)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(query_ref)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("stmt is null");
  } else if (OB_FAIL(subquery_exprs_.push_back(query_ref))) {
    LOG_WARN("push back subquery ref failed", K(ret));
  } else {
    query_ref->set_explicited_reference();
    query_ref->set_ref_id(subquery_exprs_.count() - 1);
  }
  return ret;
}

int ObDMLStmt::get_child_stmts(ObIArray<ObSelectStmt*> &child_stmts) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < get_table_size(); ++i) {
    const TableItem *table_item = get_table_item(i);
    if (OB_ISNULL(table_item)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table_item is null", K(i));
    } else if (table_item->is_generated_table()) {
      if (OB_FAIL(child_stmts.push_back(table_item->ref_query_))) {
        LOG_WARN("store child stmt failed", K(ret));
      }
    }
  }
  for (int64_t j = 0; OB_SUCC(ret) && j < get_subquery_expr_size(); ++j) {
    ObQueryRefRawExpr *subquery_ref = subquery_exprs_.at(j);
    if (OB_ISNULL(subquery_ref) ||
        OB_ISNULL(subquery_ref->get_ref_stmt())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("subquery reference is null", K(subquery_ref));
    } else if (OB_FAIL(child_stmts.push_back(subquery_ref->get_ref_stmt()))) {
      LOG_WARN("stored subquery reference stmt failed", K(ret));
    }
  }
  return ret;
}

int ObDMLStmt::set_child_stmt(const int64_t child_num, ObSelectStmt* child_stmt)
{
  int ret = OB_SUCCESS;
  int64_t child_size = 0;
  if (OB_ISNULL(child_stmt)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("null stmt", K(ret));
  } else if (OB_FAIL(get_child_stmt_size(child_size))) {
    LOG_WARN("failed to get child size", K(ret));
  } else if (OB_UNLIKELY(child_num < 0) ||
             OB_UNLIKELY(child_num >= child_size)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected child number", K(child_num), K(child_size), K(ret));
  } else {
    int pos = 0;
    bool is_find = false;
    for (int64_t i = 0; OB_SUCC(ret) && !is_find && i < get_table_size(); ++i) {
      TableItem *table_item = get_table_item(i);
      if (OB_ISNULL(table_item)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("table_item is null", K(i));
      } else if (table_item->is_generated_table()) {
        if (child_num == pos) {
          is_find = true;
          table_item->ref_query_ = child_stmt;
        } else {
          pos++;
        }
      } else { /*do nothing*/ }
    }
    for (int64_t j = 0; OB_SUCC(ret) && !is_find && j < get_subquery_expr_size(); ++j) {
      ObQueryRefRawExpr *subquery_ref = subquery_exprs_.at(j);
      if (OB_ISNULL(subquery_ref)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("subquery reference is null", K(subquery_ref));
      } else if (child_num == pos) {
        is_find = true;
        subquery_ref->set_ref_stmt(child_stmt);
      } else {
        pos++;
      }
    }
  }
  return ret;
}

int ObDMLStmt::get_child_stmt_size(int64_t &child_size) const
{
  int ret = OB_SUCCESS;
  ObArray<ObSelectStmt*> child_stmts;
  if (OB_FAIL(get_child_stmts(child_stmts))) {
    LOG_ERROR("get child stmts failed", K(ret));
  } else {
    child_size = child_stmts.count();
  }
  return ret;
}

bool ObDMLStmt::has_link_table() const
{
  bool bret = false;
  for (int i = 0; !bret && i < table_items_.count(); i++) {
    if (OB_NOT_NULL(table_items_.at(i))) {
      if (table_items_.at(i)->is_generated_table() || table_items_.at(i)->is_temp_table()) {
        if (OB_NOT_NULL(table_items_.at(i)->ref_query_)) {
          bret = table_items_.at(i)->ref_query_->has_link_table();
        }
      } else {
        bret = table_items_.at(i)->is_link_table();
      }
    }
  }
  return bret;
}

int ObDMLStmt::get_relation_exprs(ObIArray<ObRawExpr*> &rel_array, int32_t ignore_scope /* = 0*/) const
{
  int ret = OB_SUCCESS;
  FastRelExprChecker expr_checker(rel_array, ignore_scope);
  if (OB_FAIL(const_cast<ObDMLStmt *>(this)->inner_get_relation_exprs(expr_checker))) {
    LOG_WARN("get relation exprs failed", K(ret));
  }
  return ret;
}

int ObDMLStmt::get_relation_exprs(ObIArray<ObRawExprPointer> &rel_array, int32_t ignore_scope/* = 0*/)
{
  int ret = OB_SUCCESS;
  RelExprPointerChecker expr_checker(rel_array, ignore_scope);
  if (OB_FAIL(expr_checker.init())) {
    LOG_WARN("init relexpr checker failed", K(ret));
  } else if (OB_FAIL(inner_get_relation_exprs(expr_checker))) {
    LOG_WARN("get relation exprs failed", K(ret));
  }
  return ret;
}

int ObDMLStmt::get_relation_exprs_for_enum_set_wrapper(ObIArray<ObRawExpr*> &rel_array)
{
  int ret = OB_SUCCESS;
  RelExprChecker expr_checker(rel_array);
  if (OB_FAIL(expr_checker.init())) {
    LOG_WARN("init expr checker failed", K(ret));
  } else if (OB_FAIL(inner_get_relation_exprs_for_wrapper(expr_checker))) {
    LOG_WARN("get relation exprs failed", K(ret));
  }
  return ret;
}

const TableItem *ObDMLStmt::get_table_item_in_all_namespace(uint64_t table_id) const
{
  const TableItem *table_item = NULL;
  const ObDMLStmt *cur_stmt = this;
  do {
    if (NULL != (table_item = cur_stmt->get_table_item_by_id(table_id))) {
      break;
    }
    cur_stmt = cur_stmt->parent_namespace_stmt_;
  } while (cur_stmt != NULL);

  return table_item;
}

ColumnItem *ObDMLStmt::get_column_item_by_id(uint64_t table_id, uint64_t column_id) const
{
  const ColumnItem *column_item = NULL;
  int64_t num = column_items_.count();
  for (int64_t i = 0; i < num; i++) {
    if (table_id == column_items_[i].table_id_ && column_id == column_items_[i].column_id_) {
      column_item = &column_items_.at(i);
      break;
    }
  }
  //在本层查询没有找到对应的ColumnItem，到父查询中去查询对应的column item
  if (NULL == column_item && NULL != parent_namespace_stmt_) {
    column_item = parent_namespace_stmt_->get_column_item_by_id(table_id, column_id);
  }
  return const_cast<ColumnItem *>(column_item);
}

const ColumnItem *ObDMLStmt::get_column_item_by_base_id(uint64_t table_id, uint64_t base_column_id) const
{
  const ColumnItem *column_item = NULL;
  int64_t num = column_items_.count();
  for (int64_t i = 0; i < num; i++) {
    if (table_id == column_items_[i].table_id_ && base_column_id == column_items_[i].base_cid_) {
      column_item = &column_items_.at(i);
      break;
    }
  }
  //can not found the column item in my namespace, find it in the parent namespace
  if (NULL == column_item && NULL != parent_namespace_stmt_) {
    column_item = parent_namespace_stmt_->get_column_item_by_base_id(table_id, base_column_id);
  }
  return column_item;
}

ObColumnRefRawExpr *ObDMLStmt::get_column_expr_by_id(uint64_t table_id, uint64_t column_id) const
{
  ObColumnRefRawExpr *ref_expr = NULL;
  ColumnItem *column_item = get_column_item_by_id(table_id, column_id);
  if (column_item != NULL) {
    ref_expr = column_item->expr_;
  }
  return ref_expr;
}

int ObDMLStmt::get_column_exprs(uint64_t table_id, ObIArray<ObColumnRefRawExpr *> &table_cols) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_items_.count(); ++i) {
    if (column_items_.at(i).table_id_ == table_id) {
      if (OB_FAIL(table_cols.push_back(column_items_.at(i).expr_))) {
        LOG_WARN("failed to push back column exprs", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::get_column_exprs(uint64_t table_id, ObIArray<ObRawExpr*> &table_cols) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_items_.count(); ++i) {
    if (column_items_.at(i).table_id_ == table_id) {
      if (OB_FAIL(table_cols.push_back(column_items_.at(i).expr_))) {
        LOG_WARN("failed to push back column exprs", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::get_column_exprs(ObIArray<TableItem *> &table_items,
                                ObIArray<ObRawExpr *> &column_exprs) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < table_items.count(); ++i) {
    uint64_t table_id = table_items.at(i)->table_id_;
    for (int64_t j = 0; OB_SUCC(ret) && j < column_items_.count(); ++j) {
      if (column_items_.at(j).table_id_ == table_id) {
        if (OB_FAIL(column_exprs.push_back(column_items_.at(j).expr_))) {
          LOG_WARN("failed to push back column exprs", K(ret));
        }
      }
    }
  }
  return ret;
}

int64_t ObDMLStmt::get_from_item_idx(uint64_t table_id) const
{
  int64_t idx = -1;
  for (int64_t i = 0; i < from_items_.count(); ++i) {
    if (table_id == from_items_.at(i).table_id_) {
      idx = i;
      break;
    }
  }
  return idx;
}

int ObDMLStmt::check_if_contain_inner_table(bool &is_contain_inner_table) const
{
  int ret = OB_SUCCESS;
  is_contain_inner_table = false;
  for (int64_t i = 0; OB_SUCC(ret) && !is_contain_inner_table && i < table_items_.count(); ++i) {
    TableItem *table_item = table_items_.at(i);
    if (OB_ISNULL(table_item)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("table item is NULL", K(ret), K(i), K(table_items_.count()));
    } else if (is_inner_table(table_item->ref_id_)) {
      is_contain_inner_table = true;
    }
  }
  if (OB_SUCC(ret) && !is_contain_inner_table) {
    ObSEArray<ObSelectStmt*, 16> child_stmts;
    if (OB_FAIL(get_child_stmts(child_stmts))) {
      LOG_ERROR("get child stmt failed", K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && !is_contain_inner_table && i < child_stmts.count(); ++i) {
        ObSelectStmt *sub_stmt = child_stmts.at(i);
        if (OB_ISNULL(sub_stmt)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("sub stmt is null", K(ret));
        } else if (OB_FAIL(SMART_CALL(sub_stmt->check_if_contain_inner_table(
                                        is_contain_inner_table)))) {
          LOG_WARN("check sub stmt whether has is table failed", K(ret));
        }
      }
    }
  }
  return ret;
}

bool ObDMLStmt::has_for_update() const
{
  bool bret = false;
  for (int64_t i = 0; !bret && i < table_items_.count(); ++i) {
    const TableItem *table_item = table_items_.at(i);
    if (table_item != NULL && table_item->for_update_) {
      bret = true;
    }
  }
  return bret;
}

int ObDMLStmt::check_if_contain_select_for_update(bool &is_contain_select_for_update) const
{
  int ret = OB_SUCCESS;
  ObSEArray<ObSelectStmt*, 16> child_stmts;
  is_contain_select_for_update = false;
  if (has_for_update()) {
    is_contain_select_for_update = true;
  } else if (OB_FAIL(get_child_stmts(child_stmts))) {
    LOG_WARN("get child stmts failed", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && !is_contain_select_for_update && i < child_stmts.count(); ++i) {
      ObSelectStmt *sub_stmt = child_stmts.at(i);
      if (OB_ISNULL(sub_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("sub stmt is null", K(ret));
      } else if (OB_FAIL(SMART_CALL(sub_stmt->check_if_contain_select_for_update(is_contain_select_for_update)))) {
        LOG_WARN("check sub stmt whether has select for update failed", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::check_if_table_exists(uint64_t table_id, bool &is_existed) const
{
  int ret = OB_SUCCESS;
  is_existed = false;
  ObSEArray<ObSelectStmt*, 4> child_stmts;
  bool is_stack_overflow = false;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("failed to check stack overflow", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("too deep recursive", K(ret), K(is_stack_overflow));
  } else if (OB_FAIL(get_child_stmts(child_stmts))) {
    LOG_WARN("failed to get child stmts.", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && !is_existed && i < child_stmts.count(); i++) {
      if (OB_ISNULL(child_stmts.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("child stmt is null.", K(ret));
      } else if (OB_FAIL(SMART_CALL(child_stmts.at(i)->check_if_table_exists(table_id, is_existed)))) {
        LOG_WARN("failed to check if all virtual tables.", K(ret));
      } else { /* do nothing. */ }
    }
    for (int64_t i = 0; OB_SUCC(ret) && !is_existed && i < table_items_.count(); i++) {
      if (OB_ISNULL(table_items_.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("table item is null.", K(ret));
      } else if (table_id == table_items_.at(i)->ref_id_) {
        is_existed = true;
      } else if (!table_items_.at(i)->is_temp_table()) {
        //do nothing
      } else if (OB_ISNULL(table_items_.at(i)->ref_query_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("table ref query is null.", K(ret));
      } else if (OB_FAIL(SMART_CALL(table_items_.at(i)->ref_query_->check_if_table_exists(table_id, is_existed)))) {
        LOG_WARN("failed to check if all virtual tables.", K(ret));
      } else { /* do nothing. */ }
    }
  }
  return ret;
}

int ObDMLStmt::has_special_expr(const ObExprInfoFlag flag, bool &has) const
{
  int ret = OB_SUCCESS;
  ObSEArray<ObRawExpr*, 16> exprs;
  has = false;
  if (OB_FAIL(get_relation_exprs(exprs))) {
    LOG_WARN("failed to get relation exprs", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && !has && i < exprs.count(); i++) {
      if (OB_ISNULL(exprs.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else if (exprs.at(i)->has_flag(flag)) {
        has = true;
      }
    }
  }
  return ret;
}

int ObDMLStmt::rebuild_tables_hash()
{
  int ret = OB_SUCCESS;
  TableItem *ti = NULL;
  ObSEArray<uint64_t, 4> table_id_list;
  ObSEArray<int64_t, 4> bit_index_map;
  // dump old table id - rel id map
  for (int64_t i = 0; OB_SUCC(ret) && i < tables_hash_.get_column_num(); ++i) {
    uint64_t tid = OB_INVALID_ID;
    uint64_t cid = OB_INVALID_ID;
    if (OB_FAIL(tables_hash_.get_tid_cid(i, tid, cid))) {
      LOG_WARN("failed to get tid cid", K(ret));
    } else if (OB_FAIL(table_id_list.push_back(tid))) {
      LOG_WARN("failed to push back table id", K(ret));
    }
  }
  tables_hash_.reset();
  if (OB_FAIL(set_table_bit_index(OB_INVALID_ID))) {
    LOG_WARN("fail to add table_id to hash table", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table_items_.count(); i++) {
    if (OB_ISNULL(ti = table_items_.at(i))) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid argument", K(table_items_), K(ret));
    } else if (OB_FAIL(set_table_bit_index(ti->table_id_))) {
      LOG_WARN("fail to add table_id to hash table", K(ret), K(ti), K(table_items_));
    }
  }
  // create old rel id - new rel id map
  for (int64_t i = 0; OB_SUCC(ret) && i < table_id_list.count(); ++i) {
    if (OB_FAIL(bit_index_map.push_back(get_table_bit_index(table_id_list.at(i))))) {
      LOG_WARN("failed to push back new bit index", K(ret));
    }
  }
  return ret;
}

int ObDMLStmt::update_rel_ids(ObRelIds &rel_ids, const ObIArray<int64_t> &bit_index_map)
{
  int ret = OB_SUCCESS;
  ObSEArray<int64_t, 4> old_bit_index;
  if (OB_FAIL(rel_ids.to_array(old_bit_index))) {
    LOG_WARN("failed to convert bit set to bit index", K(ret));
  } else {
    rel_ids.reset();
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < old_bit_index.count(); ++i) {
    int64_t old_index = old_bit_index.at(i);
    int64_t new_index = bit_index_map.at(old_index);
    if (OB_INVALID_INDEX == new_index) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table index is invalid", K(ret), K(old_index));
    } else if (OB_FAIL(rel_ids.add_member(new_index))) {
      LOG_WARN("failed to add new table index", K(ret));
    }
  }
  return ret;
}

int ObDMLStmt::update_column_item_rel_id()
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_items_.count(); i ++) {
    ColumnItem &ci = column_items_.at(i);
    if (OB_ISNULL(ci.expr_)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid argument", K(ret), K(ci));
    } else {
      ci.expr_->get_relation_ids().reuse();
      int64_t rel_id = get_table_bit_index(ci.expr_->get_table_id());
      if (rel_id <= 0 || rel_id > table_items_.count()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid argument", K(ret), K(rel_id), K(table_items_.count()));
      } else if (OB_FAIL(ci.expr_->add_relation_id(rel_id))) {
        LOG_WARN("fail to add relation id", K(rel_id), K(ret));
      }
    }
  }

  return ret;
}

int ObDMLStmt::reset_from_item(const common::ObIArray<FromItem> &from_items)
{
  return from_items_.assign(from_items);
}

int ObDMLStmt::reset_table_item(const common::ObIArray<TableItem*> &table_items)
{
  return table_items_.assign(table_items);
}

void ObDMLStmt::clear_column_items()
{
  column_items_.reset();
}

int ObDMLStmt::remove_subquery_expr(const ObRawExpr *expr)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < subquery_exprs_.count(); i++) {
      if (expr == subquery_exprs_.at(i)) {
        if (OB_FAIL(subquery_exprs_.remove(i))) {
          LOG_WARN("failed to remove expr", K(ret));
        }
        break;
      }
    }
  }
  return ret;
}

/*
 * extract all query_ref_expr from stmt's relation exprs, and push them into stmt's query_ref_exprs_
 */
int ObDMLStmt::adjust_subquery_list()
{
  int ret = OB_SUCCESS;
  ObSEArray<ObRawExpr *, 8> relation_exprs;
  if (OB_FAIL(get_relation_exprs(relation_exprs))) {
    LOG_WARN("failed to get relation exprs", K(ret));
  } else if (FALSE_IT(subquery_exprs_.reset())) {
  } else if (OB_FAIL(ObTransformUtils::extract_query_ref_expr(relation_exprs, subquery_exprs_))) {
    LOG_WARN("failed to extract query ref expr", K(ret));
  }
  return ret;
}

int ObDMLStmt::get_stmt_equal_sets(EqualSets &equal_sets,
                                   ObIAllocator &allocator,
                                   const bool is_strict,
                                   const int check_scope) const
{
  int ret = OB_SUCCESS;
  ObSEArray<ObRawExpr *, 8> equal_set_conditions;
  if (OB_FAIL(get_equal_set_conditions(equal_set_conditions, is_strict, check_scope))) {
    LOG_WARN("failed to get equal set conditions", K(ret));
  } else if (equal_set_conditions.count() > 0) {
    if (OB_FAIL(ObEqualAnalysis::compute_equal_set(&allocator,
                                                   equal_set_conditions,
                                                   equal_sets))) {
      LOG_WARN("failed to compute equal set", K(ret));
    }
  } else { /*do nothing*/ }
  return ret;
}

// equal set conditions:
// 1. where conditions
// 2. join conditions in joined tables
// 3. join conditions in semi infos
int ObDMLStmt::get_equal_set_conditions(ObIArray<ObRawExpr *> &conditions,
                                        const bool is_strict,
                                        const int check_scope) const
{
  int ret = OB_SUCCESS;
  bool check_where = check_scope & SCOPE_WHERE;
  if (!check_where) {
  } else if (OB_FAIL(append(conditions, condition_exprs_))) {
    LOG_WARN("failed to append conditions", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < joined_tables_.count(); ++i) {
      if (OB_FAIL(extract_equal_condition_from_joined_table(joined_tables_.at(i),
                                                            conditions,
                                                            is_strict))) {
        LOG_WARN("failed to extract equal condition from joined table", K(ret));
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < semi_infos_.count(); ++i) {
      if (OB_ISNULL(semi_infos_.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected null", K(ret));
      } else if (semi_infos_.at(i)->is_anti_join()) {
        /* do nothing */
      } else if (OB_FAIL(append(conditions, semi_infos_.at(i)->semi_conditions_))) {
        LOG_WARN("failed to append conditions", K(ret));
      }
    }
  }
  return ret;
}

// where conditions:
// 1. where conditions
// 2. join conditions in joined tables
// 3. join conditions in semi/anti join
int ObDMLStmt::get_where_scope_conditions(ObIArray<ObRawExpr *> &conditions) const
{
  int ret = OB_SUCCESS;
  RelExprChecker expr_checker(conditions);
  if (OB_FAIL(expr_checker.init())) {
    LOG_WARN("init relexpr checker failed", K(ret));
  } else if (OB_FAIL(append(conditions, condition_exprs_))) {
    LOG_WARN("failed to append conditions", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < joined_tables_.count(); ++i) {
      if (OB_FAIL(get_join_condition_expr(*const_cast<JoinedTable*>(joined_tables_.at(i)),expr_checker))) {
        LOG_WARN("failed to get join condition expr ", K(ret));
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < semi_infos_.count(); ++i) {
      if (OB_ISNULL(semi_infos_.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected null", K(ret));
      } else if (OB_FAIL(append(conditions, semi_infos_.at(i)->semi_conditions_))) {
        LOG_WARN("failed to append conditions", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::extract_equal_condition_from_joined_table(const TableItem *table,
                                                         ObIArray<ObRawExpr *> &conditions,
                                                         const bool is_strict)
{
  int ret = OB_SUCCESS;
  bool is_stack_overflow = false;
  bool check_left = false;
  bool check_right = false;
  bool check_this = false;

  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("check stack overflow failed", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("too deep recursive", K(ret));
  } else if (OB_ISNULL(table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("NULL table item", K(ret));
  } else if (table->is_joined_table()) {
    const JoinedTable *joined_table = static_cast<const JoinedTable *>(table);
    if (joined_table->is_inner_join()) {
      check_left = true;
      check_right = true;
      check_this = true;
    } else if (LEFT_OUTER_JOIN == joined_table->joined_type_) {
      check_left = true;
      check_right = !is_strict;
      check_this = !is_strict;
    } else if (RIGHT_OUTER_JOIN == joined_table->joined_type_) {
      check_left = !is_strict;
      check_right = true;
      check_this = !is_strict;
    } else {
      check_left = !is_strict;
      check_right = !is_strict;
      check_this = !is_strict;
    }

    if (check_this) {
      for (int64_t i = 0; OB_SUCC(ret) && i < joined_table->get_join_conditions().count(); ++i) {
        if (OB_FAIL(conditions.push_back(joined_table->get_join_conditions().at(i)))) {
          LOG_WARN("failed to push back expr", K(ret));
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (check_left &&
               OB_FAIL(SMART_CALL(extract_equal_condition_from_joined_table(joined_table->left_table_,
                                                                            conditions,
                                                                            is_strict)))) {
      LOG_WARN("failed to extract equal condition from join table", K(ret));
    } else if (check_right &&
               OB_FAIL(SMART_CALL(extract_equal_condition_from_joined_table(joined_table->right_table_,
                                                                            conditions,
                                                                            is_strict)))) {
      LOG_WARN("failed to extract equal condition from join table", K(ret));
    }
  } else { /* do nothing */ }
  return ret;
}

int ObDMLStmt::get_rownum_expr(ObRawExpr *&expr) const
{
  int ret = OB_SUCCESS;
  expr = NULL;
  ObRawExpr *cur_expr = NULL;
  bool find = false;
  for (int64_t i = 0; OB_SUCC(ret) && !find && i < pseudo_column_like_exprs_.count(); ++i) {
    if (OB_ISNULL(cur_expr = pseudo_column_like_exprs_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get null expr", K(ret));
    } else if (T_FUN_SYS_ROWNUM == cur_expr->get_expr_type()) {
      expr = cur_expr;
      find = true;
    }
  }
  return ret;
}

int ObDMLStmt::has_rownum(bool &has_rownum) const
{
  int ret = OB_SUCCESS;
  ObRawExpr *rownum_expr = NULL;
  if (OB_FAIL(get_rownum_expr(rownum_expr))) {
    LOG_WARN("failed to get rownum expr", K(ret));
  } else {
    has_rownum = (rownum_expr != NULL);
  }
  return ret;
}

bool ObDMLStmt::has_ora_rowscn() const
{
  bool has = false;
  for (int64_t i = 0; !has && i < pseudo_column_like_exprs_.count(); ++i) {
    has = NULL != pseudo_column_like_exprs_.at(i) &&
          T_ORA_ROWSCN == pseudo_column_like_exprs_.at(i)->get_expr_type();
  }
  return has;
}

int ObDMLStmt::get_sequence_expr(ObRawExpr *&expr,
                                 const ObString seq_name, // sequence object name
                                 const ObString seq_action, // NEXTVAL or CURRVAL
                                 const uint64_t seq_id) const
{
  int ret = OB_SUCCESS;
  expr = NULL;
  ObRawExpr *cur_expr = NULL;
  bool find = false;
  for (int64_t i = 0; OB_SUCC(ret) && !find && i < pseudo_column_like_exprs_.count(); ++i) {
    if (OB_ISNULL(cur_expr = pseudo_column_like_exprs_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get null expr", K(ret));
    } else if (T_FUN_SYS_SEQ_NEXTVAL == cur_expr->get_expr_type()) {
      ObSequenceRawExpr *seq_expr = static_cast<ObSequenceRawExpr *>(cur_expr);
      if (seq_expr->get_name() == seq_name && seq_expr->get_action() == seq_action &&
          seq_expr->get_sequence_id() == seq_id) {
        expr = cur_expr;
        find = true;
      }
    }
  }
  return ret;
}

int ObDMLStmt::get_sequence_exprs(ObIArray<ObRawExpr *> &exprs) const
{
  int ret = OB_SUCCESS;
  ObRawExpr *cur_expr = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && i < pseudo_column_like_exprs_.count(); ++i) {
    if (OB_ISNULL(cur_expr = pseudo_column_like_exprs_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get null expr", K(ret));
    } else if (T_FUN_SYS_SEQ_NEXTVAL == cur_expr->get_expr_type()) {
      if (OB_FAIL(exprs.push_back(cur_expr))) {
        LOG_WARN("fail push expr", K(ret), K(i));
      }
    }
  }
  return ret;
}

int ObDMLStmt::inner_get_share_exprs(ObIArray<ObRawExpr *> &candi_share_exprs) const
{
  int ret = OB_SUCCESS;
  /**
   * column, query ref, pseudo_column can be shared
   */
  if (OB_FAIL(get_column_exprs(candi_share_exprs))) {
    LOG_WARN("failed to append column exprs", K(ret));
  } else if (OB_FAIL(append(candi_share_exprs, get_pseudo_column_like_exprs()))) {
    LOG_WARN("failed to append pseduo column like exprs", K(ret));
  } else if (OB_FAIL(append(candi_share_exprs, get_subquery_exprs()))) {
    LOG_WARN("failed to append subquery exprs", K(ret));
  } else if (OB_FAIL(append(candi_share_exprs, get_onetime_exprs()))) {
    LOG_WARN("failed to append onetime exprs", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < get_subquery_exprs().count(); ++i) {
    ObQueryRefRawExpr *query_ref = NULL;
    if (OB_ISNULL(query_ref = get_subquery_exprs().at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("query ref is null", K(ret), K(query_ref));
    } else if (OB_FAIL(append(candi_share_exprs, query_ref->get_exec_params()))) {
      LOG_WARN("failed to append exec param expr", K(ret));
    }
  }
  return ret;
}

/**
 * has_ref_assign_user_var
 * 检查stmt及其child stmt中是否包含涉及到赋值操作的用户变量
 */
int ObDMLStmt::has_ref_assign_user_var(bool &has_ref_user_var) const
{
  int ret = OB_SUCCESS;
  has_ref_user_var = false;
  if (OB_ISNULL(query_ctx_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else if (query_ctx_->all_user_variable_.empty()) {
    // do nothing
  } else {
    // quick check
    bool find = false;
    for (int64_t i = 0; OB_SUCC(ret) && !find && i < query_ctx_->all_user_variable_.count(); ++i) {
      const ObUserVarIdentRawExpr *cur_expr = query_ctx_->all_user_variable_.at(i);
      if (OB_ISNULL(cur_expr)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get null expr", K(ret));
      } else if (cur_expr->get_is_contain_assign() || cur_expr->get_query_has_udf()) {
        find = true;
      }
    }

    if (OB_SUCC(ret)){
      if (!find) {
        // no user variable assignment in query
      } else if (OB_FAIL(recursive_check_has_ref_assign_user_var(has_ref_user_var))) {
        LOG_WARN("failed to recursive check has assignment ref user var", K(ret));
      }
    }
  }
  return ret;
}

int ObDMLStmt::recursive_check_has_ref_assign_user_var(bool &has_ref_user_var) const
{
  int ret = OB_SUCCESS;
  bool is_stack_overflow = false;
  has_ref_user_var = false;
  if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
    LOG_WARN("failed to check stack overflow", K(ret));
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("too deep recursive", K(ret), K(is_stack_overflow));
  }
  for (int64_t i = 0; OB_SUCC(ret) && !has_ref_user_var && i < get_user_var_size(); ++i) {
    const ObUserVarIdentRawExpr *cur_expr = get_user_vars().at(i);
    if (OB_ISNULL(cur_expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get null expr", K(ret));
    } else if (cur_expr->get_is_contain_assign() || cur_expr->get_query_has_udf()) {
      has_ref_user_var = true;
    }
  }
  if (OB_SUCC(ret) && !has_ref_user_var) {
    ObSEArray<ObSelectStmt *, 4> child_stmts;
    if (OB_FAIL(get_child_stmts(child_stmts))) {
      LOG_WARN("failed to get child stmts", K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && !has_ref_user_var && i < child_stmts.count(); ++i) {
        if (OB_ISNULL(child_stmts.at(i))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get unexpected null", K(ret));
        } else if (child_stmts.at(i)->recursive_check_has_ref_assign_user_var(has_ref_user_var)) {
          LOG_WARN("failed to recursive check has assignment ref user var", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObDMLStmt::get_temp_table_ids(ObIArray<uint64_t> &temp_table_ids)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObSelectStmt*, 8> child_stmts;
  if (OB_FAIL(get_child_stmts(child_stmts))) {
    LOG_WARN("failed to get child stmts", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < child_stmts.count(); i++) {
      if (OB_ISNULL(child_stmts.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else if (OB_FAIL(SMART_CALL(child_stmts.at(i)->get_temp_table_ids(temp_table_ids)))) {
        LOG_WARN("failed to get temp table ids", K(ret));
      } else { /*do nothing*/ }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < table_items_.count(); i++) {
      if (OB_ISNULL(table_items_.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else if (!table_items_.at(i)->is_temp_table()) {
        /*do nothing*/
      } else if (OB_FAIL(add_var_to_array_no_dup(temp_table_ids, table_items_.at(i)->ref_id_))) {
        LOG_WARN("failed to add to array without duplicates", K(ret));
      } else { /*do nothing*/ }
    }
  }
  return ret;
}

int ObDMLStmt::collect_temp_table_infos(ObIArray<TempTableInfo> &temp_table_infos)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObSelectStmt*, 8> child_stmts;
  if (OB_FAIL(get_child_stmts(child_stmts))) {
    LOG_WARN("failed to get child stmts", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < get_table_size(); ++i) {
    TableItem *table = get_table_item(i);
    if (OB_ISNULL(table)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpect null table item", K(ret));
    } else if (!table->is_temp_table()) {
      //do nothing
    } else if (OB_ISNULL(table->ref_query_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpect null ref query", K(ret));
    } else {
      bool find = false;
      //找到对应的temp table集合
      for (int64_t j = 0; OB_SUCC(ret) && !find && j < temp_table_infos.count(); ++j) {
        TempTableInfo &info = temp_table_infos.at(j);
        if (table->ref_query_ == info.temp_table_query_) {
          if (OB_FAIL(info.table_items_.push_back(table))) {
            LOG_WARN("failed to push back table item", K(ret));
          } else {
            find = true;
          }
        }
      }
      if (OB_SUCC(ret) && !find) {
        TempTableInfo info;
        info.temp_table_query_ = table->ref_query_;
        if (OB_FAIL(SMART_CALL(table->ref_query_->collect_temp_table_infos(temp_table_infos)))) {
          LOG_WARN("failed to collect temp table infos", K(ret));
        } else if (OB_FAIL(info.table_items_.push_back(table))) {
          LOG_WARN("failed to push back table item", K(ret));
        } else if (OB_FAIL(temp_table_infos.push_back(info))) {
          LOG_WARN("failed to push back temp table info", K(ret));
        }
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < child_stmts.count(); ++i) {
    ObSelectStmt *stmt = child_stmts.at(i);
    if (OB_ISNULL(stmt)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpect null stmt", K(ret));
    } else if (OB_FAIL(SMART_CALL(stmt->collect_temp_table_infos(temp_table_infos)))) {
      LOG_WARN("failed to collect temp table infos", K(ret));
    }
  }
  return ret;
}

int ObDMLStmt::get_ora_rowscn_column(const uint64_t table_id, ObPseudoColumnRawExpr *&ora_rowscn)
{
  int ret = OB_SUCCESS;
  ora_rowscn = NULL;
  ObRawExpr *expr = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && NULL == ora_rowscn && i < pseudo_column_like_exprs_.count(); ++i) {
    if (OB_ISNULL(expr = pseudo_column_like_exprs_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("expr is NULL", K(i), K(ret));
    } else if (T_ORA_ROWSCN == expr->get_expr_type() &&
               static_cast<ObPseudoColumnRawExpr *>(expr)->get_table_id() == table_id) {
      ora_rowscn = static_cast<ObPseudoColumnRawExpr *>(expr);
    }
  }
  return ret;
}

int ObDMLStmt::has_lob_column(int64_t table_id, bool &has_lob) const
{
  int ret = OB_SUCCESS;
  has_lob = false;
  for (int64_t i = 0; OB_SUCC(ret) && !has_lob && i < column_items_.count(); i++) {
    const ObColumnRefRawExpr *column_expr = NULL;
    if (OB_ISNULL(column_expr = column_items_.at(i).expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (column_expr->get_table_id() == table_id &&
               column_expr->get_result_type().is_lob_locator()) {
      has_lob = true;
    } else { /*do nothing*/ }
  }
  return ret;
}

int ObDMLStmt::get_stmt_rowid_exprs(ObIArray<ObRawExpr *> &rowid_exprs)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObRawExpr *, 8> relation_exprs;
  if (OB_FAIL(get_relation_exprs(relation_exprs))) {
    LOG_WARN("failed to get relation exprs", K(ret));
  } else if (OB_FAIL(ObTransformUtils::extract_rowid_exprs(relation_exprs, rowid_exprs))) {
    LOG_WARN("failed to check has rownum", K(ret));
  } else {/*do nothing*/}
  return ret;
}

int ObDMLStmt::check_and_get_same_rowid_expr(const ObRawExpr *expr, ObRawExpr *&same_rowid_expr)
{
  int ret = OB_SUCCESS;
  same_rowid_expr = NULL;
  ObSEArray<ObRawExpr *, 8> rowid_exprs;
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("expr is null", K(ret));
  } else if (OB_FAIL(get_stmt_rowid_exprs(rowid_exprs))) {
    LOG_WARN("failed to get stmt rowid expr");
  } else {
    bool is_existed = false;
    for (int64_t i = 0; OB_SUCC(ret) && !is_existed && i < rowid_exprs.count(); ++i) {
      if (OB_ISNULL(rowid_exprs.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("expr is null", K(ret));
      } else if (rowid_exprs.at(i)->same_as(*expr)) {
        is_existed = true;
        same_rowid_expr = rowid_exprs.at(i);
      }
    }
  }
  return ret;
}

int ObDMLStmt::has_virtual_generated_column(int64_t table_id, bool &has_virtual_col) const
{
  int ret = OB_SUCCESS;
  const ObColumnRefRawExpr *col_expr = NULL;
  has_virtual_col = false;
  for (int64_t i = 0; OB_SUCC(ret) && !has_virtual_col && i < column_items_.count(); ++i) {
    if (OB_ISNULL(col_expr = column_items_.at(i).expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret), K(col_expr));
    } else if (table_id == col_expr->get_table_id() &&
               col_expr->is_virtual_generated_column()) {
      has_virtual_col = true;
    }
  }
  return ret;
}

int ObDMLStmt::check_hint_table_matched_table_item(ObCollationType cs_type,
                                                   const ObTableInHint &hint_table,
                                                   bool &matched) const
{
  int ret = OB_SUCCESS;
  matched = false;
  for (int64_t i = 0; !matched && OB_SUCC(ret) && i < table_items_.count(); ++i) {
    if (OB_ISNULL(table_items_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else {
      matched = hint_table.is_match_table_item(cs_type, *table_items_.at(i));
    }
  }
  return ret;
}

int ObDMLStmt::CheckConstraintItem::deep_copy(ObIRawExprCopier &expr_copier,
                                              const CheckConstraintItem &other)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(expr_copier.copy(other.check_constraint_exprs_, check_constraint_exprs_))) {
    LOG_WARN("failed to copy check constraint exprs", K(ret));
  } else if (OB_FAIL(append(check_flags_, other.check_flags_))) {
    LOG_WARN("failed to append check flags", K(ret));
  } else {
    table_id_ = other.table_id_;
    ref_table_id_ = other.ref_table_id_;
  }
  return ret;
}

int ObDMLStmt::CheckConstraintItem::assign(const CheckConstraintItem &other)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_constraint_exprs_.assign(other.check_constraint_exprs_))) {
    LOG_WARN("failed to assign check constraint exprs", K(ret));
  } else if (OB_FAIL(check_flags_.assign(other.check_flags_))) {
    LOG_WARN("failed to assign check check flags", K(ret));
  } else {
    table_id_ = other.table_id_;
    ref_table_id_ = other.ref_table_id_;
  }
  return ret;
}

int ObDMLStmt::set_check_constraint_item(CheckConstraintItem &check_constraint_item)
{
  int ret = OB_SUCCESS;
  bool found = false;
  for (int64_t i = 0; !found && i < check_constraint_items_.count(); i ++) {
    if (check_constraint_item.table_id_ == check_constraint_items_.at(i).table_id_
        && check_constraint_item.ref_table_id_ == check_constraint_items_.at(i).ref_table_id_) {
      found = true;
    }
  }
  if (found) {
    LOG_TRACE("check constraint item exists", K(check_constraint_item), K(check_constraint_items_));
  } else if (OB_FAIL(check_constraint_items_.push_back(check_constraint_item))) {
    LOG_WARN("failed to push back", K(ret));
  }
  return ret;
}

int ObDMLStmt::remove_check_constraint_item(const uint64_t table_id)
{
  int ret = OB_SUCCESS;
  for (int64_t i = check_constraint_items_.count() - 1; OB_SUCC(ret) && i >= 0; --i) {
    if (table_id == check_constraint_items_.at(i).table_id_) {
      if (OB_FAIL(check_constraint_items_.remove(i))) {
        LOG_WARN("failed to remove check constraint item", K(ret), K(table_id));
      }
    }
  }
  return ret;
}

int ObDMLStmt::get_check_constraint_items(const uint64_t table_id,
                                          CheckConstraintItem &check_constraint_item)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < check_constraint_items_.count(); ++i) {
    if (table_id != check_constraint_items_.at(i).table_id_) {
      // do nothing
    } else if (OB_FAIL(check_constraint_item.assign(check_constraint_items_.at(i)))) {
      LOG_WARN("failed to assign check constraint item", K(ret));
    } else {
      break;
    }
  }
  return ret;
}

int ObDMLStmt::get_qb_name(ObString &qb_name) const
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(query_ctx_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(query_ctx_));
  } else if (OB_FAIL(query_ctx_->get_qb_name(get_stmt_id(), qb_name))) {
    LOG_WARN("failed to get qb name", K(ret), K(get_stmt_id()), K(query_ctx_->stmt_count_), K(*this));
  }
  return ret;
}

int64_t ObDMLStmt::get_CTE_table_size() const {
  int64_t size = 0;
  TableItem *tmp_table = NULL;
  for (int64_t i = 0; i < table_items_.count(); i++) {
    if (OB_ISNULL(tmp_table = table_items_.at(i))) {

    } else if (tmp_table->cte_type_ == TableItem::NORMAL_CTE ||
               tmp_table->cte_type_ == TableItem::RECURSIVE_CTE) {
      size++;
    }
  }
  return size;
}

int ObDMLStmt::get_CTE_table_items(ObIArray<TableItem *> &cte_table_items) const {
  int ret = OB_SUCCESS;
  TableItem *tmp_table = NULL;
  for (int64_t i = 0; i < table_items_.count(); i++) {
    if (OB_ISNULL(tmp_table = table_items_.at(i))) {

    } else if (tmp_table->cte_type_ != TableItem::NORMAL_CTE &&
               tmp_table->cte_type_ != TableItem::RECURSIVE_CTE) {
    } else {
      // if two cte tables have same ref_query_, then they are the same cte, don't add tmp table.
      bool is_duplicated = false;
      for (int64_t j = 0; OB_SUCC(ret) && !is_duplicated && j < cte_table_items.count(); j++) {
        if (OB_ISNULL(cte_table_items.at(j))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("table item shouldn't be null");
        } else if (cte_table_items.at(j)->ref_query_ == tmp_table->ref_query_) {
          is_duplicated = true;
        }
      }
      if (OB_SUCC(ret) && !is_duplicated) {
        if (OB_FAIL(add_var_to_array_no_dup(cte_table_items, tmp_table))) {
          LOG_WARN("add cte_table fail");
        }
      }
    }
  }
  return ret;
}

// get all cte items in both current stmt and child stmt.
// child cte has higher priority than parent cte.
int ObDMLStmt::get_all_CTE_table_items_recursive(ObIArray<TableItem *> &cte_table_items) const
{
  int ret = OB_SUCCESS;
  ObSEArray<ObSelectStmt *, 4> child_stmts;
  if (OB_FAIL(get_child_stmts(child_stmts))) {
    LOG_WARN("fail to get child stmt");
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < child_stmts.count(); i++) {
      if (OB_ISNULL(child_stmts.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the child stmt ptr is null");
      } else if (OB_FAIL(SMART_CALL(child_stmts.at(i)->
                                    get_all_CTE_table_items_recursive(cte_table_items)))) {
        LOG_WARN("fail to get all cte table items", KPC(child_stmts.at(i)));
      }
    }
  }
  // table item
  for (int64_t i = 0; OB_SUCC(ret) && i < get_table_size(); i++) {
    const TableItem *tmp_table = get_table_item(i);
    if (OB_ISNULL(tmp_table)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get null ptr in table items");
    } else if (tmp_table->is_temp_table() && OB_NOT_NULL(tmp_table->ref_query_)) {
      if (OB_FAIL(SMART_CALL((static_cast<ObSelectStmt*>(tmp_table->ref_query_)->
                              get_all_CTE_table_items_recursive(cte_table_items))))) {
        LOG_WARN("fail to get all cte table items", KPC(child_stmts.at(i)));
      }
    } else {
      //skip
    }
  }
  get_CTE_table_items(cte_table_items);
  return ret;
}

bool ObDMLStmt::is_hierarchical_query() const
{
  return is_select_stmt() ? (static_cast<const ObSelectStmt *>(this)->is_hierarchical_query())
                                  : false;
}

bool ObDMLStmt::is_set_stmt() const
{
  return is_select_stmt() ? (static_cast<const ObSelectStmt*>(this)->is_set_stmt()) : false;
}
