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

#ifndef OCEANBASE_TRANSACTION_OB_TRANS_DEFINE_V4_
#define OCEANBASE_TRANSACTION_OB_TRANS_DEFINE_V4_

#include <cstdint>
#include <functional>
#include "lib/container/ob_iarray.h"
#include "lib/container/ob_mask_set2.h"
#include "lib/container/ob_se_array.h"
#include "lib/list/ob_list.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/trace/ob_trace_event.h"
#include "lib/utility/ob_unify_serialize.h"
#include "lib/container/ob_tuple.h"
#include "share/ob_cluster_version.h"
#include "share/ob_ls_id.h"
#include "ob_trans_hashmap.h"
#include "storage/tx/ob_trans_define.h"

namespace oceanbase
{
namespace transaction
{

struct ObTransIDAndAddr { // deadlock needed
  OB_UNIS_VERSION(1);
public:
  ObTransIDAndAddr() = default;
  ObTransIDAndAddr(const ObTransID id, const common::ObAddr &addr) : tx_id_(id), scheduler_addr_(addr) {}
  TO_STRING_KV(K_(tx_id), K_(scheduler_addr));
  bool operator==(const ObTransIDAndAddr &rhs) const { return tx_id_ == rhs.tx_id_ &&
                                                              scheduler_addr_ == rhs.scheduler_addr_; }
  bool is_valid() const { return tx_id_.is_valid() && scheduler_addr_.is_valid(); }
  ObTransID tx_id_;
  common::ObAddr scheduler_addr_;
};
OB_SERIALIZE_MEMBER_TEMP(inline, ObTransIDAndAddr, tx_id_, scheduler_addr_);

class ObITxCallback
{
public:
  virtual void callback(int ret) = 0;
};

template<typename L, typename R>
struct ObPair
{
  L left_; R right_;
public:
  ObPair() : left_(), right_() {}
  ObPair(const L &l, const R &r): left_(l), right_(r) {}
  ObPair& operator=(const ObPair &b)
  { left_ = b.left_; right_ = b.right_; return *this; }
  inline bool operator==(const ObPair &b) const {
    return b.left_ == left_ && b.right_ == right_;
  }
  TO_STRING_KV(K_(left), K_(right));
  NEED_SERIALIZE_AND_DESERIALIZE;
};

template<typename L, typename R>
int ObPair<L, R>::serialize(char *buf, const int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
  LST_DO_CODE(OB_UNIS_ENCODE, left_, right_);
  return ret;
}
template<typename L, typename R>
int ObPair<L, R>::deserialize(const char *buf, int64_t data_len, int64_t &pos)
{
  int ret = OB_SUCCESS;
  LST_DO_CODE(OB_UNIS_DECODE, left_, right_);
  return ret;
}

template<typename L, typename R>
int64_t ObPair<L, R>::get_serialize_size() const
{
  int64_t len = 0;
  LST_DO_CODE(OB_UNIS_ADD_LEN, left_, right_);
  return len;
}

template<typename T, int N = 4>
class ObRefList
{
private:
  ObSEArray<T*, N> ref_list_;
public:
  T& operator [](int index) { return *ref_list_[index]; }
  const T& operator [](int index) const { return *ref_list_[index]; }
  int push_back(T &p) { return ref_list_.push_back(&p); }
  int64_t count() const { return ref_list_.count(); }
  DECLARE_TO_STRING {
    int64_t pos = 0;
    J_ARRAY_START();
    ARRAY_FOREACH_NORET(ref_list_, i) {
      pos += ref_list_[i]->to_string(buf + pos, buf_len - pos);
      J_COMMA();
    }
    J_ARRAY_END();
    return pos;
  }
};

enum ObTxAbortCause
{
  TX_RESULT_INCOMPLETE = 1,
  IN_CONSIST_STATE = 2,
  SAVEPOINT_ROLLBACK_FAIL = 3,
  IMPLICIT_ROLLBACK = 4,
  SESSION_DISCONNECT = 5,
  STOP = 6,
  PARTICIPANT_STATE_INCOMPLETE = 7,
  PARTICIPANTS_SET_INCOMPLETE = 8,
};

enum class ObTxClass { USER, SYS };

enum class ObTxConsistencyType
{
  INVALID = 0,
  CURRENT_READ = 1,
  BOUNDED_STALENESS_READ = 2, // AKA. WeakRead
};

enum class ObTxIsolationLevel
{
  INVALID = -1,
  RU = 0,
  RC = 1,
  RR = 2,
  SERIAL = 3,
};

extern ObTxIsolationLevel tx_isolation_from_str(const ObString &s);

enum class ObTxAccessMode
{
  INVL = -1, RW = 0, RD_ONLY = 1
};

struct ObTxParam
{
  ObTxParam();
  bool is_valid() const;
  ~ObTxParam();
  int64_t timeout_us_;
  int64_t lock_timeout_us_;
  ObTxAccessMode access_mode_;
  ObTxIsolationLevel isolation_;
  int64_t cluster_id_;
  TO_STRING_KV(K_(cluster_id),
               K_(timeout_us),
               K_(lock_timeout_us),
               K_(access_mode),
               K_(isolation));
  OB_UNIS_VERSION(1);
};

struct ObTxPart
{
  ObTxPart();
  ~ObTxPart();
  share::ObLSID id_;             // identifier, the logstream
  ObAddr addr_;           // its latest address
  int64_t epoch_;         // used to judge a ctx not revived
  int64_t first_scn_;      // used to judge a ctx is clean in scheduler view
  int64_t last_scn_;       // used to get rollback savepoint set
  int64_t last_touch_ts_; // used to judge a ctx retouched after a time point
  bool operator==(const ObTxPart &rhs) const { return id_ == rhs.id_ && addr_ == rhs.addr_; }
  bool operator!=(const ObTxPart &rhs) const { return !operator==(rhs); }
  bool is_clean() const { return first_scn_ > last_scn_; }
  TO_STRING_KV(K_(id), K_(addr), K_(epoch), K_(first_scn), K_(last_scn), K_(last_touch_ts));
  OB_UNIS_VERSION(1);
};

typedef ObSEArray<ObTxPart, 4> ObTxPartList;
typedef ObRefList<ObTxPart, 4> ObTxPartRefList;
typedef ObPair<share::ObLSID, int64_t> ObTxLSEpochPair;

// internal core snapshot for read data
struct ObTxSnapshot
{
  int64_t version_;
  ObTransID tx_id_;
  int64_t scn_;
  bool elr_;
  TO_STRING_KV(K_(version), K_(tx_id), K_(scn));
  ObTxSnapshot();
  ~ObTxSnapshot();
  void reset();
  ObTxSnapshot &operator=(const ObTxSnapshot &r);
  bool is_valid() const { return version_ > 0; }
  OB_UNIS_VERSION(1);
};

// snapshot used to consistency read
struct ObTxReadSnapshot
{
  bool valid_;
  ObTxSnapshot core_;
  enum class SRC {
    INVL = 0,
    GLOBAL = 1,             // normal snapshot, external consistency complied
    LS = 2,                 // only access one logstream
    WEAK_READ_SERVICE = 3,  // do a bounded stale read, without linearizable consistency
    SPECIAL = 4,            // user specify
    NONE = 5,               // won't read, global snapshot not required
  } source_;
  share::ObLSID snapshot_lsid_;    // for source_ = LOCAL
  int64_t uncertain_bound_; // for source_ GLOBAL
  ObSEArray<ObTxLSEpochPair, 1> parts_;

