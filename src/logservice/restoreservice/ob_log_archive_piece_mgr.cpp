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

#include "ob_log_archive_piece_mgr.h"
#include "lib/container/ob_se_array.h"            // ObSEArray
#include "lib/ob_define.h"
#include "lib/ob_errno.h"
#include "lib/string/ob_string.h"                 // ObString
#include "lib/utility/ob_macro_utils.h"
#include "logservice/archiveservice/ob_archive_define.h"
#include "logservice/archiveservice/ob_archive_file_utils.h"      // ObArchiveFileUtils
#include "logservice/archiveservice/ob_archive_util.h"
#include "logservice/palf/log_define.h"
#include "logservice/palf/lsn.h"
#include "rootserver/restore/ob_restore_util.h"  // ObRestoreUtil
#include "share/backup/ob_backup_path.h"         // ObBackupPath
#include "share/backup/ob_backup_store.h"        // ObBackupStore
#include "share/backup/ob_archive_store.h"
#include "share/backup/ob_archive_piece.h"        // ObArchivePiece
#include "share/backup/ob_backup_struct.h"
#include "share/backup/ob_archive_path.h"   // ObArchivePathUtil
#include "share/rc/ob_tenant_base.h"
#include "share/ob_errno.h"

namespace oceanbase
{
namespace logservice
{
using namespace share;
void ObLogArchivePieceContext::RoundContext::reset()
{
  state_ = RoundContext::State::INVALID;
  round_id_ = 0;
  start_ts_ = OB_INVALID_TIMESTAMP;
  end_ts_ = OB_INVALID_TIMESTAMP;
  min_piece_id_ = 0;
  max_piece_id_ = 0;
  base_piece_id_ = 0;
  piece_switch_interval_ = 0;
  base_piece_ts_ = OB_INVALID_TIMESTAMP;
}

bool ObLogArchivePieceContext::RoundContext::is_valid() const
{
  return ((RoundContext::State::ACTIVE == state_ && end_ts_ == INT64_MAX)
      || (RoundContext::State::STOP == state_ && end_ts_ > start_ts_))
    && round_id_ > 0
    && start_ts_ != OB_INVALID_TIMESTAMP
    && base_piece_id_ > 0
    && piece_switch_interval_ > 0
    && base_piece_ts_ != OB_INVALID_TIMESTAMP;
}

bool ObLogArchivePieceContext::RoundContext::is_in_stop_state() const
{
  return RoundContext::State::STOP == state_;
}

bool ObLogArchivePieceContext::RoundContext::is_in_empty_state() const
{
  return RoundContext::State::EMPTY == state_;
}

ObLogArchivePieceContext::RoundContext &ObLogArchivePieceContext::RoundContext::operator=(const RoundContext &other)
{
  state_ = other.state_;
  round_id_ = other.round_id_;
  start_ts_ = other.start_ts_;
  end_ts_ = other.end_ts_;
  min_piece_id_ = other.min_piece_id_;
  max_piece_id_ = other.max_piece_id_;
  base_piece_id_ = other.base_piece_id_;
  piece_switch_interval_ = other.piece_switch_interval_;
  base_piece_ts_ = other.base_piece_ts_;
  return *this;
}

bool ObLogArchivePieceContext::RoundContext::check_round_continuous_(const RoundContext &pre_round) const
{
  bool bret = false;
  if (!pre_round.is_valid() || !is_valid()) {
    bret = false;
  } else if (pre_round.end_ts_ >= start_ts_) {
    bret = true;
  }
  return bret;
}

void ObLogArchivePieceContext::InnerPieceContext::reset()
{
  state_ = InnerPieceContext::State::INVALID;
  piece_id_ = 0;
  round_id_ = 0;
  min_lsn_in_piece_ = palf::LOG_INVALID_LSN_VAL;
  max_lsn_in_piece_ = palf::LOG_INVALID_LSN_VAL;
  min_file_id_ = 0;
  max_file_id_ = 0;
  file_id_ = 0;
  file_offset_ = 0;
  max_lsn_ = palf::LOG_INVALID_LSN_VAL;
}

bool ObLogArchivePieceContext::InnerPieceContext::is_valid() const
{
  return State::EMPTY == state_
    || State::LOW_BOUND == state_
    || ((State::ACTIVE == state_ || State::FROZEN == state_ || State::GC == state_)
        && min_lsn_in_piece_.is_valid()
        && min_file_id_ > 0
        && max_file_id_ >= min_file_id_
        && min_lsn_in_piece_.is_valid());
}

int ObLogArchivePieceContext::InnerPieceContext::update_file(
    const int64_t file_id,
    const int64_t file_offset,
    const palf::LSN &lsn)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(file_id > max_file_id_ || file_id < min_file_id_)) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", K(ret), K(file_id), K(file_offset), K(lsn));
  } else {
    file_id_ = file_id;
    file_offset_ = file_offset;
    max_lsn_ = lsn;
  }
  return ret;
}

ObLogArchivePieceContext::InnerPieceContext &ObLogArchivePieceContext::InnerPieceContext::operator=(
    const InnerPieceContext &other)
{
  state_ = other.state_;
  piece_id_ = other.piece_id_;
  round_id_ = other.round_id_;
  min_lsn_in_piece_ = other.min_lsn_in_piece_;
  max_lsn_in_piece_ = other.max_lsn_in_piece_;
  min_file_id_ = other.min_file_id_;
  max_file_id_ = other.max_file_id_;
  file_id_ = other.file_id_;
  file_offset_ = other.file_offset_;
  max_lsn_ = other.max_lsn_;
  return *this;
}

ObLogArchivePieceContext::ObLogArchivePieceContext() :
  is_inited_(false),
  locate_round_(false),
  id_(),
  dest_id_(0),
  min_round_id_(0),
  max_round_id_(0),
  round_context_(),
  inner_piece_context_(),
  archive_dest_()
{}

ObLogArchivePieceContext::~ObLogArchivePieceContext()
{
  reset();
}

