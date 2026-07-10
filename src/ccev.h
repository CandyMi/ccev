/**
 * @file ccev.h
 * @brief ccev — A lightweight, cross-platform reactor event-driven library.
 *
 * @author CandyMi
 * @license MIT
 *
 * ccev wraps epoll (Linux), kqueue (macOS/BSD), IOCP (Windows), and select
 * (other POSIX) behind a single, high-level C API. It provides:
 *   - A single-threaded event loop with automatic ONESHOT re-arm
 *   - Two-level socket abstraction:
 *       · ccev_sock_t    — raw fd + event callbacks (reactor primitive)
 *       · ccev_stream_t  — buffered I/O + stream reader (protocol primitive)
 *   - TCP listen / TCP+UDS connect with integrated async DNS resolution
 *   - Timer management backed by a D-ary heap (4-ary, O(log n))
 *   - Automatic write buffering with iovec scatter/gather
 *   - kernel sendfile (zero-copy on Linux/macOS/FreeBSD)
 *   - ICMP echo (ping) with privilege-free path on modern kernels
 *   - Stream reader: read-until-delimiter / read-N-bytes with timeout
 *   - Replaceable memory allocator hooks
 *
 * License: MIT
 * Repository: https://github.com/CandyMi/ccev
 */

#ifndef CCEV_H
#define CCEV_H

#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uint64_t */
#include <stdbool.h>  /* bool */

#ifndef CCSOCKET_SOCK_T
  #define CCSOCKET_SOCK_T
  #if _WIN32
    typedef intptr_t ccsocket_t;
  #else
    typedef int ccsocket_t;
  #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════
 *  Thread-safety model
 * ════════════════════════════════════════════════════════════════
 *
 * ccev is designed as a **single-threaded reactor**.  One thread owns
 * the loop and calls ccev_loop_run(); all I/O callbacks and timer
 * callbacks fire on that thread.  The following rules apply:
 *
 * Thread-safe (may be called from any thread):
 *   - ccev_loop_stop()     — writes to an atomic flag + wakes the loop
 *   - ccev_wakeup()        — writes one byte to the wakeup pipe
 *   - ccev_set_allocator() — must be called before ccev_loop_create()
 *
 * Loop-thread only (may be called from callbacks or from the thread
 * that called ccev_loop_run()):
 *   - All ccev_sock_* functions
 *   - All ccev_stream_* functions
 *   - All ccev_timer_* functions
 *   - ccev_connect(), ccev_listen()
 *   - ccev_dns_* functions
 *   - ccev_icmp_echo()
 *   - ccev_signal_handle(), ccev_signal_ignore()
 *
 * Not thread-safe (must NOT be called concurrently with any other
 * function on the same loop):
 *   - ccev_loop_create() / ccev_loop_destroy()
 *   - ccev_loop_run()
 *   - ccev_default_loop()
 *
 * ════════════════════════════════════════════════════════════════
 *  Constants
 * ════════════════════════════════════════════════════════════════ */

/** @brief Maximum host string length (covers IPv4, IPv6, UDS path, FQDN). */
#define CCEV_DOMAIN_MAXLEN 256

/* ════════════════════════════════════════════════════════════════
 *  Opaque handle types
 * ════════════════════════════════════════════════════════════════ */

/** Opaque event-loop instance. Created by ccev_loop_create(). */
typedef struct ccev_loop_s    ccev_loop_t;

/** Low-level socket handle — fd + event callbacks.
 *  Created by ccev_sock_create(), ccev_listen(), ccev_connect(),
 *  or delivered via ccev_listen_cb. */
typedef struct ccev_sock_s    ccev_sock_t;

/** High-level stream handle — embeds ccev_sock_t, adds write
 *  buffering and stream reader.  Created by ccev_stream_open(). */
typedef struct ccev_stream_s  ccev_stream_t;

