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

#define USING_LOG_PREFIX RPC_OBMYSQL
#include "rpc/obmysql/ob_sql_nio.h"
#include "rpc/obmysql/ob_sql_sock_session.h"
#include "rpc/obmysql/ob_i_sql_sock_handler.h"
#include "rpc/obmysql/ob_sql_sock_session.h"
#include "lib/oblog/ob_log.h"
#include "lib/allocator/ob_malloc.h"
#include "lib/queue/ob_link_queue.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/thread/ob_thread_name.h"
#include "lib/utility/ob_macro_utils.h"
#include "lib/profile/ob_trace_id.h"
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <arpa/inet.h>
#include <linux/futex.h>
#include "rpc/obrpc/ob_listener.h"

using namespace oceanbase::common;

#ifndef SO_REUSEPORT
#define SO_REUSEPORT 15
#endif

namespace oceanbase
{
namespace obmysql
{
class ObDList
{

public:
  typedef ObDLink DLink;
  ObDList() {
    head_.next_ = &head_;
    head_.prev_ = &head_;
  }
  ~ObDList() {}
  DLink* head() { return &head_; }
  void add(DLink* p) {
    DLink* prev = head_.prev_;
    DLink* next = &head_;
    p->prev_ = prev;
    p->next_ = next;
    prev->next_ = p;
    next->prev_ = p;
  }
  void del(DLink* p) {
    DLink* prev = p->prev_;
    DLink* next = (DLink*)p->next_;
    prev->next_ = next;
    next->prev_ = prev;
  }
private:
  DLink head_;
};

struct ReadyFlag
{
  ReadyFlag(): pending_(0) {}
  ~ReadyFlag() {}
  bool set_ready() {
    int32_t pending = ATOMIC_LOAD(&pending_);
    if (pending <= 1) {
      pending = ATOMIC_FAA(&pending_, 1);
    }
    return pending == 0;
  }
  bool end_handle(bool still_ready) {
    int32_t pending = 1;
    if (!still_ready) {
      pending = ATOMIC_AAF(&pending_, -1);
    }
    return pending == 0;
  }
  int32_t get_pending_flag() const { return ATOMIC_LOAD(&pending_); }
  int32_t pending_ CACHE_ALIGNED;
};

#define futex(...) syscall(SYS_futex,__VA_ARGS__)
static int futex_wake(volatile int *p, int val)
{
  return static_cast<int>(futex((int *)p, FUTEX_WAKE_PRIVATE, val, NULL, NULL, 0));
}

struct SingleWaitCond
{
public:
  SingleWaitCond(): ready_(0) {}
  ~SingleWaitCond() {}
  void signal()
  {
    if (!ATOMIC_LOAD(&ready_) && ATOMIC_BCAS(&ready_, 0, 1)) {
      futex_wake(&ready_, 1);
    }
  }
  void wait(int64_t timeout)
  {
    if (!ATOMIC_LOAD(&ready_)) {
      struct timespec ts;
      make_timespec(&ts, timeout);
      futex_wait(&ready_, 0, &ts);
    }
    ATOMIC_STORE(&ready_, 0);
  }
private:
  static struct timespec *make_timespec(struct timespec *ts, int64_t us)
  {
    ts->tv_sec = us / 1000000;
    ts->tv_nsec = 1000 * (us % 1000000);
    return ts;
  }
private:
  int32_t ready_ CACHE_ALIGNED;
};

class ReadBuffer
{
public:
  enum { IO_BUFFER_SIZE = 1<<16 };
  ReadBuffer(int fd): fd_(fd), has_EAGAIN_(false), request_more_data_(false),
                alloc_buf_(NULL), buf_end_(NULL), cur_buf_(NULL), data_end_(NULL),
                consume_sz_(0)
  {}
  ~ReadBuffer() 
  {
    if (NULL != alloc_buf_) {
      direct_free(alloc_buf_);
    }
  }
  int64_t get_remain_sz() const { return remain(); }
  void set_fd(int fd) { fd_ = fd; }
  int peek_data(int64_t limit, const char*& buf, int64_t& sz) {
    int ret = OB_SUCCESS;
    if (OB_FAIL(try_read_fd(limit))) {
      LOG_WARN("read fail", K(ret), K_(fd), K(limit));
    } else {
      buf = cur_buf_;
      sz = remain();
      if (sz < limit) {
        request_more_data_ = true;
      }
      LOG_DEBUG("peek data", K_(fd), K(limit), K(sz));
    }
    return ret;
  }
  int consume_data(int64_t sz) {
    int ret = OB_SUCCESS;
    if (sz > 0 && sz <= remain()) {
      cur_buf_ += sz;
      consume_sz_ += sz;
      LOG_DEBUG("consume data", K_(fd), K(sz));
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("consume data, invalid argument", K_(fd), K(sz));
    }
    return ret;
  }
  bool clear_EAGAIN() {
    bool ret = (has_EAGAIN_ && (remain() <= 0 || request_more_data_));
    has_EAGAIN_ = false;
    request_more_data_ = false;
    return ret;
  }
  uint64_t get_consume_sz() const {return ATOMIC_LOAD(&consume_sz_); }
private:
  int try_read_fd(int64_t limit) {
    int ret = OB_SUCCESS;
    if (limit <= 0) {
      ret = OB_INVALID_ARGUMENT;
    } else if (remain() >= limit) {
      
    } else if (cur_buf_ + limit > buf_end_ && OB_FAIL(switch_buffer(limit))) {
      LOG_ERROR("alloc read buffer fail", K_(fd), K(ret));
    } else if (OB_FAIL(do_read_fd(limit))) {
      LOG_WARN("do_read_fd fail", K(ret), K_(fd), K(limit));
    }
    return ret;
  }
  int64_t remain() const { return data_end_ - cur_buf_; }
  int switch_buffer(int64_t limit) {
    int ret = OB_SUCCESS;
    int64_t alloc_size = std::max(limit, (int64_t)IO_BUFFER_SIZE);
    char* new_buf = NULL;
    if (alloc_buf_ + alloc_size <= buf_end_) {
      int64_t rsz = remain();
      memmove(alloc_buf_, cur_buf_, rsz);
      cur_buf_ = alloc_buf_;
      data_end_ = cur_buf_ + rsz;
    } else if (NULL == (new_buf = (char*)alloc_io_buffer(alloc_size))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("alloc buffer fail", K(ret), K_(fd), K(alloc_size));
    } else {
      char* old_buffer = alloc_buf_;
      int64_t rsz = remain();
      if (rsz > 0) {
        memcpy(new_buf, cur_buf_, rsz);
      }
      alloc_buf_ = new_buf;
      buf_end_ = new_buf + alloc_size;
      cur_buf_ = new_buf;
      data_end_ = new_buf + rsz;
      free_io_buffer(old_buffer);
    }
    return ret;
  }
  int do_read_fd(int64_t sz) {
    int ret = OB_SUCCESS;
    while(remain() < sz && OB_SUCCESS == ret) {
      int64_t rbytes = 0;
      if ((rbytes = read(fd_, data_end_, buf_end_ - data_end_)) > 0) {
        data_end_ += rbytes;
      } else if (0 == rbytes) {
        LOG_INFO("read fd return EOF", K_(fd));
        has_EAGAIN_ = true;
        ret = OB_IO_ERROR; // for mysql protocol, it is not prossible
        break;
      } else if (EAGAIN == errno || EWOULDBLOCK == errno) {
        has_EAGAIN_ = true;
        break;
      } else if (EINTR == errno) {
        // pass
      } else {
        ret = OB_IO_ERROR;
        LOG_WARN("read fd has error", K_(fd), K(errno));
      }
    }
    return ret;
  }
  void* alloc_io_buffer(int64_t sz) { return direct_alloc(sz); }
  void free_io_buffer(void* p) { direct_free(p); }
  static void* direct_alloc(int64_t sz) { return ob_malloc(sz, ObModIds::OB_COMMON_NETWORK); }
  static void direct_free(void* p) { ob_free(p); }
private:
  int fd_;
  bool has_EAGAIN_;
  bool request_more_data_;
  char* alloc_buf_;
  char* buf_end_;
  char* cur_buf_;
  char* data_end_;
  uint64_t consume_sz_;
};

class ObSqlNioImpl;
class PendingWriteTask
{
public:
  PendingWriteTask(): buf_(NULL), sz_(0) {}
  ~PendingWriteTask() {}
  void reset() {
    buf_ = NULL;
    sz_ = 0;
  }
  void init(const char* buf, int64_t sz) {
    buf_ = buf;
    sz_ = sz;
  }
  int try_write(int fd, bool& become_clean) {
    int ret = OB_SUCCESS;
    int64_t wbytes = 0;
    if (NULL == buf_) {
      // no pending task
    } else if (OB_FAIL(do_write(fd, buf_, sz_, wbytes))) {
      LOG_WARN("do_write fail", K(ret));
    } else if (wbytes >= sz_) {
      become_clean = true;
      reset();
    } else {
      buf_ += wbytes;
      sz_ -= wbytes;
    }
    return ret;
  }
private:
  int do_write(int fd, const char* buf, int64_t sz, int64_t& consume_bytes) {
    int ret = OB_SUCCESS;
    int64_t pos = 0;
    while(pos < sz && OB_SUCCESS == ret) {
      int64_t wbytes = 0;
      if ((wbytes = write(fd, buf + pos, sz - pos)) >= 0) {
        pos += wbytes;
      } else if (EAGAIN == errno || EWOULDBLOCK == errno) {
        LOG_INFO("write return EAGAIN");
      } else if (EINTR == errno) {
        // pass
      } else {
        ret = OB_IO_ERROR;
        LOG_WARN("write data error", K(errno));
      }
    }
    if (OB_SUCCESS == ret) {
      consume_bytes = pos;
    }
    return ret;
  }
private:
  const char* buf_;
  int64_t sz_;
};

class ObSqlSock: public ObLink
{
public:
  ObSqlSock(ObSqlNioImpl& nio, int fd): nio_impl_(nio), fd_(fd), err_(0), read_buffer_(fd), 
            need_epoll_trigger_write_(false), may_handling_(true), handler_close_flag_(false),
            need_shutdown_(false), last_decode_time_(0), last_write_time_(0), sql_session_info_(NULL) {
    memset(sess_, 0, sizeof(sess_));
  }
  ~ObSqlSock() {}
  int64_t get_remain_sz() const { return read_buffer_.get_remain_sz(); }
  TO_STRING_KV(KP(this), K_(fd), K_(err), K(last_decode_time_), K(last_write_time_),
              K(read_buffer_.get_consume_sz()), K(get_pending_flag()), KPC(get_trace_id()));
  ObSqlNioImpl& get_nio_impl() { return nio_impl_; }
  bool set_error(int err) { return 0 == ATOMIC_TAS(&err_, err); }
  bool has_error() const { return ATOMIC_LOAD(&err_) != 0; }