void ObLogArchivePieceContext::reset()
{
  is_inited_ = false;
  id_.reset();
  archive_dest_.reset();
  reset_locate_info();
}

int ObLogArchivePieceContext::init(const share::ObLSID &id,
    const share::ObBackupDest &archive_dest)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(! id.is_valid()
        || ! archive_dest.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", K(ret), K(id), K(archive_dest));
  } else if (OB_FAIL(archive_dest_.deep_copy(archive_dest))) {
    CLOG_LOG(WARN, "root path deep copy failed", K(ret), K(id), K(archive_dest));
  } else {
    id_ = id;
    is_inited_ = true;
  }
  return ret;
}

int ObLogArchivePieceContext::get_piece(const int64_t pre_log_ts,
    const palf::LSN &start_lsn,
    int64_t &dest_id,
    int64_t &round_id,
    int64_t &piece_id,
    int64_t &file_id,
    int64_t &offset,
    palf::LSN &max_lsn)
{
  int ret = OB_SUCCESS;
  file_id = cal_archive_file_id_(start_lsn);
  if (OB_UNLIKELY(OB_INVALID_TIMESTAMP == pre_log_ts
        || ! start_lsn.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", K(ret), K(pre_log_ts), K(start_lsn));
  } else if (OB_FAIL(get_piece_(pre_log_ts, start_lsn, file_id, dest_id,
          round_id, piece_id, offset, max_lsn))) {
    CLOG_LOG(WARN, "get piece failed", K(ret));
  }
  return ret;
}

int ObLogArchivePieceContext::deep_copy_to(ObLogArchivePieceContext &other)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(other.archive_dest_.deep_copy(archive_dest_))) {
    CLOG_LOG(WARN, "deep copy failed", K(ret));
  } else {
    other.is_inited_ = is_inited_;
    other.locate_round_ = locate_round_;
    other.id_ = id_;
    other.dest_id_ = dest_id_;
    other.min_round_id_ = min_round_id_;
    other.max_round_id_ = max_round_id_;
    other.round_context_ = round_context_;
    other.inner_piece_context_ = inner_piece_context_;
  }
  return ret;
}

void ObLogArchivePieceContext::reset_locate_info()
{
  locate_round_ = false;
  dest_id_ = 0;
  min_round_id_ = 0;
  max_round_id_ = 0;
  round_context_.reset();
  inner_piece_context_.reset();
}

int ObLogArchivePieceContext::update_file_info(const int64_t dest_id,
    const int64_t round_id,
    const int64_t piece_id,
    const int64_t file_id,
    const int64_t file_offset,
    const palf::LSN &max_lsn)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(file_id <= 0 || file_offset < 0 || ! max_lsn.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", K(ret), K(file_id), K(file_offset), K(max_lsn));
  } else if (dest_id != dest_id_) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "different dest, unexpected", K(ret), K(dest_id), K(round_id),
        K(piece_id), K(file_id), K(file_offset), K(max_lsn), KPC(this));
  } else if (OB_FAIL(inner_piece_context_.update_file(file_id, file_offset, max_lsn))) {
    CLOG_LOG(WARN, "inner_piece_context_ update file failed", K(ret), K(dest_id), K(round_id),
        K(piece_id), K(file_id), K(file_offset), K(max_lsn), KPC(this));
  }
  return ret;
}

int ObLogArchivePieceContext::get_round_(const int64_t start_ts)
{
  int ret = OB_SUCCESS;
  int64_t round_id = 0;
  share::ObArchiveStore archive_store;
  if (OB_UNLIKELY(dest_id_ <= 0)) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(ERROR, "invalid dest id", K(ret), K(dest_id_));
  } else if (OB_UNLIKELY(locate_round_)) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "already locate round", K(ret), KPC(this));
  } else if (OB_FAIL(archive_store.init(archive_dest_))) {
    CLOG_LOG(WARN, "backup store init failed", K(ret), K_(archive_dest));
  } else if (OB_FAIL(archive_store.get_round_id(dest_id_, start_ts, round_id))) {
    CLOG_LOG(WARN, "archive store get round failed", K(ret), K(dest_id_), K(start_ts));
  } else if (OB_UNLIKELY(round_id <= 0)) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(ERROR, "invalid round id", K(ret), K(round_id), K(start_ts), K(archive_store));
  } else {
    round_context_.reset();
    round_context_.round_id_ = round_id;
    locate_round_ = true;
    CLOG_LOG(INFO, "get round succ", K(ret), K(start_ts), KPC(this));
  }
  return ret;
}

int ObLogArchivePieceContext::get_round_range_()
{
  int ret = OB_SUCCESS;
  int64_t min_round_id = 0;
  int64_t max_round_id = 0;
  share::ObArchiveStore archive_store;
  if (OB_FAIL(load_archive_meta_())) {
    CLOG_LOG(WARN, "load archive meta failed", K(ret));
  } else if (OB_FAIL(archive_store.init(archive_dest_))) {
    CLOG_LOG(WARN, "backup store init failed", K(ret), K_(archive_dest));
  } else if (OB_FAIL(archive_store.get_round_range(dest_id_, min_round_id, max_round_id))) {
    CLOG_LOG(WARN, "archive store get round failed", K(ret), K(dest_id_));
  } else if (OB_UNLIKELY(min_round_id <= 0 || max_round_id < min_round_id)) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "invalid round id", K(ret), K(min_round_id),
        K(max_round_id), K(archive_store), KPC(this));
  } else if (max_round_id_ == max_round_id && min_round_id_ == min_round_id) {
    // skip
  } else {
    min_round_id_ = min_round_id;
    max_round_id_ = max_round_id;
    CLOG_LOG(INFO, "get round range succ", K(ret), K(min_round_id), K(max_round_id), K(id_));
  }
  return ret;
}