/** Opaque timer handle. Created by ccev_timer_add(). */
typedef struct ccev_timer_s   ccev_timer_t;

/* ════════════════════════════════════════════════════════════════
 *  Event flags (bitmask, passed to ccev_event_cb)
 * ════════════════════════════════════════════════════════════════ */

enum {
    CCEV_EVENT_NONE  = 0,        /**< No event (initial state).             */
    CCEV_EVENT_READ  = 1 << 0,   /**< Data available to read.               */
    CCEV_EVENT_WRITE = 1 << 1,   /**< Socket ready for writing.             */
    CCEV_EVENT_HUP   = 1 << 2,   /**< Peer closed / error (fast-path).
                                  *   select backend may report this as
                                  *   READ + recv()==0 instead.             */
};

/* ════════════════════════════════════════════════════════════════
 *  Callback type definitions
 * ════════════════════════════════════════════════════════════════ */

/** @brief Low-level event callback. Fired when socket events occur.
 *  @param sock   The socket that fired the event.
 *  @param events Bitmask of CCEV_EVENT_* flags. */
typedef void (*ccev_event_cb)(ccev_sock_t *sock, int events);

/** @brief Write-complete callback. Fired when the internal write buffer
 *  has been fully flushed to the kernel.
 *  @param udata  User-provided context pointer. */
typedef void (*ccev_send_cb)(void *udata);

/** @brief Socket-closed / error callback. Fired when the peer closes
 *  the connection or an I/O error occurs.  The fd is already dead —
 *  the user should release associated resources inside this callback.
 *  @param udata  User-provided context pointer. */
typedef void (*ccev_close_cb)(void *udata);

/** @brief Accept callback. Fired when a new TCP connection is accepted.
 *  The returned @p client is already registered with the loop.
 *  The user may call ccev_sock_read_start() or ccev_stream_open()
 *  on it inside this callback.
 *  @param udata  User-provided context pointer.
 *  @param client The newly accepted client socket.
 *  @param ip     Human-readable peer IP address (valid during callback).
 *  @param port   Peer port number. */
typedef void (*ccev_listen_cb)(void *udata, ccev_sock_t *client,
                                const char *ip, int port);

/** @brief Connect completion callback.
 *  @param udata  User-provided context pointer.
 *  @param sock   The connected socket (same pointer returned by
 *                ccev_connect()), or NULL on immediate failure.
 *  @param status CCEV_OK on success, CCEV_ERR on failure (timeout,
 *                DNS error, connection refused, etc.).  On failure
 *                the socket is already scheduled for deferred close. */
typedef void (*ccev_connect_cb)(void *udata, ccev_sock_t *sock, int status);

/** @brief Stream read completion callback.
 *  @param udata  User-provided context pointer.
 *  @param data   Full data segment (delimiter-inclusive for readline).
 *                Valid only during the callback — do NOT free or store.
 *  @param len    Length of @p data (0 on error/closed).
 *  @param status CCEV_OK on success, CCEV_ERR on max_len exceeded or
 *                connection closed. */
typedef void (*ccev_stream_cb)(void *udata, const char *data,
                                size_t len, int status);

/** @brief Timer expiry callback.
 *  @param udata  User-provided context pointer. */
typedef void (*ccev_timer_cb)(void *udata);

/** @brief DNS resolution callback.
 *  @param udata    User-provided context pointer.
 *  @param address  Resolved address string, or "" on failure.
 *                  Points to a stack buffer — valid only during callback.
 *  @param status   CCEV_OK if resolution succeeded, CCEV_ERR on error. */
typedef void (*ccev_dns_cb)(void *udata, const char *address, int status);

/** @brief ICMP echo result callback.
 *  @param udata  User-provided context pointer.
 *  @param result ICMP echo result, or NULL on timeout/error. */
typedef struct ccev_icmp_result_s ccev_icmp_result_t;
typedef void (*ccev_icmp_cb)(void *udata, const ccev_icmp_result_t *result);

