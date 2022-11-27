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

#ifndef OCEANBASE_SQL_OB_LOG_WINDOW_FUNCTION_H
#define OCEANBASE_SQL_OB_LOG_WINDOW_FUNCTION_H
#include "sql/optimizer/ob_logical_operator.h"
#include "sql/optimizer/ob_log_set.h"
namespace oceanbase
{
namespace sql
{
  class ObLogWindowFunction : public ObLogicalOperator
  {
  public:
    ObLogWindowFunction(ObLogPlan &plan)
        : ObLogicalOperator(plan),
        single_part_parallel_(false),
        range_dist_parallel_(false),
        rd_pby_sort_cnt_(0)
    {}
    virtual ~ObLogWindowFunction() {}
    virtual int print_my_plan_annotation(char *buf,
                                         int64_t &buf_len,
                                         int64_t &pos,
                                         ExplainType type);

    inline int add_window_expr(ObWinFunRawExpr *win_expr)
    { return win_exprs_.push_back(win_expr); }
    inline ObIArray<ObWinFunRawExpr *> &get_window_exprs() { return win_exprs_; }
    virtual int est_cost() override;
    virtual int est_width() override;
    virtual int re_est_cost(EstimateCostInfo &param, double &card, double &cost) override;
    virtual int get_op_exprs(ObIArray<ObRawExpr*> &all_exprs) override;
    virtual int compute_op_ordering() override;
    virtual int compute_sharding_info() override;
    virtual bool is_block_op() const override;
    virtual int allocate_granule_post(AllocGIContext &ctx) override;
    virtual int allocate_granule_pre(AllocGIContext &ctx) override;
    int get_win_partition_intersect_exprs(ObIArray<ObWinFunRawExpr *> &win_exprs,
                                          ObIArray<ObRawExpr *> &win_part_exprs);
    virtual int inner_replace_generated_agg_expr(
        const common::ObIArray<std::pair<ObRawExpr *, ObRawExpr*>> &to_replace_exprs) override;
    void set_single_part_parallel(bool v) { single_part_parallel_ = v; }
    bool is_single_part_parallel() const { return single_part_parallel_; }
    void set_ragne_dist_parallel(bool v) { range_dist_parallel_ = v; }
    bool is_range_dist_parallel() const { return range_dist_parallel_; }
    int get_winfunc_output_exprs(ObIArray<ObRawExpr *> &output_exprs);
    int set_rd_sort_keys(const common::ObIArray<OrderItem> &sort_keys)
    {
      return rd_sort_keys_.assign(sort_keys);
    }
    const common::ObIArray<OrderItem> &get_rd_sort_keys() const
    {
      return rd_sort_keys_;
    }

    void set_rd_pby_sort_cnt(const int64_t cnt) { rd_pby_sort_cnt_ = cnt; }
    int64_t get_rd_pby_sort_cnt() const { return rd_pby_sort_cnt_; }

    int set_dist_hint(const common::ObIArray<WinDistAlgo> &dist_hint)
    {
      return dist_hint_.assign(dist_hint);
    }
    virtual int print_outline(planText &plan_text) override;

  private:
    ObSEArray<ObWinFunRawExpr *, 4> win_exprs_;

    // Single partition (no partition by) window function parallel process, need the PX COORD
    // to collect the partial result and broadcast the final result to each worker.
    // Enable condition:
    // 1. Only one partition (no partition by)
    // 2. Window is the whole partition
    // 3. Only the following functions supported: sum,count,max,min
    bool single_part_parallel_;

    // Range distribution window function parallel process: data is range distributed,
    // the first and last partition of each worker may be partial result. Then the PX COORD
    // calculate the `patch` of the first row of first partition and last row of last
    // partition, finally the other rows of those partition will be updated (apply the patch).
    // Enable condition:
    // 1. NDV of partition by is too small for parallelism
    // 2. Is cumulative window, window is from first row to current row
    // 3. Only the following functions supported: rank,dense_rank,sum,count,min,max
    bool range_dist_parallel_;

    // sort keys for range distributed parallel.
    common::ObSEArray<OrderItem, 8, common::ModulePageAllocator, true> rd_sort_keys_;
    // the first %rd_pby_sort_cnt_ of %rd_sort_keys_ is the partition by of window function.
    int64_t rd_pby_sort_cnt_;

    // for PQ_DISTRIBUTE_WINDOW hint outline
    common::ObSEArray<WinDistAlgo, 8, common::ModulePageAllocator, true> dist_hint_;
  };
}
}
#endif // OCEANBASE_SQL_OB_LOG_WINDOW_FUNCTION_H