  void init_weak_read(const int64_t snapshot);
  void init_special_read(const int64_t snapshot);
  void init_none_read() { valid_ = true; source_ = SRC::NONE; }
  void init_ls_read(const share::ObLSID &ls_id, const ObTxSnapshot &core);
  void wait_consistency();
  ObString get_source_name() const;
  bool is_weak_read() const { return SRC::WEAK_READ_SERVICE == source_; };
  bool is_none_read() const { return SRC::NONE == source_; }
  bool is_special() const { return SRC::SPECIAL == source_; }
  bool is_ls_snapshot() const { return SRC::LS == source_; }
  void reset();
  int assign(const ObTxReadSnapshot &);
  ObTxReadSnapshot();
  ~ObTxReadSnapshot();
  TO_STRING_KV(KP(this),
               K_(valid),
               K_(source),
               K_(core),
               K_(uncertain_bound),
               K_(snapshot_lsid),
               K_(parts));
  OB_UNIS_VERSION(1);
};

class ObTxSavePoint
{
  friend class ObTransService;
  friend class ObTxDesc;
private:
  enum class T { INVL= 0, SAVEPOINT= 1, SNAPSHOT= 2, STASH= 3 } type_;
  int64_t scn_;
  union {
    ObTxReadSnapshot *snapshot_;
    common::ObFixedLengthString<128> name_;
  };
public:
  ObTxSavePoint();
  ~ObTxSavePoint();
  ObTxSavePoint(const ObTxSavePoint &s);
  ObTxSavePoint &operator=(const ObTxSavePoint &a);
  void release();
  void rollback();
  int init(const int64_t scn, const ObString &name, const bool stash = false);
  void init(ObTxReadSnapshot *snapshot);
  bool is_savepoint() const { return type_ == T::SAVEPOINT || type_ == T::STASH; }
  bool is_snapshot() const { return type_ == T::SNAPSHOT; }
  bool is_stash() const { return type_ == T::STASH; }
  DECLARE_TO_STRING;
};

typedef ObSEArray<ObTxSavePoint, 4> ObTxSavePointList;

class ObTxExecResult
{
  friend class ObTransService;
  friend class ObTxDesc;
  OB_UNIS_VERSION(1);
  bool incomplete_; // TODO: (yunxing.cyx) remove, required before sql use new API
  share::ObLSArray touched_ls_list_;
  ObTxPartList parts_;
  ObSArray<ObTransIDAndAddr> cflict_txs_;
public:
  ObTxExecResult();
  ~ObTxExecResult();
  void reset();
  TO_STRING_KV(K_(incomplete), K_(parts), K_(touched_ls_list), K_(cflict_txs));
  void set_incomplete() {
    TRANS_LOG(TRACE, "tx result incomplete:", KP(this));
    incomplete_ = true;
  }
  void merge_cflict_txs(const common::ObIArray<ObTransIDAndAddr> &txs);
  inline bool is_incomplete() const { return incomplete_; }
  int add_touched_ls(const share::ObLSID ls);
  int add_touched_ls(const ObIArray<share::ObLSID> &ls_list);
  const share::ObLSArray &get_touched_ls() const { return touched_ls_list_; }
  int merge_result(const ObTxExecResult &r);
  int assign(const ObTxExecResult &r);
  const ObSArray<ObTransIDAndAddr> &get_conflict_txs() const { return cflict_txs_; }
};

class ObTxDesc final : public ObTransHashLink<ObTxDesc>
{
  static constexpr const char *OP_LABEL = "TX_DESC_VALUE";
  static constexpr int64_t MAX_RESERVED_CONFLICT_TX_NUM = 30;
  friend class ObTransService;
  friend class ObTxDescMgr;
  friend class ObPartTransCtx;
  friend class StopTxDescFunctor;
  friend class ObTxStmtInfo;
  typedef common::ObMaskSet2<ObTxLSEpochPair> MaskSet;
  OB_UNIS_VERSION(1);
protected:
  uint64_t tenant_id_;        // FIXME: removable
  // Identify the ownership of data when the A database and
  // the B database synchronize data with each other
  int64_t cluster_id_;
  ObTraceInfo trace_info_;
  uint64_t cluster_version_;  // compatible handle when upgrade