  void do_close() {
    if (fd_ >= 0) {
      close(fd_);
      read_buffer_.set_fd(-1);
      fd_ = -1;
    }
  }
  void set_last_decode_succ_time(int64_t time) { last_decode_time_ = time;  }  
  int64_t get_consume_sz() { return read_buffer_.get_consume_sz(); }

  int peek_data(int64_t limit, const char*& buf, int64_t& sz) {
    return  read_buffer_.peek_data(limit ,buf, sz);
  }
  int consume_data(int64_t sz) { return read_buffer_.consume_data(sz); }
  void init_write_task(const char* buf, int64_t sz) {
    pending_write_task_.init(buf, sz);
  }

  bool is_need_epoll_trigger_write() const { return need_epoll_trigger_write_; }
  int do_pending_write(bool& become_clean) {
    int ret = OB_SUCCESS;
    if (OB_FAIL(pending_write_task_.try_write(fd_, become_clean))) {
      need_epoll_trigger_write_ = false;
      LOG_WARN("pending write task write fail", K(ret));
    } else if (become_clean) {
      last_write_time_ = ObTimeUtility::current_time();
      need_epoll_trigger_write_ = false;
      LOG_DEBUG("pending write clean", K(this));
    } else {
      LOG_INFO("need epoll trigger write", K(this));
      need_epoll_trigger_write_ = true;
    }
    return ret;
  }
  int write_data(const char* buf, int64_t sz) {
    int ret = OB_SUCCESS;
    int64_t pos = 0;
    while(pos < sz && OB_SUCCESS == ret) {
      int64_t wbytes = 0;
      if ((wbytes = write(fd_, buf + pos, sz - pos)) >= 0) {
        pos += wbytes;
        LOG_DEBUG("write fd", K(wbytes));
      } else if (EAGAIN == errno || EWOULDBLOCK == errno) {
        write_cond_.wait(1000 * 1000);
        LOG_INFO("write cond wakeup");
      } else if (EINTR == errno) {
        // pass
      } else {
        ret = OB_IO_ERROR;
        LOG_WARN("write data error", K(errno));
      }
    }
    last_write_time_ = ObTimeUtility::current_time();
    return ret;
  }
  const rpc::TraceId* get_trace_id() const {
    ObSqlSockSession* sess = (ObSqlSockSession *)sess_;
    return &(sess->sql_req_.get_trace_id());
  }
  bool wait_handling() { return ready_flag_.set_ready(); }
  int32_t get_pending_flag() const { return ready_flag_.get_pending_flag(); }
  void set_writable() { write_cond_.signal(); }
  bool set_readable() { return ready_flag_.set_ready(); }
  bool end_handle() { return ready_flag_.end_handle(!read_buffer_.clear_EAGAIN()); }
  int get_fd() { return fd_; }
  void disable_may_handling_flag() { ATOMIC_STORE(&may_handling_, false); }
  bool get_may_handling_flag() const { return ATOMIC_LOAD(&may_handling_); }
  void set_last_decode_time() { last_decode_time_ = ObTimeUtility::current_time(); }
  int64_t get_last_decode_time() const  { return last_decode_time_; }
  int64_t get_last_write_time() const { return last_write_time_; }
  int on_disconnect() {
    ObSqlSockSession* sess = (ObSqlSockSession *)sess_;
    return sess->on_disconnect();
  }
  void set_sql_session_info(void* sess) { sql_session_info_ = sess; }
  void reset_sql_session_info() { ATOMIC_STORE(&sql_session_info_, NULL); }
  bool sql_session_info_is_null() { return NULL == ATOMIC_LOAD(&sql_session_info_); }
  bool handler_close_been_called() const { return handler_close_flag_; }
  void set_handler_close_been_called() { handler_close_flag_ = true; }
  void remove_fd_from_epoll(int epfd) {
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd_, nullptr) < 0) {
      LOG_WARN("remove sock fd from epoll failed", K(fd_), K(epfd));
    }
  }
  void set_shutdown() { ATOMIC_STORE(&need_shutdown_, true); }
  bool need_shutdown() const { return ATOMIC_LOAD(&need_shutdown_); }
  void shutdown() { ::shutdown(fd_, SHUT_RD); }
public:
  ObDLink dlink_;
  ObDLink all_list_link_;
  ObLink write_task_link_;
private:
  ObSqlNioImpl& nio_impl_;
  int fd_;
  int err_;
  ReadBuffer read_buffer_;
  ReadyFlag ready_flag_;
  SingleWaitCond write_cond_;
  PendingWriteTask pending_write_task_;
  bool need_epoll_trigger_write_;
  bool may_handling_;
  bool handler_close_flag_;
  bool need_shutdown_;
  int64_t last_decode_time_;
  int64_t last_write_time_;
  void* sql_session_info_;
public:
  char sess_[3000] __attribute__((aligned(16)));
};