int ObLogArchivePieceContext::load_archive_meta_()
{
  int ret = OB_SUCCESS;
  share::ObBackupStore backup_store;
  share::ObBackupFormatDesc desc;
  if (OB_FAIL(backup_store.init(archive_dest_))) {
    CLOG_LOG(WARN, "backup store init failed", K(ret), K_(archive_dest));
  } else if (OB_FAIL(backup_store.read_format_file(desc))) {
    CLOG_LOG(WARN, "read single file failed", K(ret), K(archive_dest_), K(backup_store));
  } else if (OB_UNLIKELY(! desc.is_valid())) {
    ret = OB_INVALID_DATA;
    CLOG_LOG(WARN, "backup format desc is invalid", K(ret), K(desc), K(backup_store));
  } else {
    dest_id_ = desc.dest_id_;
    CLOG_LOG(INFO, "load archive meta succ", K(desc));
  }
  return ret;
}

// 此处假设stop状态的round, 一定都包含round end file
// 反之仅round end file持久化成功的轮次, 才可以确认是stop状态, 与归档集群状态无关
int ObLogArchivePieceContext::load_round_(const int64_t round_id, RoundContext &round_context, bool &exist)
{
  int ret = OB_SUCCESS;
  share::ObArchiveStore archive_store;
  bool start_exist = false;
  bool end_exist = false;
  share::ObRoundStartDesc start_desc;
  share::ObRoundEndDesc end_desc;
  exist = true;
  if (OB_FAIL(archive_store.init(archive_dest_))) {
    CLOG_LOG(WARN, "backup store init failed", K(ret), K_(archive_dest));
  } else if (OB_FAIL(archive_store.is_round_start_file_exist(dest_id_, round_id, start_exist))) {
    CLOG_LOG(WARN, "check round start file exist failed", K(ret), K(round_id), KPC(this));
  } else if (! start_exist) {
    exist = false;
    CLOG_LOG(INFO, "round not exist, skip it", K(round_id), KPC(this));
  } else if (OB_FAIL(archive_store.read_round_start(dest_id_, round_id, start_desc))) {
    CLOG_LOG(WARN, "read round start file failed", K(ret), K(round_id), KPC(this));
  } else if (OB_FAIL(archive_store.is_round_end_file_exist(dest_id_, round_id, end_exist))) {
    CLOG_LOG(WARN, "check round start file exist failed", K(ret), K(round_id), KPC(this));
  } else if (! end_exist) {
    CLOG_LOG(INFO, "round end file not exist, round not stop", K(round_id), KPC(this));
  } else if (OB_FAIL(archive_store.read_round_end(dest_id_, round_id, end_desc))) {
    CLOG_LOG(WARN, "check round start file exist failed", K(ret), K(round_id), KPC(this));
  }

  if (OB_FAIL(ret) || ! exist) {
  } else {
    round_context.round_id_ = start_desc.round_id_;
    round_context.start_ts_ = start_desc.start_scn_;
    round_context.base_piece_id_ = start_desc.base_piece_id_;
    round_context.base_piece_ts_ = start_desc.start_scn_;
    round_context.piece_switch_interval_ = start_desc.piece_switch_interval_;
    if (end_exist) {
      round_context.state_ = RoundContext::State::STOP;
      round_context.end_ts_ = end_desc.checkpoint_scn_;
      round_context.max_piece_id_ = cal_piece_id_(round_context.end_ts_);
    } else {
      round_context.state_ = RoundContext::State::ACTIVE;
      round_context.end_ts_ = INT64_MAX;
    }
  }
  return ret;
}

int ObLogArchivePieceContext::get_piece_(const int64_t log_ts,
    const palf::LSN &lsn,
    const int64_t file_id,
    int64_t &dest_id,
    int64_t &round_id,
    int64_t &piece_id,
    int64_t &offset,
    palf::LSN &max_lsn)
{
  int ret = OB_SUCCESS;
  bool done = false;
  while (OB_SUCC(ret) && ! done) {
    if (OB_FAIL(switch_round_if_need_(log_ts, lsn))) {
      CLOG_LOG(WARN, "switch round if need failed", K(ret), KPC(this));
    } else if (OB_FAIL(switch_piece_if_need_(file_id, log_ts, lsn))) {
      CLOG_LOG(WARN, "switch piece if need", K(ret), KPC(this));
    } else {
      ret = get_(lsn, file_id, dest_id, round_id, piece_id, offset, max_lsn, done);
    }

    // 由于场景复杂, 为避免遗漏场景导致无法跳出循环, 每次重试sleep 100ms
    if (! done) {
      ob_usleep(100 * 1000L);
    }

    if (REACH_TIME_INTERVAL(10 * 1000 * 1000L)) {
      CLOG_LOG(WARN, "get piece cost too much time", K(log_ts), K(lsn), KPC(this));
    }
  }
  return ret;
}

int ObLogArchivePieceContext::switch_round_if_need_(const int64_t log_ts, const palf::LSN &lsn)
{
  int ret = OB_SUCCESS;
  RoundOp op = RoundOp::NONE;
  RoundContext pre_round = round_context_;
  check_if_switch_round_(lsn, op);
  switch (op) {
    case RoundOp::NONE:
      break;
    case RoundOp::LOAD:
      ret = load_round_info_();
      break;
    case RoundOp::LOAD_RANGE:
      ret = get_round_range_();
      break;
    case RoundOp::LOCATE:
      ret = get_round_(log_ts);
      break;
    case RoundOp::FORWARD:
      ret = forward_round_(pre_round);
      break;
    case RoundOp::BACKWARD:
      ret = backward_round_();
      break;
    default:
      ret = OB_ERR_UNEXPECTED;
      CLOG_LOG(ERROR, "invalid round op", K(ret), K(op));
      break;
  }
  return ret;
}