/** @brief Signal handler callback.
 *  @param udata  User-provided context pointer.
 *  @param signum OS signal number. */
typedef void (*ccev_signal_cb)(void *udata, int signum);

/** @brief Per-iteration callback.  Fires once at the end of each
 *  event-loop iteration, after dispatch and closing queue processing.
 *  @param loop  The event-loop instance. */
/** @brief Per-iteration callback.  Fires once at the end of each
 *  event-loop iteration, after dispatch and closing queue processing.
 *  @param loop  The event-loop instance.
 *  @param args  User-defined pointer passed to ccev_each(). */
typedef void (*ccev_loop_each_cb)(ccev_loop_t *loop, void *args);

/* ════════════════════════════════════════════════════════════════
 *  Enumerations & flag constants
 * ════════════════════════════════════════════════════════════════ */

/** @brief Event-loop run modes. */
typedef enum {
    CCEV_RUN_ONCE,    /**< Process ready events once and return.         */
    CCEV_RUN_FOREVER, /**< Loop until ccev_loop_stop() is called.        */
} ccev_run_mode_t;

/** @brief Timer mode. */
typedef enum {
    CCEV_TIMER_ONCE,   /**< Fire once then auto-remove.                  */
    CCEV_TIMER_REPEAT, /**< Fire repeatedly at the given interval.       */
} ccev_timer_mode_t;

/** @brief DNS query record type. */
typedef enum {
    CCEV_DNS_A    = 1, /**< A record (IPv4).                             */
    CCEV_DNS_AAAA = 2, /**< AAAA record (IPv6).                          */
} ccev_dns_type_t;

/** @brief Socket option flags (OR-able bitmask).
 *  Used by ccev_listen(), ccev_connect(). */
typedef unsigned int ccev_flag_t;
enum {
    /** Enable SO_REUSEADDR — allow binding to a recently-used address. */
    CCEV_REUSEADDR    = 1u << 8,
    /** Enable SO_REUSEPORT — allow multiple processes to bind same port. */
    CCEV_REUSEPORT    = 1u << 9,
    /** Enable TCP_NODELAY — disable Nagle's algorithm. */
    CCEV_TCP_NODELAY  = 1u << 10,
    /** Enable TCP_DEFER_ACCEPT (Linux) / SO_ACCEPTFILTER (FreeBSD). */
    CCEV_ACCEPT_DEFER = 1u << 11,
};

/* ════════════════════════════════════════════════════════════════
 *  Return codes
 * ════════════════════════════════════════════════════════════════ */

#define CCEV_OK     0   /**< Operation completed successfully.           */
#define CCEV_ERR   -1   /**< Operation failed / fd is closed / error.   */

/* ════════════════════════════════════════════════════════════════
 *  Memory allocator (optional override)
 * ════════════════════════════════════════════════════════════════ */

/** @brief Replace the internal memory allocator.
 *
 *  Must be called before any ccev_loop_create() call. Not thread-safe.
 *
 *  @param realloc_fn  Pointer to a realloc-compatible function.
 *  @param free_fn     Pointer to a free-compatible function.
 */
void ccev_set_allocator(void *(*realloc_fn)(void*, size_t),
                        void  (*free_fn)(void*));

/* ════════════════════════════════════════════════════════════════
 *  Event-loop lifecycle
 * ════════════════════════════════════════════════════════════════ */

/** @brief Create a new event-loop instance.
 *  @param max_events  Max events epoll_wait() may return per iteration.
 *  @return Loop handle, or NULL on allocation failure. */
ccev_loop_t *ccev_loop_create(int max_events);

/** @brief Get the default event-loop singleton (not thread-safe).
 *  @return Default loop handle. */
ccev_loop_t *ccev_default_loop(void);