  ObTxConsistencyType tx_consistency_type_; // transaction level consistency_type : strong or bounded read

  common::ObAddr addr_;                // where we site
  ObTransID tx_id_;                    // identifier
  ObXATransID xid_;                    // xa info if participant in XA
  ObTxIsolationLevel isolation_;       // isolation level
  ObTxAccessMode access_mode_;         // READ_ONLY | READ_WRITE
  int64_t snapshot_version_;           // snapshot for RR | SERIAL Isolation
  int64_t snapshot_uncertain_bound_;   // uncertain bound of @snapshot_version_
  int64_t snapshot_scn_;               // the time of acquire @snapshot_version_
  uint32_t sess_id_;                   // sesssion id

  uint64_t op_sn_;                     // Tx level operation sequence No

  enum class State                     // State of Tx
  {
    INVL,
    IDLE,               // created
    ACTIVE,             // explicit started
    IMPLICIT_ACTIVE,    // implicit started
    ROLLBACK_SAVEPOINT, // rolling back to savepoint
    IN_TERMINATE,       // committing, aborting
    ABORTED,            // internal rolled back
    ROLLED_BACK,        // rolled back
    COMMIT_TIMEOUT,     // commit timeouted
    COMMIT_UNKNOWN,     // commit complted but result unknown, either committed or aborted
    COMMITTED,          // committed
    SUB_PREPARING,      // XA prepare started
    SUB_PREPARED,       // XA prepare response received
    SUB_COMMITTING,     // XA commit started
    SUB_COMMITTED,      // XA commit response received
    SUB_ROLLBACKING,    // XA rollback started
    SUB_ROLLBACKED,     // XA rollback response received
  } state_;