void ObLogArchivePieceContext::check_if_switch_round_(const palf::LSN &lsn, RoundOp &op)
{
  op = RoundOp::NONE;
  if (min_round_id_ == 0 || max_round_id_ == 0 || is_max_round_done_(lsn) /* 当前读取到最大round的最大值, 并且round已经STOP*/) {
    op = RoundOp::LOAD_RANGE;
  } else if (! locate_round_) {
    op = RoundOp::LOCATE;
  } else if (need_backward_round_(lsn)/*确定当前round日志全部大于需要消费日志, 并且当前round大于最小round id*/) {
    op = RoundOp::BACKWARD;
  } else if (need_forward_round_(lsn)/*确定当前round日志全部小于需要消费日志, 并且当前round小于最大round id*/) {
    op = RoundOp::FORWARD;
  } else if (need_load_round_info_(lsn)/*当前round能访问到的最大piece已经STOP, 并且当前round还是ACTIVE的*/) {
    op = RoundOp::LOAD;
  }

  if (RoundOp::NONE != op) {
    CLOG_LOG(INFO, "check_if_switch_round_ op", K(op), KPC(this));
  }
}

// 检查是否所有round日志都被消费完
// 当前读取到最大round的最大值, 并且round已经STOP
bool ObLogArchivePieceContext::is_max_round_done_(const palf::LSN &lsn) const
{
  bool bret = false;
  if (max_round_id_ <= 0) {
    bret = false;
  } else if (! round_context_.is_valid()
      || round_context_.round_id_ < max_round_id_
      || ! round_context_.is_in_stop_state()) {
    bret = false;
  } else if (! inner_piece_context_.is_valid()
      || inner_piece_context_.round_id_ < max_round_id_
      || inner_piece_context_.piece_id_ < round_context_.max_piece_id_
      || ! inner_piece_context_.is_fronze_()
      || lsn < inner_piece_context_.max_lsn_in_piece_) {
    bret = false;
  } else {
    bret = true;
    CLOG_LOG(INFO, "max round consume done, need switch load round range", KPC(this));
  }
  return bret;
}

// 可以前向切round, 一定是piece已经切到最小, 并且最小piece是FROZEN状态
bool ObLogArchivePieceContext::need_backward_round_(const palf::LSN &lsn) const
{
  bool bret = false;
  if (min_round_id_ > 0
      && round_context_.is_valid()
      && round_context_.round_id_ > min_round_id_
      && (inner_piece_context_.is_fronze_() || inner_piece_context_.is_empty_())
      && round_context_.min_piece_id_ == inner_piece_context_.piece_id_
      && inner_piece_context_.min_lsn_in_piece_ > lsn) {
    bret = true;
  }
  return bret;
}

// Note: 对于空round, 默认一定是与下一个round是不连续的; 因此遇到空round, 默认只能前向查询, 不支持后向查找
//  bad case round 1、2、3, 当前为3，需要切到round 1, 中间round 2为空, 只依赖round 2信息无法判断需要切到round 1
//  遇到前向切round, 由调用者处理
bool ObLogArchivePieceContext::need_forward_round_(const palf::LSN &lsn) const
{
  bool bret = false;
  if (max_round_id_ <= 0) {
    bret = false;
  } else if (round_context_.is_in_empty_state()) {
    bret = true;
    CLOG_LOG(INFO, "empty round, only forward round supported", K(round_context_));
  } else if (round_context_.is_valid()
      && round_context_.is_in_stop_state()
      && round_context_.round_id_ < max_round_id_
      && inner_piece_context_.piece_id_ == round_context_.max_piece_id_
      && inner_piece_context_.is_valid()) {
    // 当前piece是round下最新piece, 并且round已经STOP
    if (inner_piece_context_.is_low_bound_()) {
      // 当前piece状态为LOW_BOUND, 在当前piece内日志流仍未产生
      bret = true;
    }
    else if ((inner_piece_context_.is_fronze_() || inner_piece_context_.is_empty_())
        && inner_piece_context_.max_lsn_in_piece_ <= lsn) {
      // 同时当前piece状态为FROZEN/EMPTY
      // 并且需要日志LSN大于当前piece下最大值
      bret = true;
    }
  }
  return bret;
}

// 前提: 当前round_context状态非STOP
// 1. 当前round_context信息是无效的
// 2. 当前round最大piece不包含需要消费的日志
bool ObLogArchivePieceContext::need_load_round_info_(const palf::LSN &lsn) const
{
  bool bret = false;
  if (round_context_.is_in_stop_state()) {
    bret = false;
  } else if (round_context_.round_id_ > max_round_id_ || round_context_.round_id_ < min_round_id_ || round_context_.round_id_ <= 0) {
    bret = false;
  } else if (! round_context_.is_valid()) {
    bret = true;
  } else if (round_context_.max_piece_id_ == inner_piece_context_.piece_id_
      && inner_piece_context_.is_fronze_()
      && lsn >= inner_piece_context_.max_lsn_in_piece_) {
    bret = true;
  }
  return bret;
}

int ObLogArchivePieceContext::load_round_info_()
{
  int ret = OB_SUCCESS;
  bool round_exist = false;
  int64_t min_piece_id = 0;
  int64_t max_piece_id = 0;
  if (OB_UNLIKELY(round_context_.round_id_ > max_round_id_
        || round_context_.round_id_ < min_round_id_
        || round_context_.round_id_ <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "round id not valid", K(ret), K(round_context_));
  } else if (OB_FAIL(load_round_(round_context_.round_id_, round_context_, round_exist))) {
    CLOG_LOG(WARN, "load round failed", K(ret), K(round_context_));
  } else if (! round_exist) {
    ret = OB_ARCHIVE_ROUND_NOT_CONTINUOUS;
    CLOG_LOG(WARN, "round not exist, unexpected", K(ret), K(round_exist), K(round_context_));
  } else if (OB_FAIL(get_round_piece_range_(round_context_.round_id_, min_piece_id, max_piece_id))
      && OB_ENTRY_NOT_EXIST != ret) {
    CLOG_LOG(WARN, "get piece range failed", K(ret), K(round_context_));
  } else if (OB_ENTRY_NOT_EXIST == ret) {
    if (round_context_.is_in_stop_state()) {
      round_context_.state_ = RoundContext::State::EMPTY;
    }
  } else {
    round_context_.min_piece_id_ = min_piece_id;
    round_context_.max_piece_id_ = max_piece_id;
    CLOG_LOG(INFO, "load round info succ", K(round_context_));
  }
  return ret;
}