/** @brief Get the default event-loop singleton with custom max_events.
 *
 *  If the default loop already exists, returns the existing loop
 *  regardless of @p events — the parameter only takes effect on
 *  first call.
 *
 *  @param max_events  Max events per poll iteration (0 for default 128).
 *  @return Default loop handle, or NULL on allocation failure. */
ccev_loop_t *ccev_default_loop_ex(int max_events);

/** @brief Destroy an event-loop instance.
 *  @param loop  Loop handle (NULL-safe). */
void ccev_loop_destroy(ccev_loop_t *loop);

/** @brief Request the event loop to stop.
 *  Thread-safe. Wakes the loop via an internal pipe.
 *  @param loop  Loop handle. */
void ccev_loop_stop(ccev_loop_t *loop);

/** @brief Run the event loop.
 *  @param mode  CCEV_RUN_ONCE or CCEV_RUN_FOREVER.
 *  @return In ONCE mode: events processed. In FOREVER: does not return. */
int ccev_loop_run(ccev_loop_t *loop, ccev_run_mode_t mode);

/* ════════════════════════════════════════════════════════════════
 *  Low-level socket API (ccev_sock_t)
 * ════════════════════════════════════════════════════════════════ */

/** @brief Create a ccev_sock_t from an external file descriptor.
 *
 *  The caller is responsible for creating and configuring the fd.
 *  The returned sock has no callbacks bound and no events armed.
 *  Call ccev_sock_read_start() to arm read events.
 *
 *  @param loop  Event-loop handle.
 *  @param fd    File descriptor to monitor (ccsocket_t).
 *  @param udata User pointer (retrievable via ccev_sock_get_udata()).
 *  @return Socket handle, or NULL on failure. */
ccev_sock_t *ccev_sock_create(ccev_loop_t *loop, ccsocket_t fd, void *udata);

/** @brief Start listening for TCP connections (or Unix domain sockets).
 *
 *  Creates a socket, binds to @p host:@p port, and starts accepting.
 *  The returned sock can be closed with ccev_sock_close() to stop
 *  listening.
 *
 *  Unix domain sockets:
 *    - Use a filesystem path as @p addr ("/tmp/mysock").
 *    - An \@ prefix is accepted and stripped to form a relative
 *      path ("/tmp/mysock" and "@mysock" are equivalent UDS paths;
 *      \@ does NOT imply Linux abstract namespace).
 *    - @p port must be 0.
 *    - The socket file is automatically unlink()'d on close.
 *
 *  @param loop      Event-loop handle.
 *  @param addr      Address ("0.0.0.0", "::", "/tmp/sock", "@mysock", etc.).
 *  @param port      Port (0 for Unix domain sockets).
 *  @param backlog   listen(2) backlog size.
 *  @param flags     OR-ed ccev_flag_t.
 *  @param cb        Callback invoked for every new connection.
 *  @param udata     User pointer passed to @p cb.
 *  @return Listener socket, or NULL on failure. */
ccev_sock_t *ccev_listen(ccev_loop_t *loop, const char *addr, uint16_t port,
                           int backlog, ccev_flag_t flags,
                           ccev_listen_cb cb, void *udata);

/** @brief Initiate an asynchronous connection (TCP or UDS).
 *
 *  For hostnames the library resolves DNS internally, then begins
 *  non-blocking connect(2).  The total timeout spans both DNS and
 *  TCP connect phases.
 *
 *  The returned sock has fd == -1 until DNS+connect completes.
 *  Do NOT attempt I/O on it before the callback fires.
 *
 *  @param loop         Event-loop handle.
 *  @param host         Target address/domain (or UDS path).
 *  @param port         Target port (0 for Unix domain sockets).
 *  @param timeout_ms   Total timeout in ms (0 = no timeout).
 *  @param flags        OR-ed ccev_flag_t.
 *  @param cb           Callback on connect completion or failure.
 *  @param udata        User pointer passed to @p cb.
 *  @return Socket handle, or NULL on immediate failure (OOM). */
