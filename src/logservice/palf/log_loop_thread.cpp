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

#include "log_loop_thread.h"
#include "palf_env_impl.h"
#include "lib/thread/ob_thread_name.h"

namespace oceanbase
{
using namespace common;
using namespace share;
namespace palf
{
LogLoopThread::LogLoopThread()
    : palf_env_impl_(NULL),
      is_inited_(false)
{
}

LogLoopThread::~LogLoopThread()
{
  destroy();
}

int LogLoopThread::init(PalfEnvImpl *palf_env_impl)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    PALF_LOG(WARN, "LogLoopThread has been inited", K(ret));
  } else if (NULL == palf_env_impl) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid argument", K(ret), KP(palf_env_impl));
  } else {
    palf_env_impl_ = palf_env_impl;
    share::ObThreadPool::set_run_wrapper(MTL_CTX());
    is_inited_ = true;
  }

  if ((OB_FAIL(ret)) && (OB_INIT_TWICE != ret)) {
    destroy();
  }
  PALF_LOG(INFO, "LogLoopThread init finished", K(ret));
  return ret;
}

void LogLoopThread::destroy()
{
  stop();
  wait();
  is_inited_ = false;
  palf_env_impl_ = NULL;
}

void LogLoopThread::run1()
{
  lib::set_thread_name("LogLoop");
  log_loop_();
  PALF_LOG(INFO, "log_loop_thread will stop");
}

void LogLoopThread::log_loop_()
{
  int64_t last_switch_state_time = OB_INVALID_TIMESTAMP;
  int64_t last_check_freeze_mode_time = OB_INVALID_TIMESTAMP;
  int64_t last_sw_freeze_time = OB_INVALID_TIMESTAMP;
  while (!has_set_stop()) {
    int tmp_ret = OB_SUCCESS;
    const int64_t start_ts = ObTimeUtility::current_time();
    // try check and switch state for all replicas
    if (start_ts - last_switch_state_time >= 10 * 1000) {
      if (OB_SUCCESS != (tmp_ret = palf_env_impl_->try_switch_state_for_all())) {
        PALF_LOG(WARN, "try_switch_state_for_all failed", K(tmp_ret));
      }
      last_switch_state_time = start_ts;
    }
    // try switch freeze mode
    const int64_t now = ObTimeUtility::current_time();
    if (now - last_check_freeze_mode_time >= 1 * 1000 * 1000) {
      if (OB_SUCCESS != (tmp_ret = palf_env_impl_->check_and_switch_freeze_mode())) {
        PALF_LOG(WARN, "check_and_switch_freeze_mode failed", K(tmp_ret));
      }
      last_check_freeze_mode_time = now;
    }
    // try freeze log
    if (OB_SUCCESS != (tmp_ret = palf_env_impl_->try_freeze_log_for_all())) {
      PALF_LOG(WARN, "try_freeze_log_for_all failed", K(tmp_ret));
    }
    const int64_t round_cost_time = ObTimeUtility::current_time() - start_ts;
    int32_t sleep_ts = PALF_LOG_LOOP_INTERVAL_US - static_cast<const int32_t>(round_cost_time);
    if (sleep_ts < 0) {
      sleep_ts = 0;
    }
    ob_usleep(sleep_ts);

    if (REACH_TIME_INTERVAL(5 * 1000 * 1000)) {
      PALF_LOG(INFO, "LogLoopThread round_cost_time", K(round_cost_time));
    }
  }
}

} // namespace palf
} // namespace oceanbase
