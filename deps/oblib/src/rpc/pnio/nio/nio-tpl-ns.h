
#ifndef __ns__
#define __ns__
#define my_flush_cb tns(_flush_cb)
#define my_sk_do_decode tns(_sk_do_decode)
#define my_req_t tns(_req_t)
#define my_flush_cb_exception tns(_flush_cb_exception)
#define my_sk_handle_event_ready tns(_sk_handle_event_ready)
#define my_sk_t tns(_sk_t)
#define my_sk_handle_msg tns(_sk_handle_msg)
#define my_flush_cb_after_flush tns(_flush_cb_after_flush)
#define my_flush_cb_on_post_fail tns(_flush_cb_on_post_fail)
#define my_decode tns(_decode)
#define my_sk_read tns(_sk_read)
#define my_msg_t tns(_msg_t)
#define my_t tns(_t)
#define my_sk_do_flush tns(_sk_do_flush)
#define my_sk_flush tns(_sk_flush)
#define my_write_queue_on_sk_destroy tns(_write_queue_on_sk_destroy)
#define my_sk_consume tns(_sk_consume)
#else
#undef __ns__
#undef tns
#undef my_flush_cb
#undef my_sk_do_decode
#undef my_req_t
#undef my_flush_cb_exception
#undef my_sk_handle_event_ready
#undef my_sk_t
#undef my_sk_handle_msg
#undef my_flush_cb_after_flush
#undef my_flush_cb_on_post_fail
#undef my_decode
#undef my_sk_read
#undef my_msg_t
#undef my_t
#undef my_sk_do_flush
#undef my_sk_flush
#undef my_write_queue_on_sk_destroy
#undef my_sk_consume
#endif