int ObLogArchivePieceContext::get_round_piece_range_(const int64_t round_id, int64_t &min_piece_id, int64_t &max_piece_id)
{
  int ret = OB_SUCCESS;
  share::ObArchiveStore archive_store;
  share::ObPieceInfoDesc desc;
  if (OB_FAIL(archive_store.init(archive_dest_))) {
    CLOG_LOG(WARN, "backup store init failed", K(ret), K_(archive_dest));
  } else if (OB_FAIL(archive_store.get_piece_range(dest_id_, round_id, min_piece_id, max_piece_id))) {
    CLOG_LOG(WARN, "get piece range failed", K(ret), K(dest_id_), K(round_id));
  }
  return ret;
}

// 由于round id可能不连续, 该函数找到一个可以消费的轮次
int ObLogArchivePieceContext::forward_round_(const RoundContext &pre_round)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(min_round_id_ <= 0
        || max_round_id_ < min_round_id_
        || round_context_.round_id_ >= max_round_id_)) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", K(ret), KPC(this));
  } else {
    bool done = false;
    int64_t round_id = round_context_.round_id_ + 1;
    while (OB_SUCC(ret) && ! done && round_id <= max_round_id_) {
      bool exist = false;
      if (OB_FAIL(check_round_exist_(round_id, exist))) {
        CLOG_LOG(WARN, "check round exist failed", K(ret));
      } else if (exist) {
        done = true;
      } else {
        round_id++;
      }
    }
    // 由于max_round_id一定存在, 预期成功一定可以done
    if (OB_SUCC(ret) && done) {
      round_context_.reset();
      round_context_.round_id_ = round_id;
      CLOG_LOG(INFO, "forward round succ", K(round_id), KPC(this));
    }

    if (OB_SUCC(ret) && done) {
      if (OB_FAIL(load_round_info_())) {
        CLOG_LOG(WARN, "load round info failed", K(ret));
      } else if (! round_context_.check_round_continuous_(pre_round)) {
        ret = OB_ARCHIVE_ROUND_NOT_CONTINUOUS;
        CLOG_LOG(WARN, "forward round not continue", K(ret), K(pre_round), K(round_context_));
      }
    }
  }
  return ret;
}

// 前向查找更加复杂, 不支持直接前向查询, 由调用者处理
int ObLogArchivePieceContext::backward_round_()
{
  return OB_ERR_OUT_OF_LOWER_BOUND;
}

int ObLogArchivePieceContext::check_round_exist_(const int64_t round_id, bool &exist)
{
  int ret = OB_SUCCESS;
  share::ObArchiveStore archive_store;
  if (OB_FAIL(archive_store.init(archive_dest_))) {
    CLOG_LOG(WARN, "backup store init failed", K(ret), K_(archive_dest));
  } else if (OB_FAIL(archive_store.is_round_start_file_exist(dest_id_, round_id, exist))) {
    CLOG_LOG(WARN, "check round start file exist failed", K(ret), K(dest_id_), K(round_id));
  }
  return ret;
}

int ObLogArchivePieceContext::switch_piece_if_need_(const int64_t file_id, const int64_t log_ts, const palf::LSN &lsn)
{
  int ret = OB_SUCCESS;
  PieceOp op = PieceOp::NONE;
  check_if_switch_piece_(file_id, lsn, op);
  switch (op) {
    case PieceOp::NONE:
      break;
    case PieceOp::LOAD:
      ret = get_cur_piece_info_(log_ts, lsn);
      break;
    case PieceOp::BACKWARD:
      ret = backward_piece_();
      break;
    case PieceOp::FORWARD:
      ret = forward_piece_();
      break;
    default:
      ret = OB_NOT_SUPPORTED;
      CLOG_LOG(ERROR, "piece op not supported", K(ret), K(op), KPC(this));
  }
  return ret;
}

void ObLogArchivePieceContext::check_if_switch_piece_(const int64_t file_id,
    const palf::LSN &lsn,
    PieceOp &op)
{
  op = PieceOp::NONE;
  if (! round_context_.is_valid()) {
    op = PieceOp::NONE;
  } else if (! inner_piece_context_.is_valid()) {
    op = PieceOp::LOAD;
  } else if (inner_piece_context_.round_id_ != round_context_.round_id_) {
    op = PieceOp::LOAD;
  }
  // 当前piece状态确定
  else if (inner_piece_context_.is_fronze_()) {
    // check forward or backward
    if (inner_piece_context_.max_lsn_in_piece_ > lsn) {
      if (inner_piece_context_.min_lsn_in_piece_ > lsn) {
        op = PieceOp::BACKWARD;
      } else {
         op = PieceOp::NONE;
      }
    } else {
      op = PieceOp::FORWARD;
    }
  }
  // 当前piece为EMPTY
  else if (inner_piece_context_.is_empty_()) {
    if (inner_piece_context_.max_lsn_in_piece_.is_valid() && inner_piece_context_.max_lsn_in_piece_ > lsn) {
      op = PieceOp::BACKWARD;
    } else {
      op = PieceOp::FORWARD;
    }
  }
  // 当前piece状态是LOW BOUND
  else if (inner_piece_context_.is_low_bound_()) {
    op = PieceOp::FORWARD;
  }
  // 当前piece内日志流GC
  else if (inner_piece_context_.is_gc_()) {
    if (inner_piece_context_.max_lsn_in_piece_ > lsn) {
      if (inner_piece_context_.min_lsn_in_piece_ > lsn) {
        op = PieceOp::BACKWARD;
      } else {
         op = PieceOp::NONE;
      }
    } else {
      // gc state, no bigger lsn exist in next pieces
      op = PieceOp::NONE;
    }
  }
  // 当前piece仍然为ACTIVE
  else {
    if (inner_piece_context_.max_file_id_ > file_id) {
      if (inner_piece_context_.min_lsn_in_piece_ >= lsn) {
        op = PieceOp::BACKWARD;
      } else {
        op = PieceOp::NONE;
      }
    }
    // 只有消费最新piece最新文件才需要读文件前LOAD, 代价可以接受
    else {
      op = PieceOp::LOAD;
    }
  }

  if (PieceOp::NONE != op) {
    CLOG_LOG(INFO, "check switch_piece_ op", K(inner_piece_context_), K(round_context_), K(op));
  }
}

