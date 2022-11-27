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
#include <string.h>
#include "sql/engine/expr/ob_expr_to_clob.h"
#include "sql/session/ob_sql_session_info.h"
#include "objit/common/ob_item_type.h"
#include "lib/oblog/ob_log.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

ObExprToClob::ObExprToClob(ObIAllocator &alloc)
    : ObExprToCharCommon(alloc, T_FUN_SYS_TO_CLOB, N_TO_CLOB, 1)
{
}

ObExprToClob::~ObExprToClob()
{
}

int ObExprToClob::calc_result_type1(ObExprResType &type,
                                    ObExprResType &text,
                                    common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;
  ObSessionNLSParams nls_param = type_ctx.get_session()->get_session_nls_params();

  if (OB_ISNULL(type_ctx.get_session())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session is NULL", K(ret));
  } else if (ob_is_null(text.get_type())) {
    type.set_null();
  } else if (ob_is_string_tc(text.get_type())
             || ob_is_clob(text.get_type(), text.get_collation_type())
             || ob_is_raw(text.get_type())
             || ob_is_numeric_type(text.get_type())
             || ob_is_oracle_datetime_tc(text.get_type())
             || ob_is_rowid_tc(text.get_type())
             || ob_is_interval_tc(text.get_type())) {
    type.set_clob();
    type.set_collation_level(CS_LEVEL_IMPLICIT);
    type.set_collation_type(nls_param.nls_collation_);
    if (!text.is_clob()) {
      text.set_calc_type(ObVarcharType);
    }
    text.set_calc_collation_type(nls_param.nls_collation_);
  } else {
    ret = OB_ERR_INVALID_TYPE_FOR_OP;
    LOG_WARN("wrong type of argument in function to_clob", K(ret), K(text));
  }

  return ret;
}

int ObExprToClob::calc_to_clob_expr(const ObExpr &expr, ObEvalCtx &ctx,
                                    ObDatum &res)
{
  int ret = OB_SUCCESS;
  ObDatum *arg = NULL;
  if (OB_UNLIKELY(1 != expr.arg_cnt_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid arg cnt or arg res type", K(ret), K(expr.arg_cnt_));
  } else if (OB_FAIL(expr.eval_param_value(ctx, arg))) {
    LOG_WARN("eval param failed", K(ret));
  } else if (arg->is_null()) {
    res.set_null();
  } else {
    res.set_datum(*arg);
    if (!res.is_null() && res.len_ > OB_MAX_LONGTEXT_LENGTH) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("wrong length of result in function to_clob", K(ret), K(res.len_));
    }
  }
  return ret;
}

int ObExprToClob::cg_expr(ObExprCGCtx &expr_cg_ctx, const ObRawExpr &raw_expr,
                       ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  rt_expr.eval_func_ = calc_to_clob_expr;
  return ret;
}
} // end of sql
} // end of oceanbase