static struct epoll_event *__make_epoll_event(struct epoll_event *event, uint32_t event_flag, void* val) {
  event->events = event_flag;
  event->data.ptr = val;
  return event;
}

static int epoll_regist(int epfd, int fd, uint32_t eflag, void* s) {
  int err = 0;
  struct epoll_event event;
  if (0 != epoll_ctl(epfd, EPOLL_CTL_ADD, fd, __make_epoll_event(&event, eflag, s))) {
    err = -EIO;
    LOG_ERROR("add fd to epoll failed", K(fd), K(epfd), K(errno));
  }
  return err;
}

static int socket_set_opt(int fd, int option, int value)
{
  return setsockopt(fd, SOL_SOCKET, option, (void *)&value, sizeof(value));
}

static int listen_create(int port) {
  int err = 0;
  int fd = 0;
  struct sockaddr_in sin;
  if ((fd = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0)) < 0) {
    LOG_ERROR("sql nio create socket for listen failed", K(errno));
    err = errno;
  } else if (socket_set_opt(fd, SO_REUSEPORT, 1) < 0) {
    LOG_ERROR("sql nio set sock opt SO_REUSEPORT failed", K(errno), K(fd));
    err = errno;
  } else if (socket_set_opt(fd, SO_REUSEADDR, 1) < 0) {
    LOG_ERROR("sql nio set sock opt SO_REUSEADDR failed", K(errno), K(fd));
    err = errno;
  } else if (bind(fd, (sockaddr*)obrpc::make_unix_sockaddr(&sin, 0, port), sizeof(sin))) {
    LOG_ERROR("sql nio bind listen fd failed", K(errno), K(fd));
    err = errno;
  } else if (listen(fd, 1024) < 0) {
    LOG_ERROR("sql nio listen failed", K(errno), K(fd));
    err = errno;
  }
  if (0 != err) {
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
  }
  return fd;
}