// 由log_ts以及inner_piece_context与round_context共同决定piece_id
// 1. inner_piece_context.round_id == round_context.round_id 说明依然消费当前round, 如果当前piece_id有效, 并且该piece依然active, 则继续刷新该piece
// 2. inner_piece_context.round_id == round_context.round_id 并且piece状态为FROZEN或者EMPTY, 非预期错误
// 3. inner_piece_context.round_id != round_context.round_id, 需要由log_ts以及round piece范围, 决定piece id
int ObLogArchivePieceContext::get_cur_piece_info_(const int64_t log_ts, const palf::LSN &lsn)
{
  int ret = OB_SUCCESS;
  int64_t piece_id = 0;
  if (OB_FAIL(cal_load_piece_id_(log_ts, piece_id))) {
    CLOG_LOG(WARN, "cal load piece id failed", K(ret), K(log_ts));
  } else if (0 >= piece_id) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid piece id", K(ret), K(piece_id), K(log_ts));
  } else if (OB_FAIL(get_piece_meta_info_(piece_id))) {
    CLOG_LOG(WARN, "get piece meta info failed", K(ret), K(piece_id));
  } else if (OB_FAIL(get_piece_file_range_())) {
    CLOG_LOG(WARN, "get piece file range failed", K(ret), K(inner_piece_context_));
  } else if (OB_FAIL(get_min_lsn_in_piece_())) {
    CLOG_LOG(WARN, "get min lsn in piece failed", K(ret));
  } else {
    CLOG_LOG(INFO, "get cur piece info succ", KPC(this));
  }
  return ret;
}

int ObLogArchivePieceContext::cal_load_piece_id_(const int64_t log_ts, int64_t &piece_id)
{
  int ret = OB_SUCCESS;
  const int64_t base_piece_id = cal_piece_id_(log_ts);
  if (inner_piece_context_.round_id_ == round_context_.round_id_) {
    // 大概率被回收了, 报错处理, 后续可以优化
    if (inner_piece_context_.piece_id_ < round_context_.min_piece_id_) {
      ret = OB_ERR_UNEXPECTED;
      CLOG_LOG(WARN, "piece maybe recycle", K(ret), KPC(this));
    } else if (inner_piece_context_.piece_id_ > round_context_.max_piece_id_) {
      ret = OB_ERR_UNEXPECTED;
      CLOG_LOG(ERROR, "piece id out of range", K(ret), KPC(this));
    } else {
      piece_id = inner_piece_context_.piece_id_;
    }
  } else {
    piece_id = std::max(round_context_.min_piece_id_, base_piece_id);
  }
  CLOG_LOG(INFO, "cal load piece id", K(id_), K(round_context_), K(log_ts), K(piece_id));
  return ret;
}

int ObLogArchivePieceContext::get_piece_meta_info_(const int64_t piece_id)
{
  int ret = OB_SUCCESS;
  const int64_t round_id = round_context_.round_id_;
  share::ObArchiveStore archive_store;
  bool piece_meta_exist = true;
  bool is_ls_in_piece = false;
  palf::LSN min_lsn;
  palf::LSN max_lsn;
  if (piece_id > round_context_.max_piece_id_ || piece_id < round_context_.min_piece_id_) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "piece id out of round range", K(ret), K(piece_id), KPC(this));
  } else if (OB_FAIL(archive_store.init(archive_dest_))) {
    CLOG_LOG(WARN, "backup store init failed", K(ret), K_(archive_dest));
  } else if (OB_FAIL(archive_store.is_single_piece_file_exist(dest_id_, round_id, piece_id, piece_meta_exist))) {
    // 不同日志流归档进度不同, 可能存在多个piece未FROZEN情况, 消费piece需要确认是否FROZEN
    CLOG_LOG(WARN, "check single piece file exist failed", K(ret), K(piece_id), KPC(this));
  } else if (! piece_meta_exist) {
    // single piece file not exist, active piece
  } else if (OB_FAIL(get_ls_inner_piece_info_(id_, dest_id_, round_id, piece_id, min_lsn, max_lsn, is_ls_in_piece))) {
    CLOG_LOG(WARN, "get ls inner piece info failed", K(ret), K(round_id), K(piece_id), K(id_));
  }

  if (OB_SUCC(ret)) {
    inner_piece_context_.round_id_ = round_id;
    inner_piece_context_.piece_id_ = piece_id;
    // 如果piece meta 存在, 那么piece已经FROZEN, 日志流在该piece状态可能为FROZEN, EMPTY和LOW_BOUND
    if (piece_meta_exist) {
      // ls在该piece内存在, 那么状态一定是FROZEN或者EMPTY
      if (is_ls_in_piece) {
        inner_piece_context_.min_lsn_in_piece_ = min_lsn;
        inner_piece_context_.max_lsn_in_piece_ = max_lsn;
        if (inner_piece_context_.min_lsn_in_piece_ == inner_piece_context_.max_lsn_in_piece_) {
          inner_piece_context_.state_ = InnerPieceContext::State::EMPTY;
        } else {
          inner_piece_context_.state_ = InnerPieceContext::State::FROZEN;
          inner_piece_context_.min_file_id_ = cal_archive_file_id_(inner_piece_context_.min_lsn_in_piece_);
          inner_piece_context_.max_file_id_ = cal_archive_file_id_(inner_piece_context_.max_lsn_in_piece_);
        }
      }
      // ls在该piece内不存在, 并且该piece是FROZEN状态, 那么ls一定在更大piece内
      // NOTE: restore不理解归档文件内容, 很难区分是GC或LOW_BOUND
      // 依赖zeyong提交, 标示日志流已GC, 物理恢复场景固定, 可以假设是LOW_BOUND
      else {
        inner_piece_context_.state_ = InnerPieceContext::State::LOW_BOUND;
      }
    }
    // piece meta 不存在, piece一定是ACTIVE状态的
    else {
      inner_piece_context_.state_ = InnerPieceContext::State::ACTIVE;
    }
  }
  return ret;
}

