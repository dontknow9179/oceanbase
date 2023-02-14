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

#include "log_checksum.h"
#include "lib/checksum/ob_crc64.h"
#include "log_define.h"

namespace oceanbase
{
using namespace common;
namespace palf
{
LogChecksum::LogChecksum()
  : is_inited_(false),
    prev_accum_checksum_(0),
    accum_checksum_(0),
    verify_checksum_(0)
{}

int LogChecksum::init(const int64_t palf_id, const int64_t accum_checksum)
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else {
    palf_id_ = palf_id;
    accum_checksum_ = accum_checksum;
    verify_checksum_ = accum_checksum;
    is_inited_ = true;
    PALF_LOG(INFO, "LogChecksum init success", K_(palf_id), K_(verify_checksum), K_(accum_checksum));
  }
  return ret;
}

void LogChecksum::destroy()
{
  is_inited_ = false;
  palf_id_ = INVALID_PALF_ID;
  accum_checksum_ = 0;
  verify_checksum_ = 0;
}

int LogChecksum::acquire_accum_checksum(const int64_t data_checksum,
                                        int64_t &accum_checksum)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else {
    prev_accum_checksum_ = accum_checksum_;
    accum_checksum_ = common::ob_crc64(accum_checksum_, const_cast<int64_t *>(&data_checksum),
                                       sizeof(data_checksum));
    accum_checksum = accum_checksum_;
    PALF_LOG(TRACE, "acquire_accum_checksum success", K_(palf_id), K(data_checksum), K_(verify_checksum),
        K_(prev_accum_checksum), K_(accum_checksum));
  }
  return ret;
}

int LogChecksum::verify_accum_checksum(const int64_t data_checksum,
                                       const int64_t accum_checksum)
{
  int ret = common::OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else {
    const int64_t old_verify_checksum = verify_checksum_;
    verify_checksum_ = common::ob_crc64(verify_checksum_, const_cast<int64_t *>(&data_checksum),
                                        sizeof(data_checksum));
    if (verify_checksum_ != accum_checksum) {
      ret = common::OB_CHECKSUM_ERROR;
      LOG_DBA_ERROR(OB_CHECKSUM_ERROR, "msg", "log checksum error", "ret", ret, K_(palf_id), K(data_checksum),
          K(accum_checksum), K(old_verify_checksum), K_(verify_checksum));
    } else {
      PALF_LOG(TRACE, "verify_accum_checksum success", K(ret), K_(palf_id), K(data_checksum), K(accum_checksum),
               K_(verify_checksum), K_(accum_checksum));
    }
  }
  return ret;
}

int LogChecksum::rollback_accum_checksum(const int64_t curr_accum_checksum)
{
  int ret = common::OB_SUCCESS;
  if (curr_accum_checksum != accum_checksum_) {
    ret = OB_STATE_NOT_MATCH;
    PALF_LOG(WARN, "accum_checksum does not match", K_(palf_id), K(curr_accum_checksum), K_(prev_accum_checksum),
        K_(accum_checksum));
  } else {
    accum_checksum_ = prev_accum_checksum_;
    prev_accum_checksum_ = 0;
    PALF_LOG(INFO, "rollback_accum_checksum", K_(palf_id), K(curr_accum_checksum), K_(prev_accum_checksum),
        K_(accum_checksum));
  }
  return ret;
}

void LogChecksum::set_accum_checksum(const int64_t accum_checksum)
{
  accum_checksum_ = accum_checksum;
  PALF_LOG(INFO, "set_accum_checksum", K_(palf_id), K(accum_checksum));
}

void LogChecksum::set_verify_checksum(const int64_t verify_checksum)
{
  verify_checksum_ = verify_checksum;
  PALF_LOG(INFO, "set_verify_checksum", K_(palf_id), K(verify_checksum));
}

} // namespace palf
} // namespace oceanbase