static void evfd_read(int fd)
{
  uint64_t v = 0;
  while(read(fd, &v, sizeof(v)) < 0 && errno == EINTR)
    ;
}

static int evfd_write(int fd)
{
  uint64_t v = 1;
  int64_t bytes = 0;
  while((bytes = write(fd, &v, sizeof(v))) < 0 && errno == EINTR)
    ;
  return bytes == sizeof(v)? OB_SUCCESS: OB_IO_ERROR;
}

class Evfd
{
public:
  Evfd(): evfd_(-1), in_epoll_(0) {}
  ~Evfd() {
    if (evfd_ >= 0) {
      close(evfd_);
      evfd_ = -1;
    }
  }
  int create(int epfd) {
    int ret = OB_SUCCESS;
    uint32_t eflag = EPOLLIN | EPOLLERR | EPOLLET | EPOLLRDHUP;
    if ((evfd_ = eventfd(0, EFD_NONBLOCK)) < 0) {
      ret = OB_IO_ERROR;
      LOG_WARN("eventfd create fail", K(errno));
    } else if (0 != epoll_regist(epfd, evfd_, eflag, this)) {
      ret = OB_IO_ERROR;
    }
    return ret;
  }
  void begin_epoll() { ATOMIC_STORE(&in_epoll_, 1); }
  void end_epoll() { ATOMIC_STORE(&in_epoll_, 0); }
  void signal() {
    if (1 == ATOMIC_LOAD(&in_epoll_)) {
      evfd_write(evfd_);
    }
  }
  void consume() { evfd_read(evfd_); }
private:
  int evfd_;
  int in_epoll_ CACHE_ALIGNED;
};
/*
  how a socket destroy:
  set_error() -> prepare_destroy() -> wait_handing() -> handler.on_close()

  set_error() triggered by worker or by epoll.
  prepare_destroy() add sock to close_pending_list.
  handler.on_close() free user allocated resource.
 */