int ObLogArchivePieceContext::get_ls_inner_piece_info_(const share::ObLSID &id, const int64_t dest_id,
    const int64_t round_id, const int64_t piece_id, palf::LSN &min_lsn, palf::LSN &max_lsn, bool &exist)
{
  int ret = OB_SUCCESS;
  share::ObArchiveStore archive_store;
  share::ObSingleLSInfoDesc desc;
  exist = false;
  if (OB_FAIL(archive_store.init(archive_dest_))) {
    CLOG_LOG(WARN, "backup store init failed", K(ret), K_(archive_dest));
  } else if (OB_FAIL(archive_store.read_single_ls_info(dest_id, round_id, piece_id, id, desc))
      && OB_BACKUP_FILE_NOT_EXIST != ret) {
    CLOG_LOG(WARN, "get single piece file failed", K(ret), K(dest_id), K(round_id), K(piece_id), K(id));
  } else if (OB_BACKUP_FILE_NOT_EXIST == ret) {
    exist = false;
    ret = OB_SUCCESS;
    CLOG_LOG(INFO, "ls not exist in cur piece", K(dest_id), K(round_id), K(piece_id), K(id));
  } else if (OB_UNLIKELY(!desc.is_valid())) {
    ret = OB_INVALID_DATA;
    CLOG_LOG(WARN, "invalid single piece file", K(ret), K(dest_id),
        K(round_id), K(piece_id), K(id), K(desc));
  } else {
    exist = true;
    min_lsn = desc.min_lsn_;
    max_lsn = desc.max_lsn_;
  }
  return ret;
}

int ObLogArchivePieceContext::get_piece_file_range_()
{
  int ret = OB_SUCCESS;
  share::ObArchiveStore archive_store;
  int64_t min_file_id = 0;
  int64_t max_file_id = 0;
  if (! inner_piece_context_.is_active()) {
    // piece context is frozen or empty or low bound, file range is certain
  } else if (OB_FAIL(archive_store.init(archive_dest_))) {
    CLOG_LOG(WARN, "backup store init failed", K(ret), K_(archive_dest));
  } else if (OB_FAIL(archive_store.get_file_range_in_piece(dest_id_, inner_piece_context_.round_id_,
          inner_piece_context_.piece_id_, id_, min_file_id, max_file_id))
      && OB_ENTRY_NOT_EXIST != ret) {
    CLOG_LOG(WARN, "get file range failed", K(ret), K(inner_piece_context_), K(id_));
  } else if (OB_ENTRY_NOT_EXIST == ret) {
    CLOG_LOG(INFO, "file not exist in piece, rewrite ret code", K(ret), KPC(this));
    ret = OB_ITER_END;
  } else if (min_file_id <= 0 || max_file_id < min_file_id) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(ERROR, "invalid file id", K(ret), K(min_file_id),
        K(max_file_id), K(id_), K(inner_piece_context_));
  } else {
    inner_piece_context_.min_file_id_ = min_file_id;
    inner_piece_context_.max_file_id_ = max_file_id;
  }
  return ret;
}

int ObLogArchivePieceContext::forward_piece_()
{
  int ret = OB_SUCCESS;
  const int64_t piece_id = inner_piece_context_.piece_id_;
  if (inner_piece_context_.is_active()) {
    ret = OB_STATE_NOT_MATCH;
    CLOG_LOG(WARN, "piece context state not match", K(ret), KPC(this));
  } else if (piece_id >= round_context_.max_piece_id_) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "piece id not smaller than max piece id, can not forward piece", K(ret), KPC(this));
  } else if (inner_piece_context_.round_id_ != round_context_.round_id_) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "round id not match", K(ret), KPC(this));
  } else {
    inner_piece_context_.reset();
    inner_piece_context_.round_id_ = round_context_.round_id_;
    inner_piece_context_.piece_id_ = piece_id + 1;
    CLOG_LOG(INFO, "forward piece succ", K(ret), "pre_piece_id", piece_id, KPC(this));
  }
  return ret;
}

int ObLogArchivePieceContext::backward_piece_()
{
  int ret = OB_SUCCESS;
  const int64_t piece_id = inner_piece_context_.piece_id_;
  if (piece_id <= round_context_.min_piece_id_) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "piece id not bigger than min piece id, can not backward piece", K(ret), KPC(this));
  } else if (inner_piece_context_.round_id_ != round_context_.round_id_) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "round id not match", K(ret), KPC(this));
  } else {
    inner_piece_context_.reset();
    inner_piece_context_.round_id_ = round_context_.round_id_;
    inner_piece_context_.piece_id_ = piece_id - 1;
    CLOG_LOG(INFO, "backward piece succ", K(ret), "pre_piece_id", piece_id, KPC(this));
  }
  return ret;
}

int64_t ObLogArchivePieceContext::cal_piece_id_(const int64_t log_ts) const
{
  return share::ObArchivePiece(log_ts, round_context_.piece_switch_interval_,
      round_context_.base_piece_ts_, round_context_.base_piece_id_)
    .get_piece_id();
}