ccev_sock_t *ccev_connect(ccev_loop_t *loop, const char *host, uint16_t port,
                            unsigned int timeout_ms, ccev_flag_t flags,
                            ccev_connect_cb cb, void *udata);

/** @brief Arm read events and optionally set the read event callback.
 *
 *  When @p cb is non-NULL, it replaces the current read callback and
 *  registers CCEV_EVENT_READ with the reactor.  When @p cb is NULL,
 *  the read callback is unchanged but the event is still armed.
 *
 *  @param sock  Socket handle.
 *  @param cb    Read event callback, or NULL to keep existing.
 *  @return CCEV_OK or CCEV_ERR (closed/invalid). */
int ccev_sock_read_start(ccev_sock_t *sock, ccev_event_cb cb);

/** @brief Disarm read events. The read callback is preserved but
 *  will not fire until ccev_sock_read_start() is called again.
 *  @param sock  Socket handle.
 *  @return CCEV_OK or CCEV_ERR. */
int ccev_sock_read_stop(ccev_sock_t *sock);

/** @brief Arm write events and optionally set the write event callback.
 *
 *  Most users should use ccev_stream_write() instead — this low-level
 *  API is for custom protocol implementations.
 *
 *  @param sock  Socket handle.
 *  @param cb    Write event callback, or NULL to keep existing.
 *  @return CCEV_OK or CCEV_ERR. */
int ccev_sock_write_start(ccev_sock_t *sock, ccev_event_cb cb);

/** @brief Disarm write events.
 *  @param sock  Socket handle.
 *  @return CCEV_OK or CCEV_ERR. */
int ccev_sock_write_stop(ccev_sock_t *sock);

/** @brief Set the close/error callback.
 *  Fired when the peer closes the connection or an I/O error occurs.
 *  @param sock  Socket handle.
 *  @param cb    Close callback.
 *  @param udata User pointer for the callback. */
void ccev_sock_set_close_cb(ccev_sock_t *sock, ccev_close_cb cb, void *udata);

/** @brief Close and release a socket.
 *
 *  The socket is removed from epoll and scheduled for deferred close.
 *  The close_cb (if set) fires before memory is freed.
 *
 *  @param sock  Socket handle.
 *  @return CCEV_OK or CCEV_ERR. */
int ccev_sock_close(ccev_sock_t *sock);

/** @brief Get the underlying file descriptor (diagnostic use only). */
ccsocket_t ccev_sock_get_fd(const ccev_sock_t *sock);

/** @brief Get the user-data pointer. */
void  *ccev_sock_get_udata(const ccev_sock_t *sock);

/** @brief Set the user-data pointer. */
void   ccev_sock_set_udata(ccev_sock_t *sock, void *udata);

/** @brief Get the number of active sockets.
 *  @param loop  Event-loop handle.
 *  @return Socket count. */
int ccev_sock_count(const ccev_loop_t *loop);

/* ════════════════════════════════════════════════════════════════
 *  High-level stream API (ccev_stream_t)
 * ════════════════════════════════════════════════════════════════ */

/** @brief Upgrade a ccev_sock_t to a ccev_stream_t.
 *
 *  The returned stream address is the same as the passed sock address;
 *  all socket variants share a common allocation via the internal
 *  ccev_sock_any_t union.  After this call, use the returned stream
 *  pointer; the original sock pointer is still valid (same address).
 *
 *  The stream takes ownership of the sock's event callbacks internally,
 *  routing them through buffered I/O and the stream reader.
 *
 *  @param sock  A live socket (not in closing state).
 *  @return Stream handle, or NULL on failure (OOM / closed sock). */
ccev_stream_t *ccev_stream_open(ccev_sock_t *sock);

/** @brief Close a stream and release all resources.
 *
 *  Flushes any pending write data, closes the underlying socket,
 *  frees write buffers and the stream reader.  Safe to call on
 *  an already-closed stream (no-op).
 *
 *  @param st  Stream handle.
 *  @return CCEV_OK or CCEV_ERR. */