  union FLAG                         // flags
  {
    uint64_t v_;
    struct
    {
      bool SHADOW_:1;                // this tx desc is a shadow copy
      bool REPLICA_:1;               // a replica of primary/original, its state is transient, without whole lifecyle
      bool TRACING_:1;               // tracing the Tx
      bool INTERRUPTED_: 1;          // a single for blocking operation
      bool RELEASED_: 1;             // after released, commit can give up
      bool BLOCK_: 1;                // tx is blocking within some loop
      bool PARTS_INCOMPLETE_: 1;     // participants set incomplete (must abort)
      bool PART_EPOCH_MISMATCH_: 1;  // participant's born epoch mismatched
    };
    void switch_to_idle_();
  } flags_;

  int64_t alloc_ts_;                 // time of allocated
  int64_t active_ts_;                // time of ACTIVE | IMPLICIT_ACTIVE
  int64_t timeout_us_;               // tx parameters from ObTxParam
  int64_t lock_timeout_us_;          // lock conflict wait timeout in micorsecond
  int64_t expire_ts_;                // tick when ACTIVE
  int64_t commit_ts_;                // COMMIT start time
  int64_t finish_ts_;                // COMMIT/ABORT finish time

  int64_t active_scn_;               // logical time of ACTIVE | IMPLICIT_ACTIVE
  int64_t min_implicit_savepoint_;   // mininum of implicit savepoints
  ObTxPartList parts_;               // participant list
  ObTxSavePointList savepoints_;     // savepoints established
  // cflict_txs_ is used to store conflict trans id when try acquire row lock failed(meet lock conflict)
  // this information will used to detect deadlock
  // cflict_txs_ is valid when transaction is not executed on local
  // on scheduler, cflict_txs_ merges all participants executed results on remote
  // on participant, cflict_txs_ temporary stores conflict information, and will be read by upper layers, bring back to scheduler
  ObSArray<ObTransIDAndAddr> cflict_txs_;

  // used during commit
  share::ObLSID coord_id_;           // coordinator ID
  int64_t commit_expire_ts_;         // commit operation deadline
  share::ObLSArray commit_parts_;    // participants to do commit
  int64_t commit_version_;           // Tx commit version
  int commit_out_;                   // the commit result
  /* internal abort cause */
  int16_t abort_cause_;              // Tx Aborted cause
  bool can_elr_;                     // can early lock release

private:
  // FOLLOWING are runtime auxiliary fields
  ObSpinLock lock_;
  ObSpinLock commit_cb_lock_;       // protect commit_cb_ field
  ObITxCallback *commit_cb_;        // async commit callback
  int64_t exec_info_reap_ts_;       // the time reaping incremental tx exec info
  MaskSet brpc_mask_set_;           // used in message driven savepoint rollback
  ObTransCond rpc_cond_;            // used in message driven savepoint rollback

