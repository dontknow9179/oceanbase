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

#ifndef OCEANBASE_ROOTSERVER_FREEZE_OB_FREEZE_INFO_DETECTOR_
#define OCEANBASE_ROOTSERVER_FREEZE_OB_FREEZE_INFO_DETECTOR_

#include "rootserver/freeze/ob_freeze_reentrant_thread.h"

namespace oceanbase
{
namespace common
{
class ObMySQLProxy;
}
namespace rootserver
{
class ObFreezeInfoManager;
class ObThreadIdling;

class ObFreezeInfoDetector : public ObFreezeReentrantThread
{
public:
  ObFreezeInfoDetector();
  virtual ~ObFreezeInfoDetector() {}
  int init(const uint64_t tenant_id,
           common::ObMySQLProxy &sql_proxy,
           ObFreezeInfoManager &freeze_info_manager,
           ObThreadIdling &major_scheduler_idling);

  virtual void run3() override;
  virtual int blocking_run() override { BLOCKING_RUN_IMPLEMENT(); }
  virtual int64_t get_schedule_interval() const override;

  virtual int start() override;

  int signal();

private:
  int check_need_broadcast(bool &need_broadcast);
  int try_broadcast_freeze_info(const int64_t expected_epoch);
  int try_renew_snapshot_gc_ts();
  int try_minor_freeze();
  int try_update_zone_info(const int64_t expected_epoch);

  int can_start_work(bool &can_work);

private:
  static const int64_t FREEZE_INFO_DETECTOR_THREAD_CNT = 1;
  static const int64_t UPDATER_INTERVAL_US = 10 * 1000 * 1000; // 10s

  bool is_inited_;
  bool is_gc_ts_inited_;
  int64_t last_gc_timestamp_;
  ObFreezeInfoManager *freeze_info_mgr_;
  ObThreadIdling *major_scheduler_idling_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObFreezeInfoDetector);
};

}
}
#endif // OCEANBASE_ROOTSERVER_FREEZE_OB_FREEZE_INFO_DETECTOR_