int ccev_stream_close(ccev_stream_t *st);

/* ── Write operations ── */

/** @brief Write data asynchronously.
 *
 *  Data is appended to the internal write buffer and sent
 *  non-blocking.  If the kernel buffer is full, the remainder
 *  is queued and EPOLLOUT is armed automatically.
 *
 *  Each write's callback fires once when that specific buffer chunk
 *  has been fully flushed to the kernel.  The callback is bound to
 *  the buffer entry, so multiple writes with distinct callbacks
 *  each trigger independently.
 *
 *  @param st    Stream handle.
 *  @param data  Data to write (may be freed after return).
 *  @param len   Data length.
 *  @param cb    Per-buffer write-complete callback, or NULL.
 *  @param udata User pointer for @p cb.
 *  @return @p len (always accepted) or CCEV_ERR (OOM/closed). */
int ccev_stream_write(ccev_stream_t *st, const void *data, size_t len,
                       ccev_send_cb cb, void *udata);

/** @brief Batch write with explicit flush control.
 *
 *  @param st    Stream handle.
 *  @param data  Data to write (may be NULL when done=true for flush-only).
 *  @param len   Data length.
 *  @param done  false = buffer only (no send attempt).
 *               true  = buffer then trigger send.
 *  @param cb    Per-buffer write-complete callback, or NULL.
 *  @param udata User pointer for @p cb.
 *  @return Bytes accepted, or CCEV_ERR. */
int ccev_stream_write_batch(ccev_stream_t *st, const void *data, size_t len,
                             bool done, ccev_send_cb cb, void *udata);

/** @brief Flush the write buffer — send all queued data.
 *  Equivalent to ccev_stream_write_batch(st, NULL, 0, true, NULL, NULL).
 *  @param st  Stream handle.
 *  @return CCEV_OK or CCEV_ERR. */
int ccev_stream_flush(ccev_stream_t *st);

/** @brief Send a file using kernel sendfile (zero-copy).
 *  @param st    Stream handle.
 *  @param path  File path to send.
 *  @param cb    Completion callback, or NULL.
 *  @param udata User pointer for @p cb.
 *  @return CCEV_OK or CCEV_ERR. */
int ccev_stream_sendfile(ccev_stream_t *st, const char *path,
                          ccev_send_cb cb, void *udata);

/** @brief Set the global write-drain callback.
 *
 *  Fires once when the entire write buffer has been flushed to the
 *  kernel (wbuf_len reaches 0).  Unlike per-buffer callbacks, this
 *  is a single notification independent of how many write() calls
 *  were batched.  Useful for flow control.
 *
 *  @param st    Stream handle.
 *  @param cb    Callback (NULL to clear).
 *  @param udata User pointer. */
void ccev_stream_set_send_cb(ccev_stream_t *st, ccev_send_cb cb, void *udata);

/** @brief Get the number of bytes pending in the write buffer.
 *  @param st  Stream handle.
 *  @return Pending bytes. */
size_t ccev_stream_wbuf_len(const ccev_stream_t *st);

/* ── Read operations ── */

/** @brief Read until @p delim is found (delimiter inclusive).
 *
 *  When the delimiter (or @p maxlen bytes) is received, the callback
 *  fires exactly once.  Call again for subsequent reads.
 *
 *  Only one stream reader may be active at a time — calling this
 *  while another reader is active cancels the previous one.
 *
 *  @param st         Stream handle.
 *  @param delim      Delimiter byte to search for (e.g. '\\n').
 *  @param maxlen     Maximum bytes before yielding CCEV_ERR.
 *  @param timeout_ms Read timeout in ms (0 = no timeout).
 *  @param cb         Completion callback.
 *  @param udata      User pointer for @p cb.
 *  @return CCEV_OK or CCEV_ERR on invalid params. */