  ObTxTimeoutTask commit_task_;     // commit retry task
  ObXACtx *xa_ctx_;                 // xa context
  ObTransTraceLog tlog_;
private:
  /* these routine should be called by txn-service only to avoid corrupted state */
  void reset();
  void set_tx_id(const ObTransID &tx_id) { tx_id_ = tx_id; }
  void reset_tx_id() { tx_id_.reset(); }
  // udpate clean part's unknown field
  int update_clean_part(const share::ObLSID &id,
                        const int64_t epoch,
                        const ObAddr &addr);
  int update_part(ObTxPart &p);
  int update_parts(const share::ObLSArray &parts);
  int switch_to_idle();
  int set_commit_cb(ObITxCallback *cb);
  void execute_commit_cb();
private:
  int update_part_(ObTxPart &p, bool append = true);
  int merge_conflict_txs_(const ObIArray<ObTransIDAndAddr> &conflict_ids);
  int update_parts_(const ObTxPartList &list);
  void implicit_start_tx_();
  bool acq_commit_cb_lock_if_need_();
  void print_trace_() const;
public:
  ObTxDesc();
  ~ObTxDesc();
  TO_STRING_KV(KP(this),
               K_(tx_id),
               K_(state),
               K_(addr),
               K_(tenant_id),
               "session_id", sess_id_,
               "xid", PC((!xid_.empty() ? &xid_ : (ObXATransID*)nullptr)),
               K_(access_mode),
               K_(tx_consistency_type),
               K_(isolation),
               K_(snapshot_version),
               K_(snapshot_scn),
               K_(active_scn),
               K_(op_sn),
               K_(alloc_ts),
               K_(active_ts),
               K_(commit_ts),
               K_(finish_ts),
               K_(timeout_us),
               K_(lock_timeout_us),
               K_(expire_ts),
               K_(coord_id),
               K_(parts),
               K_(exec_info_reap_ts),
               K_(commit_version),
               KP_(commit_cb),
               K_(cluster_id),
               K_(cluster_version),
               K_(flags_.SHADOW),
               K_(flags_.INTERRUPTED),
               K_(flags_.BLOCK),
               K_(flags_.REPLICA),
               K_(can_elr),
               K_(cflict_txs),
               K_(abort_cause),
               K_(commit_expire_ts),
               K(commit_task_.is_registered()),
               K_(ref));