class ObSqlNioImpl
{
public:
  ObSqlNioImpl(ObISqlSockHandler& handler): handler_(handler), epfd_(-1), lfd_(-1) {}
  ~ObSqlNioImpl() {}
  int init(int port) {
    int ret = OB_SUCCESS;
    uint32_t epflag = EPOLLIN;
    if ((epfd_ = epoll_create1(EPOLL_CLOEXEC)) < 0) {
      ret = OB_IO_ERROR;
      LOG_WARN("epoll_create fail", K(ret), K(errno));
    } else if ((lfd_ = listen_create(port)) < 0) {
      ret = OB_IO_ERROR;
      LOG_WARN("listen create fail", K(port), K(errno));
    } else if (0 != epoll_regist(epfd_, lfd_, epflag, NULL)) {
      ret = OB_IO_ERROR;
      LOG_WARN("regist listen fd fail", K(ret));
    } else if (OB_FAIL(evfd_.create(epfd_))) {
      LOG_WARN("evfd create fail", K(ret));
    } else {
      LOG_INFO("sql_nio listen succ", K(port));
    }
    return ret;
  }
  void do_work() {
    if (write_req_queue_.empty()) {
      evfd_.begin_epoll();
      if (write_req_queue_.empty()) {
        handle_epoll_event();
      }
      evfd_.end_epoll();
    }
    handle_write_req_queue();
    handle_close_req_queue();
    handle_pending_destroy_list();
    print_session_info();
  }
  void push_close_req(ObSqlSock* s) {
    if (s->set_error(EIO)) {
      LOG_WARN("close sql sock by user req", K(*s));
      close_req_queue_.push(s);
    } else {
      LOG_WARN("user req close, and epoll thread already set error", K(*s));
    }
  }
  void push_write_req(ObSqlSock* s) {
    write_req_queue_.push(&s->write_task_link_);
    evfd_.signal();
  }
  void revert_sock(ObSqlSock* s) {
    if (OB_UNLIKELY(s->has_error())) {
      LOG_TRACE("revert_sock: sock has error", K(*s));
      s->disable_may_handling_flag();
    } else if (OB_UNLIKELY(s->need_shutdown())) {
      LOG_TRACE("sock revert succ and push to close req queue", K(*s));
      push_close_req(s);
      s->disable_may_handling_flag();
    } else if (OB_UNLIKELY(!s->end_handle())) {
      LOG_TRACE("revert_sock: sock still readable", K(*s));
      int ret = OB_SUCCESS;
      if (OB_FAIL(handler_.on_readable(s->sess_))) {
        LOG_TRACE("push to omt queue fail", K(ret), K(*s));
        push_close_req(s);
        s->disable_may_handling_flag();
      }
    }
  }

private:
  void handle_epoll_event() {
    const int maxevents = 512;
    struct epoll_event events[maxevents];
    int cnt = epoll_wait(epfd_, events, maxevents, 1000);
    for(int i = 0; i < cnt; i++) {
      ObSqlSock* s = (ObSqlSock*)events[i].data.ptr;
      if (OB_UNLIKELY(NULL == s)) {
        do_accept_loop();
      } else if (OB_UNLIKELY((void*)&evfd_ == (void*)s)) {
        evfd_.consume();
      } else {
        handle_sock_event(s, events[i].events);
      }
    }
  }