int ObLogArchivePieceContext::get_min_lsn_in_piece_()
{
  int ret = OB_SUCCESS;
  const int64_t piece_id = inner_piece_context_.piece_id_;
  const int64_t round_id = inner_piece_context_.round_id_;
  share::ObBackupPath piece_path;
  share::ObBackupPath path;
  char *buf = NULL;
  const int64_t buf_size = archive::ARCHIVE_FILE_HEADER_SIZE;
  int64_t read_size = 0;
  archive::ObArchiveFileHeader header;
  int64_t pos = 0;
  if (inner_piece_context_.is_empty_()
      || inner_piece_context_.is_low_bound_()
      || inner_piece_context_.min_lsn_in_piece_.is_valid()) {
    // do nothing
  } else if (OB_UNLIKELY(piece_id <= 0 || round_id <= 0)) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "invalid piece id or round id", K(ret), K(piece_id), K(round_id));
  } else if (OB_FAIL(share::ObArchivePathUtil::get_piece_dir_path(archive_dest_, dest_id_, round_id, piece_id, piece_path))) {
    CLOG_LOG(WARN, "get piece dir failed", K(ret), K(dest_id_), K(round_id), K(piece_id), K(archive_dest_));
  } else if (OB_FAIL(share::ObArchivePathUtil::build_restore_path(piece_path.get_ptr(),
          id_, inner_piece_context_.min_file_id_, path))) {
    CLOG_LOG(WARN, "build restore path failed", K(ret), K(piece_path), K(id_));
  } else if (OB_ISNULL(buf = (char *)mtl_malloc(buf_size, "ArcFile"))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    CLOG_LOG(WARN, "alloc memory failed", K(ret));
  } else if (OB_FAIL(archive::ObArchiveFileUtils::range_read(path.get_ptr(),
          archive_dest_.get_storage_info(), buf, buf_size, 0, read_size))) {
    CLOG_LOG(WARN, "range read failed", K(ret), K(path));
  } else if (OB_UNLIKELY(read_size != buf_size)) {
    ret = OB_INVALID_DATA;
    CLOG_LOG(WARN, "invalid data", K(ret), K(read_size), K(buf_size), K(path));
  } else if (OB_FAIL(header.deserialize(buf, buf_size, pos))) {
    CLOG_LOG(WARN, "archive file header deserialize failed", K(ret));
  } else if (OB_UNLIKELY(! header.is_valid())) {
    ret = OB_INVALID_DATA;
    CLOG_LOG(WARN, "archive file header not valid", K(ret), K(header), K(path));
  } else {
    inner_piece_context_.min_lsn_in_piece_ = header.start_lsn_;
    CLOG_LOG(INFO, "get min lsn in piece succ", K(ret), K(header), KPC(this));
  }
  if (NULL != buf) {
    mtl_free(buf);
    buf = NULL;
  }
  return ret;
}

int64_t ObLogArchivePieceContext::cal_archive_file_id_(const palf::LSN &lsn) const
{
  return archive::cal_archive_file_id(lsn, palf::PALF_BLOCK_SIZE);
}

int ObLogArchivePieceContext::get_(const palf::LSN &lsn,
    const int64_t file_id,
    int64_t &dest_id,
    int64_t &round_id,
    int64_t &piece_id,
    int64_t &offset,
    palf::LSN &max_lsn,
    bool &done)
{
  int ret = OB_SUCCESS;
  done = false;
  if (! inner_piece_context_.is_valid()
      || inner_piece_context_.is_empty_()
      || inner_piece_context_.is_low_bound_()) {
    // skip
  } else if (inner_piece_context_.is_fronze_()) {
    if (inner_piece_context_.min_lsn_in_piece_ <= lsn && inner_piece_context_.max_lsn_in_piece_ > lsn) {
      done = true;
    }
  } else {
    if (inner_piece_context_.min_lsn_in_piece_ <= lsn && file_id <= inner_piece_context_.max_file_id_) {
      done = true;
    }
  }

  if (done) {
    dest_id = dest_id_;
    round_id = inner_piece_context_.round_id_;
    piece_id = inner_piece_context_.piece_id_;
    if (inner_piece_context_.file_id_ == file_id && lsn >= inner_piece_context_.max_lsn_) {
      offset = inner_piece_context_.file_offset_;
      max_lsn = inner_piece_context_.max_lsn_;
    } else {
      offset = 0;
    }
  }

  // 已消费到最大依然没有定位到该LSN, 并且当前piece包含日志范围小于该LSN, 返回OB_ITER_END
  if (! done) {
    if (inner_piece_context_.is_valid()
        && inner_piece_context_.round_id_ == max_round_id_
        && inner_piece_context_.piece_id_ == round_context_.max_piece_id_
        && inner_piece_context_.min_lsn_in_piece_ < lsn) {
      ret = OB_ITER_END;
    }
  }

  // 该日志流在该piece已GC, 并且最大LSN小于等于需要获取的LSN, 返回OB_ITER_END
  if (! done) {
    if (inner_piece_context_.is_valid()
        && inner_piece_context_.is_gc_()
        && inner_piece_context_.max_lsn_in_piece_ <= lsn) {
      CLOG_LOG(INFO, "ls gc in this piece, and lsn bigger than ls max lsn, iter to end", K(lsn), KPC(this));
      ret = OB_ITER_END;
    }
  }

  // 该LSN日志已被回收
  // 最小round最小piece包含的最小lsn依然大于指定LSN
  if (! done) {
    if (inner_piece_context_.is_valid()
        && round_context_.is_valid()
        && inner_piece_context_.min_lsn_in_piece_.is_valid()
        && inner_piece_context_.piece_id_ == round_context_.min_piece_id_
        && inner_piece_context_.round_id_ == min_round_id_
        && inner_piece_context_.min_lsn_in_piece_ > lsn) {
      ret = OB_ARCHIVE_LOG_RECYCLED;
      CLOG_LOG(WARN, "lsn smaller than any log exist, maybe archive log had been recycled", K(lsn), KPC(this));
    }
  }
  return ret;
}

} // namespace logservice
} // namespace oceanbase