  int get_conflict_txs(ObIArray<ObTransIDAndAddr> &array)
  { ObSpinLockGuard guard(lock_); return array.assign(cflict_txs_); }
  void reset_conflict_txs()
  { ObSpinLockGuard guard(lock_); cflict_txs_.reset(); }
  int merge_conflict_txs(const ObIArray<ObTransIDAndAddr> &conflict_ids);
  bool contain(const ObTransID &trans_id) const { return tx_id_ == trans_id; } /*used by TransHashMap*/
  uint64_t get_tenant_id() const { return tenant_id_; }
  void set_cluster_id(uint64_t cluster_id) { cluster_id_ = cluster_id; }
  uint64_t get_cluster_id() const { return cluster_id_; }
  uint32_t get_session_id() const { return sess_id_; }
  ObAddr get_addr() const { return addr_; }
  uint64_t get_cluster_version() const { return cluster_version_; }
  ObTxConsistencyType get_tx_consistency_type() const { return tx_consistency_type_; }
  ObTxIsolationLevel get_isolation_level() const { return isolation_; }
  const ObTransID &tid() const { return tx_id_; }
  bool is_valid() const { return !is_in_tx() || tx_id_.is_valid(); }
  ObTxAccessMode get_access_mode() const { return access_mode_; }
  bool is_rdonly() const { return access_mode_ == ObTxAccessMode::RD_ONLY; }
  int64_t get_op_sn() const { return op_sn_; }
  int inc_op_sn() { return ++op_sn_; }
  int64_t get_commit_version() const { return commit_version_; }
  bool contain_savepoint(const ObString &sp);
  bool is_tx_end() {
    return is_committed() || is_rollbacked();
  }
  bool is_committing() {
    return state_ == State::IN_TERMINATE
      || state_ == State::SUB_PREPARING
      || state_ == State::SUB_COMMITTING
      || state_ == State::SUB_ROLLBACKING;
  }
  bool is_terminated() {
    return state_ == State::ABORTED || is_tx_end();
  }
  bool is_committed() {
    return state_ == State::COMMITTED
      || state_ == State::COMMIT_TIMEOUT
      || state_ == State::COMMIT_UNKNOWN
      || state_ == State::SUB_COMMITTED;
  }
  bool is_rollbacked() {
    return state_ == State::ROLLED_BACK
      || state_ == State::SUB_ROLLBACKED;
  }
  bool is_commit_unsucc() {
    return state_ == State::COMMIT_TIMEOUT
      || state_ == State::COMMIT_UNKNOWN
      || state_ == State::ROLLED_BACK;
  }
  bool is_tx_timeout() { return ObClockGenerator::getClock() > expire_ts_; }
  bool is_tx_commit_timeout() { return ObClockGenerator::getClock() > commit_expire_ts_;}
  void set_xa_ctx(ObXACtx *xa_ctx) { xa_ctx_ = xa_ctx; }
  ObXACtx *get_xa_ctx() { return xa_ctx_; }
  void set_xid(const ObXATransID &xid) { xid_ = xid; }
  const ObXATransID &get_xid() const { return xid_; }
  bool is_xa_trans() const { return !xid_.empty(); }
  void reset_for_xa() { xid_.reset(); xa_ctx_ = NULL; }
  int trans_deep_copy(const ObTxDesc &x);
  int64_t get_active_ts() const { return active_ts_; }
  int64_t get_expire_ts() const;
  int64_t get_tx_lock_timeout() const { return lock_timeout_us_; }
  bool is_in_tx() const { return state_ > State::IDLE; }
  bool is_tx_active() const { return state_ >= State::ACTIVE && state_ < State::IN_TERMINATE; }
  void print_trace();
  bool can_free_route() const;
  const ObTransID &get_tx_id() const { return tx_id_; }
  ObITxCallback *get_end_tx_cb() { return commit_cb_; }
  void reset_end_tx_cb() { commit_cb_ = NULL; }
  const ObString &get_tx_state_str() const;
  int merge_exec_info_with(const ObTxDesc &other);
  int get_inc_exec_info(ObTxExecResult &exec_info);
  int add_exec_info(const ObTxExecResult &exec_info);
  bool has_implicit_savepoint() const;
  void add_implicit_savepoint(const int64_t savepoint);
  void release_all_implicit_savepoint();
  void release_implicit_savepoint(const int64_t savepoint);
  ObTransTraceLog &get_tlog() { return tlog_; }
  bool is_xa_terminate_state_() const;
  void set_can_elr(const bool can_elr) { can_elr_ = can_elr; }
  bool is_can_elr() const { return can_elr_; }
  bool need_rollback() { return state_ == State::ABORTED; }
  ObITxCallback *cancel_commit_cb();
};

class ObTxDescMgr final
{
public:
  ObTxDescMgr(ObTransService &txs): inited_(false), stoped_(true), tx_id_allocator_(), txs_(txs) {}
 ~ObTxDescMgr() { inited_ = false; stoped_ = true; }
  int init(std::function<int(ObTransID&)> tx_id_allocator, const lib::ObMemAttr &mem_attr);
  int start();
  int stop();
  int wait();
  void destroy();
  int alloc(ObTxDesc *&tx_desc);
  void free(ObTxDesc *tx_desc);
  int add(ObTxDesc &tx_desc);
  int add_with_txid(const ObTransID &tx_id, ObTxDesc &tx_desc);
  int get(const ObTransID &tx_id, ObTxDesc *&tx_desc);
  void revert(ObTxDesc &tx);
  int remove(ObTxDesc &tx);
  int acquire_tx_ref(const ObTransID &trans_id);
  int release_tx_ref(ObTxDesc *tx_desc);
  int64_t get_alloc_count() const { return map_.alloc_cnt(); }
  int64_t get_total_count() const { return map_.count(); }
private:
  struct {
    bool inited_: 1;
    bool stoped_: 1;
  };
  class ObTxDescAlloc
  {
  public:
    ObTxDescAlloc(): alloc_cnt_(0) {}
    ObTxDesc* alloc_value()
    {
      ATOMIC_INC(&alloc_cnt_);
      return op_alloc(ObTxDesc);
    }
    void free_value(ObTxDesc *v)
    {
      if (NULL != v) {
        ATOMIC_DEC(&alloc_cnt_);
        op_free(v);
      }
    }
    int64_t get_alloc_cnt() const { return ATOMIC_LOAD(&alloc_cnt_); }
  private:
    int64_t alloc_cnt_;
  };
  ObTransHashMap<ObTransID, ObTxDesc, ObTxDescAlloc, common::SpinRWLock, 1 << 16 /*bucket_num*/> map_;
  std::function<int(ObTransID&)> tx_id_allocator_;
  ObTransService &txs_;
};

class ObTxInfo
{
  friend class ObTransService;
  OB_UNIS_VERSION(1);
protected:
  uint64_t tenant_id_;
  int64_t cluster_id_;
  uint64_t cluster_version_;
  common::ObAddr addr_;
  ObTransID tx_id_;
  ObTxIsolationLevel isolation_;
  ObTxAccessMode access_mode_;
  int64_t snapshot_version_;
  int64_t snapshot_uncertain_bound_;
  uint64_t op_sn_;
  int64_t alloc_ts_;
  int64_t active_ts_;
  int64_t timeout_us_;
  int64_t expire_ts_;
  int64_t finish_ts_;
  int64_t active_scn_;
  ObTxPartList parts_;
public:
  TO_STRING_KV(K_(tenant_id),
               K_(tx_id),
               K_(access_mode),
               K_(isolation),
               K_(snapshot_version),
               K_(active_scn),
               K_(op_sn),
               K_(alloc_ts),
               K_(active_ts),
               K_(timeout_us),
               K_(expire_ts),
               K_(parts),
               K_(cluster_id),
               K_(cluster_version));
  // TODO xa
  bool is_valid() const { return tx_id_.is_valid(); }
  const ObTransID &tid() const { return tx_id_; }
};

class ObTxStmtInfo
{
  friend class ObTransService;
  OB_UNIS_VERSION(1);
protected:
  ObTransID tx_id_;
  uint64_t op_sn_;
  ObTxPartList parts_;
  ObTxDesc::State state_;
public:
  TO_STRING_KV(K_(tx_id),
               K_(op_sn),
               K_(parts),
               K_(state));
  // TODO xa
  bool is_valid() const { return tx_id_.is_valid(); }
  const ObTransID &tid() const { return tx_id_; }
};

class TxCtxRoleState
{
public:
  static const int64_t INVALID = -1;
  static const int64_t LEADER = 0;
  static const int64_t FOLLOWER = 1;
  static const int64_t MAX = 2;