  void handle_close_req_queue() {
    ObSqlSock* s = NULL;
    while((s = (ObSqlSock*)close_req_queue_.pop())) {
      prepare_destroy(s);
    }
  }
  void prepare_destroy(ObSqlSock* s) {
    LOG_WARN("prepare destroy", K(*s));
    s->remove_fd_from_epoll(epfd_);
    s->on_disconnect();
    pending_destroy_list_.add(&s->dlink_);
  }

  void handle_pending_destroy_list() {
    ObDLink* head = pending_destroy_list_.head();
    ObLink* cur = head->next_;
    while(cur != head) {
      ObSqlSock* s = CONTAINER_OF(cur, ObSqlSock, dlink_);
      cur = cur->next_;
      bool need_destroy = false;
      if (false == s->handler_close_been_called()) {
        if (false == s->get_may_handling_flag()) {
          LOG_WARN("can close safely, do destroy", K(*s));
          need_destroy = true;
        } else {
          if (s->wait_handling()) {
            LOG_WARN("sock ref clean, do destroy", K(*s));
            need_destroy = true;
          } else {
            LOG_WARN("wait handling done...", K(*s));
          }
        }
        if (need_destroy) {
          handler_.on_close(s->sess_, 0);
          s->set_handler_close_been_called();
        }
      } else {
        if (true == s->sql_session_info_is_null()) {
          pending_destroy_list_.del(&s->dlink_);
          remove_session_info(s);
          s->do_close();
          free_sql_sock(s);
        }
      }
    }
  }
  void handle_sock_event(ObSqlSock* s, uint32_t mask) {
    if (OB_UNLIKELY((EPOLLERR & mask) || (EPOLLHUP & mask) || (EPOLLRDHUP & mask))) {

      if (s->set_error(EIO)) {
        LOG_WARN("sock error detect by epoll", K(mask), K(*s));
        prepare_destroy(s);
      } else {
        LOG_WARN("sock error detect by epoll, and worker thread alread set error", K(*s));
      }
    } else {
      int err = 0;
      if (OB_UNLIKELY(0 == err && (EPOLLOUT & mask))) {
        s->set_writable();
        if (s->is_need_epoll_trigger_write()) {
          err = do_pending_write(s);
        }
      }
      if (OB_LIKELY(0 == err && (EPOLLIN & mask))) {
        if (OB_LIKELY(s->set_readable())) {
          err = handler_.on_readable(s->sess_);
        }
      }
      if (OB_UNLIKELY(0 != err)) {
        revert_sock(s);
        if (s->set_error(err)) {
          prepare_destroy(s);
        }
      }
    }
  }
  int do_pending_write(ObSqlSock* s) {
    int ret = OB_SUCCESS;
    bool become_clean = false;
    if (OB_FAIL(s->do_pending_write(become_clean))) {
    } else if (become_clean) {
      handler_.on_flushed(s->sess_);
    }
    return ret;
  }
  void handle_write_req_queue() {
    ObLink* p = NULL;
    while((p = (ObLink*)write_req_queue_.pop())) {
      ObSqlSock* s = CONTAINER_OF(p, ObSqlSock, write_task_link_);
      if (s->has_error()) {
        revert_sock(s);
      } else if (0 != do_pending_write(s)) {
        revert_sock(s);
        if (s->set_error(EIO)) {
          prepare_destroy(s);
        }
      }
    }
  }