int ccev_stream_readline(ccev_stream_t *st, char delim, size_t maxlen,
                          int timeout_ms, ccev_stream_cb cb, void *udata);

/** @brief Read exactly @p n bytes.
 *
 *  Once @p n bytes have been accumulated the callback fires once.
 *  Same single-reader semantics as ccev_stream_readline().
 *
 *  @param st         Stream handle.
 *  @param n          Exact number of bytes to read (> 0).
 *  @param timeout_ms Read timeout in ms (0 = no timeout).
 *  @param cb         Completion callback.
 *  @param udata      User pointer for @p cb.
 *  @return CCEV_OK or CCEV_ERR. */
int ccev_stream_readnum(ccev_stream_t *st, size_t n,
                          int timeout_ms, ccev_stream_cb cb, void *udata);

/** @brief Read continuously — dispatch accumulated data as it arrives.
 *
 *  Unlike ccev_stream_readline/readnum (one-shot), this mode keeps the
 *  reader active after each dispatch.  The callback fires every time
 *  new data is available, until ccev_stream_read_stop() is called,
 *  the timeout fires, or the connection closes.
 *
 *  Only one stream reader may be active at a time — calling this
 *  while another reader is active cancels the previous one.
 *
 *  @param st         Stream handle.
 *  @param timeout_ms Read idle timeout in ms (0 = no timeout).
 *  @param cb         Dispatch callback.
 *  @param udata      User pointer for @p cb.
 *  @return CCEV_OK or CCEV_ERR. */
int ccev_stream_read(ccev_stream_t *st, int timeout_ms,
                      ccev_stream_cb cb, void *udata);

/** @brief Cancel the active stream reader (if any).
 *  Restores the underlying sock's read callback.
 *  Safe to call when no reader is active.
 *  @param st  Stream handle. */
void ccev_stream_read_stop(ccev_stream_t *st);

/** @brief Set the close callback on a stream.
 *  Delegates to ccev_sock_set_close_cb() internally.
 *  @param st     Stream handle.
 *  @param cb     Close callback.
 *  @param udata  User pointer. */
void ccev_stream_set_close_cb(ccev_stream_t *st, ccev_close_cb cb, void *udata);

/* ════════════════════════════════════════════════════════════════
 *  Timer subsystem
 * ════════════════════════════════════════════════════════════════ */

/** @brief Add a timer.
 *
 *  Internal D-ary heap (4-ary, O(log n)).  REPEAT timers are
 *  automatically re-inserted after firing.
 *
 *  @param loop      Event-loop handle.
 *  @param delay_ms  Delay in milliseconds.
 *  @param mode      CCEV_TIMER_ONCE or CCEV_TIMER_REPEAT.
 *  @param cb        Timer expiry callback.
 *  @param udata     User pointer.
 *  @return Timer handle, or NULL on failure. */
ccev_timer_t *ccev_timer_add(ccev_loop_t *loop, uint64_t delay_ms,
                               ccev_timer_mode_t mode,
                               ccev_timer_cb cb, void *udata);

/** @brief Delete a timer (lazy — marks inactive, freed when reached). */
int ccev_timer_del(ccev_loop_t *loop, ccev_timer_t *timer);

/** @brief Reset a timer to a new delay from now. */
int ccev_timer_reset(ccev_loop_t *loop, ccev_timer_t *timer,
                       uint64_t delay_ms);

/** @brief Get the number of active timers. */
int ccev_timer_count(ccev_loop_t *loop);

/* ════════════════════════════════════════════════════════════════
 *  Asynchronous DNS resolver
 * ════════════════════════════════════════════════════════════════ */

/** @brief Set the DNS server address.
 *  Default: "1.1.1.1", port 53. */
int ccev_dns_set_server(ccev_loop_t *loop, const char *address, uint16_t port);