  static bool is_valid(const int64_t state)
  { return state > INVALID && state < MAX; }
};
class TxCtxOps
{
public:
  static const int64_t INVALID = -1;
  static const int64_t TAKEOVER = 0;
  static const int64_t REVOKE = 1;
  static const int64_t RESUME = 2;
  static const int64_t SWITCH_GRACEFUL = 3;
  static const int64_t MAX = 4;

  static bool is_valid(const int64_t ops)
  { return ops > INVALID && ops < MAX; }
};
class TxCtxStateHelper
{
public:
  explicit TxCtxStateHelper(int64_t &state) : state_(state),
                                         last_state_(TxCtxRoleState::INVALID),
                                         is_switching_(false) {}
  ~TxCtxStateHelper() {}
  int switch_state(const int64_t op);
  void restore_state();
private:
  int64_t &state_;
  int64_t last_state_;
  bool is_switching_;
};

#define REC_TRANS_TRACE(recorder_ptr, trace_event) do {   \
  if (NULL != recorder_ptr) {                             \
    REC_TRACE(*recorder_ptr, trace_event);                \
  }                                                       \
} while (0)

#define REC_TRANS_TRACE_EXT(recorder_ptr, trace_event, pairs...) do {  \
  if (NULL != recorder_ptr) {                                          \
    REC_TRACE_EXT(*recorder_ptr, trace_event, ##pairs);                \
  }                                                                    \
} while (0)

#define REC_TRANS_TRACE_EXT2(recorder_ptr, trace_event, pairs...) do { \
  if (NULL != recorder_ptr) {                                          \
    REC_TRACE_EXT(*recorder_ptr, trace_event, ##pairs, OB_ID(opid), opid_);\
  }                                                                    \
} while (0)

} // transaction
} // oceanbase

#endif // OCEANBASE_TRANSACTION_OB_TRANS_DEFINE_V4_