  void do_accept_loop() {
    while(1){
      int fd = -1;
      if ((fd = accept4(lfd_, NULL, NULL, SOCK_NONBLOCK|SOCK_CLOEXEC)) < 0) {
        if (EAGAIN == errno || EWOULDBLOCK == errno) {
          break;
        } else {
          LOG_ERROR("accept4 fail", K(lfd_), K(errno));
          break;
        }
      } else {
        int err = 0;
        if (0 != (err = do_accept_one(fd))) {
          LOG_ERROR("do_accept_one fail", K(fd), K(err));
          close(fd);
        }
      }
    }
  }
  int do_accept_one(int fd) {
    int err = 0;
    ObSqlSock* s = NULL;
    uint32_t epflag = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLET | EPOLLRDHUP;
    if (NULL == (s = alloc_sql_sock(fd))) {
      err = -ENOMEM;
      LOG_WARN("alloc_sql_sock fail", K(fd), K(err));
    } else if (0 != (err = epoll_regist(epfd_, fd, epflag, s))) {
      LOG_WARN("epoll_regist fail", K(fd), K(err));
    } else if (0 != (err = handler_.on_connect(s->sess_, fd))) {
      LOG_WARN("on_connect fail", K(err));
    } else {
      LOG_INFO("accept one succ", K(*s));
    }
    if (0 != err && NULL != s) {
      ObSqlSockSession* sess = (ObSqlSockSession *)s->sess_;
      sess->destroy_sock();
    }
    return err;
  }
private:
  ObSqlSock* alloc_sql_sock(int fd) {
    ObSqlSock* s = NULL;
    if (NULL != (s = (ObSqlSock*)direct_alloc(sizeof(*s)))) {
      new(s)ObSqlSock(*this, fd);
      record_session_info(s);
    }
    return s;
  }
  void free_sql_sock(ObSqlSock* s) {
    if (NULL != s) {
      s->~ObSqlSock();
      direct_free(s);
    }
  }
  void record_session_info(ObSqlSock *& s) {
    all_list_.add(&s->all_list_link_);
  }
  void remove_session_info(ObSqlSock *& s) {
    all_list_.del(&s->all_list_link_);
  }

