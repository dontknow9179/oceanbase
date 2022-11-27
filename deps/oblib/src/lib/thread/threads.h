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

#ifndef USER_THREAD_H
#define USER_THREAD_H

#include <functional>
#include "lib/ob_errno.h"
#include "lib/thread/thread.h"
#include "lib/utility/ob_macro_utils.h"
#include "lib/alloc/alloc_assist.h"
#include "lib/lock/ob_spin_rwlock.h"

extern int64_t global_thread_stack_size;

namespace oceanbase {
namespace lib {

enum ThreadCGroup
{
  INVALID_CGROUP = 0,
  FRONT_CGROUP = 1,
  BACK_CGROUP = 2,
};

class Threads;
class IRunWrapper
{
public:
  virtual ~IRunWrapper() {}
  virtual int pre_run(Threads*)
  {
    int ret = OB_SUCCESS;
    return ret;
  }
  virtual int end_run(Threads*)
  {
    int ret = OB_SUCCESS;
    return ret;
  }
};

class Threads
{
public:
  explicit Threads(int64_t n_threads = 1)
      : n_threads_(n_threads),
        threads_(nullptr),
        stack_size_(global_thread_stack_size),
        stop_(true),
        thread_max_tasks_(INT64_MAX),
        run_wrapper_(nullptr),
        cgroup_(INVALID_CGROUP)
  {}
  virtual ~Threads();
  static IRunWrapper *&get_expect_run_wrapper();

  /// \brief Set number of threads for running.
  ///
  /// When set before threads are running, this function simply set
  /// local varible which would be read for \c run().
  ///
  /// When set after threads are running, this function would adjust
  /// real threads count other than set local variable.
  ///
  /// \param n_threads Number of threads to set.
  ///
  /// \return Return OB_SUCCESS if threads count has successfully
  ///         adjust to that number, i.e. there are such exact number
  ///         of threads are running if it has started, or would run
  ///         after call \c start() function.
  int do_set_thread_count(int64_t n_threads);
  int set_thread_count(int64_t n_threads);
  int inc_thread_count(int64_t inc = 1);

  int init();
  // IRunWrapper 用于创建多租户线程时指定租户上下文
  // cgroup_ctrl 和IRunWrapper配合使用，实现多租户线程的CPU隔离
  void set_run_wrapper(IRunWrapper *run_wrapper, ThreadCGroup cgroup = ThreadCGroup::FRONT_CGROUP)
  {
    run_wrapper_ = run_wrapper;
    cgroup_ = cgroup;
  }
  virtual int start();
  virtual void stop();
  virtual void wait();
  void destroy();

  pid_t get_tid() const
  {
    OB_ASSERT(n_threads_ > 0);
    return threads_[0]->get_tid();
  }
public:
  void set_thread_max_tasks(uint64_t cnt);

  template <class Functor>
  int submit(const Functor &func)
  {
    UNUSED(func);
    int ret = OB_SUCCESS;
    return ret;
  }
  ThreadCGroup get_cgroup() { return cgroup_; }
protected:
  virtual bool has_set_stop() const { return ATOMIC_LOAD(&stop_); }
  bool &has_set_stop() { return stop_; }
  int64_t get_thread_count() const { return n_threads_; }
  uint64_t get_thread_idx() const { return thread_idx_; }

private:
  virtual void run(int64_t idx);
  virtual void run1() {}

  /// \brief Create thread with start entry \c entry.
  int create_thread(Thread *&thread, std::function<void()> entry);

  /// \brief Destroy thread.
  void destroy_thread(Thread *thread);

private:
  static thread_local uint64_t thread_idx_;
  int64_t n_threads_;
  Thread **threads_;
  int64_t stack_size_;
  bool stop_;
  // protect for thread count changing.
  common::SpinRWLock lock_;
  int64_t thread_max_tasks_;
  // tenant ctx
  IRunWrapper *run_wrapper_;
  // thread cgroups
  ThreadCGroup cgroup_;
  //
};

using ThreadPool = Threads;

}  // lib
}  // oceanbase


#endif /* USER_THREAD_H */