/** @brief Resolve a domain asynchronously.
 *
 *  If @p domain is already an IP or UDS path, the callback fires
 *  immediately.  Otherwise sends UDP DNS queries to the configured
 *  server.
 *
 *  @param loop        Event-loop handle.
 *  @param domain      Domain name, IP, or UDS path.
 *  @param timeout_ms  Per-query timeout in ms (0 = no timeout).
 *  @param type        CCEV_DNS_A, CCEV_DNS_AAAA, or bitwise OR.
 *  @param cb          Completion callback.
 *  @param udata       User pointer.
 *  @return CCEV_OK or CCEV_ERR. */
int ccev_dns_resolve(ccev_loop_t *loop, const char *domain,
                      unsigned int timeout_ms, ccev_dns_type_t type,
                      ccev_dns_cb cb, void *udata);

/** @brief Flush the DNS cache and reload from OS hosts file. */
void ccev_dns_flush(ccev_loop_t *loop);

/* ════════════════════════════════════════════════════════════════
 *  Utilities
 * ════════════════════════════════════════════════════════════════ */

/** @brief Wake up the event loop from another thread. */
int ccev_wakeup(ccev_loop_t *loop);

/* ════════════════════════════════════════════════════════════════
 *  ICMP echo (ping)
 * ════════════════════════════════════════════════════════════════ */

/** ICMP echo result. */
struct ccev_icmp_result_s {
    char     ip[64];         /**< Target IP string. */
    double   rtt_ms;         /**< Round-trip time in milliseconds. */
    size_t   payload_len;    /**< Echo payload length. */
    int      ttl;            /**< Time-to-live from ICMP reply header. */
};

/** @brief Send an ICMP echo request (ping).
 *
 *  Hostnames are resolved via the internal async DNS resolver.
 *  Requires CAP_NET_RAW / root on most systems; some kernels
 *  support a privilege-free SOCK_DGRAM+ICMP path.
 *
 *  @param loop        Event-loop handle.
 *  @param host        Target IP or hostname.
 *  @param timeout_ms  Timeout in ms (0 = no timeout).
 *  @param cb          Result callback (NULL on timeout/error).
 *  @param udata       User pointer.
 *  @return CCEV_OK or CCEV_ERR. */
int ccev_icmp_echo(ccev_loop_t *loop, const char *host,
                    unsigned int timeout_ms,
                    ccev_icmp_cb cb, void *udata);

/* ════════════════════════════════════════════════════════════════
 *  Signal handling (default loop only)
 * ════════════════════════════════════════════════════════════════ */

/** @brief Register a signal handler on the default loop.
 *  @param signum  OS signal number.
 *  @param cb      Handler callback.
 *  @param udata   User pointer.
 *  @return CCEV_OK or CCEV_ERR. */
int ccev_signal_handle(int signum, ccev_signal_cb cb, void *udata);

/** @brief Restore a signal to its default disposition. */
int ccev_signal_ignore(int signum);

/** @brief Register a per-iteration callback.
 *
 *  The callback fires at the start of every loop iteration,
 *  before timer processing, I/O dispatch, and the closing queue.
 *  Useful for metrics collection, co-operative background work,
 *  or integrating with external polling mechanisms.
 *
 *  Because the callback runs before epoll_wait(), it will not fire
 *  again until the current iteration completes.  The loop blocks
 *  normally when idle — no periodic polling is introduced.
 *
 *  Only one callback may be registered at a time — calling this
 *  again replaces the previous one.  Pass NULL to clear.
 *
 *  @param loop  Event-loop handle.
 *  @param cb    Callback, or NULL to clear.
 *  @param args  User-defined pointer forwarded to cb on each iteration.
 *  @return CCEV_OK or CCEV_ERR (NULL loop). */
int ccev_each(ccev_loop_t *loop, ccev_loop_each_cb cb, void *args);

#ifdef __cplusplus
}
#endif

#endif /* CCEV_H */