  void print_session_info() {
    static const int64_t max_process_time = 1000L * 1000L * 20L; // 20s
    if (TC_REACH_TIME_INTERVAL(15*1000*1000L)) {
      ObDLink* head = all_list_.head();
      ObLink* cur = head->next_;
      while (cur != head) {
        ObSqlSock* s = CONTAINER_OF(cur, ObSqlSock, all_list_link_);
        cur = cur->next_;
        if (s->get_pending_flag()) {
          int64_t time_interval = ObTimeUtility::current_time() - s->get_last_decode_time();
          if (time_interval > max_process_time) {
            LOG_INFO("[sql nio session]", K(*s));
          }
        }
      }
    }
  }
  static void* direct_alloc(int64_t sz) { return common::ob_malloc(sz, common::ObModIds::OB_COMMON_NETWORK); }
  static void direct_free(void* p) { common::ob_free(p); }

private:
  ObISqlSockHandler& handler_;
  int epfd_;
  int lfd_;
  Evfd evfd_;
  ObSpScLinkQueue close_req_queue_;
  ObSpScLinkQueue write_req_queue_;
  ObDList pending_destroy_list_;
  ObDList all_list_;
};

int ObSqlNio::start(int port, ObISqlSockHandler* handler, int n_thread)
{
  int ret = OB_SUCCESS;
  if (NULL == (impl_ = (typeof(impl_))ob_malloc(sizeof(ObSqlNioImpl) * n_thread, "SqlNio"))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("alloc sql nio fail", K(ret));
  } else {
    for(int i = 0; OB_SUCCESS == ret && i < n_thread; i++) {
      new(impl_ + i)ObSqlNioImpl(*handler);
      if (OB_FAIL(impl_[i].init(port))) {
        LOG_WARN("impl init fail", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      set_thread_count(n_thread);
      lib::Threads::start();
    }
  }
  return ret;
}

void ObSqlNio::stop()
{
  lib::Threads::stop();
}

void ObSqlNio::wait()
{
  lib::Threads::wait();
}

void ObSqlNio::destroy()
{
}

void ObSqlNio::run(int64_t idx)
{
  int ret = OB_SUCCESS;
  if (NULL != impl_) {
    lib::set_thread_name("sql_nio", idx);
    while(!has_set_stop()) {
      impl_[idx].do_work();
    }
  }
}

static ObSqlSock* sess2sock(void* sess)
{
  return CONTAINER_OF(sess, ObSqlSock, sess_);
}

void ObSqlNio::destroy_sock(void* sess)
{
  ObSqlSock* sock = sess2sock(sess);
  sock->get_nio_impl().push_close_req(sock);
}

void ObSqlNio::revert_sock(void* sess)
{
  ObSqlSock* sock = sess2sock(sess);
  sock->get_nio_impl().revert_sock(sock);
}

void ObSqlNio::set_shutdown(void* sess)
{
  ObSqlSock* sock = sess2sock(sess);
  sock->set_shutdown();
}

void ObSqlNio::shutdown(void* sess)
{
  ObSqlSock* sock = sess2sock(sess);
  sock->shutdown();
}

void ObSqlNio::set_last_decode_succ_time(void* sess, int64_t time)
{
  sess2sock(sess)->set_last_decode_succ_time(time);
}

void ObSqlNio::reset_sql_session_info(void* sess)
{
  ObSqlSock* sock = sess2sock(sess);
  sock->reset_sql_session_info();
}

void ObSqlNio::set_sql_session_info(void* sess, void* sql_session)
{
  ObSqlSock* sock = sess2sock(sess);  
  sock->set_sql_session_info(sql_session);
}

bool ObSqlNio::has_error(void* sess)
{
  return sess2sock(sess)->has_error();
}

int ObSqlNio::peek_data(void* sess, int64_t limit, const char*& buf, int64_t& sz)
{
  return sess2sock(sess)->peek_data(limit, buf, sz);
}

int ObSqlNio::consume_data(void* sess, int64_t sz)
{
  return sess2sock(sess)->consume_data(sz);
}

int ObSqlNio::write_data(void* sess, const char* buf, int64_t sz)
{
  return sess2sock(sess)->write_data(buf, sz);
}

void ObSqlNio::async_write_data(void* sess, const char* buf, int64_t sz)
{
  ObSqlSock* sock = sess2sock(sess);
  sock->init_write_task(buf, sz);
  sock->get_nio_impl().push_write_req(sock);
}

}; // end namespace obmysql
}; // end namespace oceanbase
