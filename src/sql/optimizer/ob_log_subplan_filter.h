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

#ifndef OCEANBASE_SQL_OB_LOG_SUBPLAN_FILTER_H_
#define OCEANBASE_SQL_OB_LOG_SUBPLAN_FILTER_H_

#include "sql/optimizer/ob_logical_operator.h"

namespace oceanbase
{
namespace sql
{
struct ObBasicCostInfo;
class ObLogSubPlanFilter : public ObLogicalOperator
{
public:
  ObLogSubPlanFilter(ObLogPlan &plan)
      : ObLogicalOperator(plan),
        dist_algo_(DIST_INVALID_METHOD),
        subquery_exprs_(),
        exec_params_(),
        onetime_exprs_(),
        init_plan_idxs_(),
        one_time_idxs_(),
        update_set_(false),
        enable_das_batch_rescans_(false)
  {}
  ~ObLogSubPlanFilter() {}
  virtual int est_cost() override;
  virtual int re_est_cost(EstimateCostInfo &param, double &card, double &cost) override;
  int inner_est_cost(double &first_child_card, double &op_cost);

  inline int add_subquery_exprs(const ObIArray<ObQueryRefRawExpr *> &query_exprs)
  {
    return append(subquery_exprs_, query_exprs);
  }

  inline int add_exec_params(const ObIArray<ObExecParamRawExpr *> &exec_params)
  {
    return append(exec_params_, exec_params);
  }
  /**
   *  Get the exec params
   */
  inline const common::ObIArray<ObExecParamRawExpr*> &get_exec_params() const { return exec_params_; }

  inline common::ObIArray<ObExecParamRawExpr *> &get_exec_params() { return exec_params_; }

  inline bool has_exec_params() const { return !exec_params_.empty(); }

  inline const common::ObIArray<ObExecParamRawExpr *> &get_onetime_exprs() const { return onetime_exprs_; }

  inline common::ObIArray<ObExecParamRawExpr *> &get_onetime_exprs() { return onetime_exprs_; }

  inline int add_onetime_exprs(const common::ObIArray<ObExecParamRawExpr*> &onetime_exprs)
  {
    return append(onetime_exprs_, onetime_exprs);
  }

  inline const common::ObBitSet<> &get_onetime_idxs() { return one_time_idxs_; }

  inline int add_onetime_idxs(const common::ObBitSet<> &one_time_idxs) { return one_time_idxs_.add_members(one_time_idxs); }

  inline const common::ObBitSet<> &get_initplan_idxs() { return init_plan_idxs_; }

  inline int add_initplan_idxs(const common::ObBitSet<> &init_plan_idxs) { return init_plan_idxs_.add_members(init_plan_idxs); }

  virtual int get_op_exprs(ObIArray<ObRawExpr*> &all_exprs) override;

  bool is_my_subquery_expr(const ObQueryRefRawExpr *query_expr);
  bool is_my_exec_expr(const ObRawExpr *expr);
  bool is_my_onetime_expr(const ObRawExpr *expr);

  int get_exists_style_exprs(ObIArray<ObRawExpr*> &subquery_exprs);

  int inner_replace_generated_agg_expr(
      const ObIArray<std::pair<ObRawExpr *, ObRawExpr *>   >&to_replace_exprs) override;

  virtual int print_my_plan_annotation(char *buf, int64_t &buf_len, int64_t &pos, ExplainType type);
  // 从子节点中抽取估算代价相关的信息，存入children_cost_info中
  int get_children_cost_info(double &first_child_refine_card, common::ObIArray<ObBasicCostInfo> &children_cost_info);
  void set_update_set(bool update_set)
  { update_set_ = update_set; }
  bool is_update_set() { return update_set_; }
  int allocate_granule_pre(AllocGIContext &ctx);
  int allocate_granule_post(AllocGIContext &ctx);
  virtual int compute_one_row_info() override;
  virtual int compute_sharding_info() override;
  inline DistAlgo get_distributed_algo() { return dist_algo_; }
  inline void set_distributed_algo(const DistAlgo set_dist_algo) { dist_algo_ = set_dist_algo; }
  
  int add_px_batch_rescan_flag(bool flag) { return enable_px_batch_rescans_.push_back(flag); }
  common::ObIArray<bool> &get_px_batch_rescans() {  return enable_px_batch_rescans_; }
  
  inline bool &get_enable_das_batch_rescans() { return enable_das_batch_rescans_; }
  int check_and_set_use_batch();
  int check_if_match_das_batch_rescan(bool &enable_das_batch_rescans);
  int check_if_match_das_batch_rescan(ObLogicalOperator *root,
                                      bool &enable_das_batch_rescans);
  int set_use_das_batch(ObLogicalOperator* root);

  int allocate_startup_expr_post() override;

  int allocate_subquery_id();
private:
  int extract_exist_style_subquery_exprs(ObRawExpr *expr,
                                         ObIArray<ObRawExpr*> &exist_style_exprs);
  int check_expr_contain_row_subquery(const ObRawExpr *expr,
                                         bool &contains);
protected:
  DistAlgo dist_algo_;
  common::ObSEArray<ObQueryRefRawExpr *, 8, common::ModulePageAllocator, true> subquery_exprs_;
  common::ObSEArray<ObExecParamRawExpr *, 8, common::ModulePageAllocator, true> exec_params_;
  common::ObSEArray<ObExecParamRawExpr *, 8, common::ModulePageAllocator, true> onetime_exprs_;

  //InitPlan idxs，InitPlan只算一次，需要存储结果
  common::ObBitSet<> init_plan_idxs_;
  //One-Time idxs，One-Time只算一次，不用存储结果
  common::ObBitSet<> one_time_idxs_;
  bool update_set_;
  common::ObSEArray<bool , 8, common::ModulePageAllocator, true> enable_px_batch_rescans_;
  bool enable_das_batch_rescans_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObLogSubPlanFilter);
};
} // end of namespace sql
} // end of namespace oceanbase


#endif // OCEANBASE_SQL_OB_LOG_SUBPLAN_FILTER_H_
