/**
 * @file ccev.h
 * @brief ccev — A lightweight, cross-platform reactor event-driven library.
 *
 * ccev wraps epoll (Linux), kqueue (macOS/BSD), IOCP (Windows), and select
 * (other POSIX) behind a single, high-level C API. It provides:
 *   - A single-threaded event loop with automatic ONESHOT re-arm
 *   - TCP listen / TCP+UDP connect with async DNS resolution
 *   - Timer management backed by a D-ary heap (4-ary, O(log n))
 *   - Automatic write buffering with iovec scatter/gather
 *   - kernel sendfile (zero-copy on Linux/macOS/FreeBSD)
 *   - ICMP echo (ping) with privilege-free path on modern kernels
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
#include "ccsocket.h" /* ccsocket_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════
 *  Opaque handle types
 * ════════════════════════════════════════════════════════════════ */

/** Opaque event-loop instance. Created by ccev_loop_create(). */
typedef struct ccev_loop_s  ccev_loop_t;

/** Opaque connection / I/O handle.
 *  Created by ccev_listen(), ccev_conn_create(), or delivered via
 *  ccev_accept_cb / ccev_connect_cb. */
typedef struct ccev_conn_s  ccev_conn_t;

/** Opaque timer handle. Created by ccev_timer_add(). */
typedef struct ccev_timer_s ccev_timer_t;

/** DNS resolution result — a singly-linked list of addresses. */
typedef struct ccev_address_s {
    char                    ip[64];   /**< Human-readable IP string.        */
    int                     ttl;      /**< DNS TTL in seconds.              */
    struct ccev_address_s  *next;     /**< Next address, NULL at end-of-list. */
} ccev_address_t;

/* ════════════════════════════════════════════════════════════════
 *  Callback type definitions
 * ════════════════════════════════════════════════════════════════ */

/** @brief Readable callback. Fired when new data is available on the fd.
 *  The user should call ccev_conn_recv() to consume data inside this callback.
 *  @param udata  User-provided context pointer. */
typedef void (*ccev_recv_cb)(void *udata);

/** @brief Write-complete callback. Fired when the internal write buffer has
 *  been fully flushed to the kernel.
 *  @param udata  User-provided context pointer. */
typedef void (*ccev_send_cb)(void *udata);

/** @brief Connection-closed / error callback. Fired when the peer closes
 *  the connection or an I/O error occurs. The fd is already dead by this
 *  point — the user must call ccev_conn_close() to release resources.
 *  @param udata  User-provided context pointer. */
typedef void (*ccev_close_cb)(void *udata);

/** @brief Timer expiry callback.
 *  @param udata  User-provided context pointer. */
typedef void (*ccev_timer_cb)(void *udata);

/** @brief Accept callback. Fired when a new TCP connection is accepted.
 *  The returned @p conn is already registered with the loop but has no
 *  callbacks bound yet. The user should set callbacks and optionally
 *  call ccev_conn_recv() to arm read events.
 *  @param udata  User-provided context pointer.
 *  @param conn   The newly accepted connection handle.
 *  @param ip     Human-readable peer IP address.
 *  @param port   Peer port number. */
typedef void (*ccev_accept_cb)(void *udata, ccev_conn_t *conn,
                                const char *ip, int port);

/** @brief Connect completion callback.
 *  @param udata  User-provided context pointer.
 *  @param conn   The connected connection handle, or NULL on failure.
 *  @param status CCEV_OK on success, CCEV_ERR on failure. */
typedef void (*ccev_connect_cb)(void *udata, ccev_conn_t *conn, int status);

/** @brief DNS resolution callback.
 *  @param udata  User-provided context pointer.
 *  @param addr   Linked list of resolved addresses. NULL if resolution failed.
 *                Must be freed with ccev_dns_free().
 *  @param status CCEV_OK if at least one address was resolved,
 *                CCEV_ERR on total failure / timeout. */
typedef void (*ccev_dns_cb)(void *udata, ccev_address_t *addr, int status);

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

/** @brief Flags for socket options (OR-able bitmask).
 *  Used by ccev_listen(), ccev_connect(). */
typedef unsigned int ccev_flag_t;
enum {
    /** Enable SO_REUSEADDR — allow binding to a recently-used address. */
    CCEV_REUSEADDR   = 1u << 8,
    /** Enable SO_REUSEPORT — allow multiple processes to bind the same port. */
    CCEV_REUSEPORT   = 1u << 9,
    /** Enable TCP_NODELAY — disable Nagle's algorithm. */
    CCEV_TCP_NODELAY = 1u << 10,
    /** Restrict a dual-stack socket to IPv6 only. */
    CCEV_IPV6_ONLY   = 1u << 11,
    /** Use UDP (datagram) instead of TCP (stream) for connect(). */
    CCEV_UDP         = 1u << 12,
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
 *  If never called, the library uses the standard libc realloc/free.
 *
 *  @param realloc_fn  Pointer to a realloc-compatible function (or NULL
 *                     to keep the default).
 *  @param free_fn     Pointer to a free-compatible function (or NULL).
 */
void ccev_set_allocator(void *(*realloc_fn)(void*, size_t),
                        void  (*free_fn)(void*));

/* ════════════════════════════════════════════════════════════════
 *  Event-loop lifecycle
 * ════════════════════════════════════════════════════════════════ */

/** @brief Create a new event-loop instance.
 *
 *  @param max_events  Maximum number of events epoll_wait() may return
 *                     per iteration. Typical values: 64–1024.
 *  @return Loop handle, or NULL on allocation failure.
 */
ccev_loop_t *ccev_loop_create(int max_events);

/** @brief Destroy an event-loop instance.
 *
 *  Closes all remaining connections, timers, and frees resources.
 *  Do NOT call from inside a callback that belongs to this loop.
 *  @param loop  Loop handle (NULL-safe).
 */
void ccev_loop_destroy(ccev_loop_t *loop);

/** @brief Request the event loop to stop.
 *
 *  Thread-safe. Wakes the loop from epoll_wait() via an internal pipe.
 *  The loop will exit after the current iteration completes.
 *  @param loop  Loop handle.
 */
void ccev_loop_stop(ccev_loop_t *loop);

/** @brief Run the event loop.
 *
 *  @param mode  CCEV_RUN_ONCE — process ready events and return.
 *               CCEV_RUN_FOREVER — loop until ccev_loop_stop().
 *  @return In ONCE mode: number of events processed (0 on timeout).
 *          In FOREVER mode: does not return (call ccev_loop_stop()
 *          from a callback or another thread to exit).
 */
int ccev_loop_run(ccev_loop_t *loop, ccev_run_mode_t mode);

/* ════════════════════════════════════════════════════════════════
 *  High-level TCP listen / TCP+UDP connect
 * ════════════════════════════════════════════════════════════════ */

/** @brief Start listening for TCP connections.
 *
 *  Creates a TCP socket, binds to @p host:@p port, and starts accepting.
 *  The returned handle can be closed with ccev_conn_close() to stop
 *  listening.
 *
 *  @param loop       Event-loop handle.
 *  @param host       Bind address ("0.0.0.0", "::", "127.0.0.1", etc.).
 *  @param port       Port string ("8080", "https", etc.).
 *  @param backlog    listen(2) backlog size.
 *  @param flags      OR-ed ccev_flag_t socket options.
 *  @param on_accept  Callback invoked for every new connection.
 *  @param udata      User pointer passed to on_accept.
 *  @return Connection handle (the listener itself), or NULL on failure.
 */
ccev_conn_t *ccev_listen(ccev_loop_t *loop, const char *host, const char *port,
                          int backlog, ccev_flag_t flags,
                          ccev_accept_cb on_accept, void *udata);

/** @brief Initiate an asynchronous connection (TCP or UDP).
 *
 *  For TCP: creates a non-blocking socket, begins connect(2), and
 *  returns immediately. The @p on_connect callback fires on completion.
 *
 *  For UDP (CCEV_UDP flag): creates a UDP socket and calls
 *  ccsocket_connect() to associate the remote address.
 *
 *  @param loop         Event-loop handle.
 *  @param host         Target hostname or IP.
 *  @param port         Target port.
 *  @param timeout_ms   Connection timeout in ms. 0 = no timeout.
 *  @param flags        OR-ed ccev_flag_t (CCEV_UDP for datagram).
 *  @param on_connect   Callback on connect finish or failure.
 *  @param udata        User pointer passed to on_connect.
 *  @return CCEV_OK on successful initiation, CCEV_ERR on error.
 */
int ccev_connect(ccev_loop_t *loop, const char *host, const char *port,
                 unsigned int timeout_ms, ccev_flag_t flags,
                 ccev_connect_cb on_connect, void *udata);

/* ════════════════════════════════════════════════════════════════
 *  External fd registration
 * ════════════════════════════════════════════════════════════════ */

/** @brief Register an external file descriptor with the reactor.
 *
 *  The caller is responsible for creating and configuring the fd.
 *  The returned handle has no callbacks bound and no events armed.
 *  Call ccev_conn_recv() with a non-NULL callback to arm read events.
 *
 *  @param loop  Event-loop handle.
 *  @param fd    The file descriptor to monitor (ccsocket_t).
 *  @param udata User pointer (retrievable via ccev_conn_get_udata()).
 *  @return Connection handle, or NULL on failure.
 */
ccev_conn_t *ccev_conn_create(ccev_loop_t *loop, ccsocket_t fd, void *udata);

/* ════════════════════════════════════════════════════════════════
 *  Connection I/O
 * ════════════════════════════════════════════════════════════════ */

/** @brief Read data from a connection.
 *
 *  Three behaviours depending on the arguments:
 *
 *  1. (buf!=NULL, len>0, cb!=NULL)
 *     Attempt a non-blocking recv() immediately. If data is available,
 *     it is read into @p buf and @p cb is called synchronously with
 *     the read data. Returns the number of bytes read. If recv() would
 *     block (EWOULDBLOCK), registers CCEV_READ (internal ONESHOT) and
 *     returns CCEV_OK — @p cb will fire later when data arrives.
 *
 *  2. (buf!=NULL, len>0, cb=NULL)
 *     Pure synchronous read. Returns bytes read, 0 on EOF, CCEV_ERR
 *     on error. Does NOT register any epoll event.
 *
 *  3. (buf=NULL, len=0, cb!=NULL)
 *     Register/update the recv callback and arm CCEV_READ. Does NOT
 *     attempt an immediate read. Returns CCEV_OK.
 *
 *  4. (buf=NULL, len=0, cb=NULL, udata=NULL)
 *     Clear the recv callback and disarm CCEV_READ. Returns CCEV_OK.
 *
 *  Once the fd is closed (CCEV_ERR from any I/O call), only
 *  ccev_conn_close() is valid.
 *
 *  @param conn  Connection handle.
 *  @param buf   Read buffer, or NULL for callback-only mode.
 *  @param len   Buffer capacity, or 0.
 *  @param cb    Readable callback. Non-NULL updates the internal recv_cb.
 *  @param udata User pointer for the callback.
 *  @return Bytes read (>0), 0 (EOF), or CCEV_ERR on error/closed.
 */
int ccev_conn_recv(ccev_conn_t *conn, void *buf, size_t len,
                    ccev_recv_cb cb, void *udata);

/** @brief Send data asynchronously.
 *
 *  Attempts a non-blocking write first. On EAGAIN the data is buffered
 *  internally and EPOLLOUT is armed automatically. The @p cb fires
 *  when the entire write buffer has been flushed.
 *
 *  If the write succeeds immediately, @p cb is called synchronously in
 *  the same call. The @p cb parameter is required to exist (non-NULL)
 *  when you want to be notified of write completion — pass NULL if you
 *  don't need a callback for this particular send.
 *
 *  @param conn  Connection handle.
 *  @param data  Data to send.
 *  @param len   Data length.
 *  @param cb    Write-complete callback. Passing a non-NULL value
 *               updates the internal send_cb. NULL preserves the
 *               existing callback.
 *  @param udata User pointer for the callback.
 *  @return Number of bytes accepted (may be less than @p len on EAGAIN
 *          if not all data is buffered), or CCEV_ERR on closed fd.
 */
int ccev_conn_send(ccev_conn_t *conn, const void *data, size_t len,
                    ccev_send_cb cb, void *udata);

/** @brief Send data with batching control (bulk-transfer friendly).
 *
 *  @param conn  Connection handle.
 *  @param data  Data to send.
 *  @param len   Data length.
 *  @param done  false = buffer only (no immediate flush attempt).
 *               true  = flush the accumulated buffer via ccsocket_sendv()
 *                       and write the remaining data. @p cb fires when
 *                       everything is sent.
 *  @param cb    Write-complete callback. Only the last non-NULL @p cb
 *               across a batch of sendall(...,done=false) calls will
 *               fire when the final flush succeeds.
 *  @param udata User pointer for the callback.
 *  @return Bytes accepted, or CCEV_ERR.
 */
int ccev_conn_sendall(ccev_conn_t *conn, const void *data, size_t len,
                       bool done, ccev_send_cb cb, void *udata);

/** @brief Send a file using kernel sendfile (zero-copy when supported).
 *
 *  Delegates to ccsocket_sendfile(). Sends the file from its current
 *  seek position. Falls back to read+send on platforms without
 *  kernel sendfile support.
 *
 *  @param conn    Connection handle.
 *  @param fd      Open file descriptor to send.
 *  @param cb      Write-complete callback. NULL preserves existing.
 *  @param udata   User pointer for the callback.
 *  @return CCEV_OK on success, CCEV_ERR on failure.
 */
int ccev_conn_sendfile(ccev_conn_t *conn, int fd, ccev_send_cb cb, void *udata);

/** @brief Shutdown and close a connection.
 *
 *  Performs a platform-appropriate shutdown of the socket, marks the
 *  handle as closed (all subsequent I/O returns CCEV_ERR), and schedules
 *  the close_cb for invocation. The user should call this from within
 *  the close_cb to complete resource teardown.
 *
 *  If the connection was created by ccev_listen() (a listener), calling
 *  this stops listening and closes the listener socket.
 *
 *  @param conn  Connection handle.
 *  @return CCEV_OK or CCEV_ERR.
 */
int ccev_conn_close(ccev_conn_t *conn);

/** @brief Set the close/error callback.
 *
 *  Fired when the peer closes the connection or an I/O error is detected
 *  (EPOLLERR / EPOLLHUP on the epoll backend). Inside this callback the
 *  user should call ccev_conn_close() to release the handle.
 *
 *  @param conn  Connection handle.
 *  @param cb    Close callback.
 *  @param udata User pointer for the callback.
 */
void ccev_conn_set_close_cb(ccev_conn_t *conn, ccev_close_cb cb, void *udata);

/* ════════════════════════════════════════════════════════════════
 *  Connection metadata & diagnostics
 * ════════════════════════════════════════════════════════════════ */

/** @brief Get the user-data pointer. */
void  *ccev_conn_get_udata(ccev_conn_t *conn);
/** @brief Set the user-data pointer. */
void   ccev_conn_set_udata(ccev_conn_t *conn, void *udata);
/** @brief Get the underlying file descriptor (diagnostic use only). */
ccsocket_t ccev_conn_fd(ccev_conn_t *conn);

/* ════════════════════════════════════════════════════════════════
 *  Timer subsystem
 * ════════════════════════════════════════════════════════════════ */

/** @brief Add a timer.
 *
 *  The timer is managed by an internal D-ary heap (4-ary, O(log n)).
 *  When @p delay_ms elapses, @p cb is called. REPEAT timers are
 *  automatically re-inserted with expiry = now + interval.
 *
 *  @param loop      Event-loop handle.
 *  @param delay_ms  Delay in milliseconds.
 *  @param mode      CCEV_TIMER_ONCE or CCEV_TIMER_REPEAT.
 *  @param cb        Timer expiry callback.
 *  @param udata     User pointer for the callback.
 *  @return Timer handle, or NULL on failure.
 */
ccev_timer_t *ccev_timer_add(ccev_loop_t *loop, uint64_t delay_ms,
                               ccev_timer_mode_t mode,
                               ccev_timer_cb cb, void *udata);

/** @brief Delete a timer.
 *
 *  The timer is lazily removed (marked inactive) and freed when the
 *  heap naturally reaches it. Safe to call from inside the timer's
 *  own callback.
 *
 *  @param loop   Event-loop handle.
 *  @param timer  Timer handle returned by ccev_timer_add().
 *  @return CCEV_OK or CCEV_ERR (if timer was already inactive).
 */
int ccev_timer_del(ccev_loop_t *loop, ccev_timer_t *timer);

/** @brief Reset (re-schedule) a timer.
 *
 *  Changes the timer's expiry to now + @p delay_ms. Uses
 *  ccheap_update() with an embedded index field for O(log n)
 *  repositioning. If the timer has already expired but hasn't been
 *  freed yet, it is re-inserted into the heap.
 *
 *  @param loop      Event-loop handle.
 *  @param timer     Timer handle.
 *  @param delay_ms  New delay from now in milliseconds.
 *  @return CCEV_OK or CCEV_ERR.
 */
int ccev_timer_reset(ccev_loop_t *loop, ccev_timer_t *timer,
                       uint64_t delay_ms);

/* ════════════════════════════════════════════════════════════════
 *  Asynchronous DNS resolver
 * ════════════════════════════════════════════════════════════════ */

/** @brief Set the DNS server addresses for resolution.
 *
 *  Default value: {"1.1.1.1"}.
 *  Must be called before any ccev_dns_resolve() call (not thread-safe).
 *
 *  @param servers  Array of server strings, e.g. {"1.1.1.1","8.8.8.8"}.
 *  @param n        Number of servers in the array.
 *  @return CCEV_OK or CCEV_ERR.
 */
int ccev_dns_set_server(ccev_loop_t *loop, const char *servers[], int n, int port);

/** @brief Resolve a domain name asynchronously.
 *
 *  Sends UDP DNS queries to all configured servers simultaneously.
 *  The first successful response wins (race mode); remaining responses
 *  are discarded. If both CCEV_DNS_A and CCEV_DNS_AAAA are specified,
 *  both A and AAAA queries are issued independently.
 *
 *  @param loop        Event-loop handle.
 *  @param domain      Domain name to resolve.
 *  @param timeout_ms  Per-query timeout in ms. 0 = no timeout.
 *  @param type        CCEV_DNS_A, CCEV_DNS_AAAA, or bitwise OR for both.
 *  @param cb          Completion callback. Fires once when at least one
 *                     query completes or all queries have failed/timed out.
 *  @param udata       User pointer for the callback.
 *  @return CCEV_OK on successful initiation, CCEV_ERR on error.
 */
int ccev_dns_resolve(ccev_loop_t *loop, const char *domain,
                      unsigned int timeout_ms, ccev_dns_type_t type,
                      ccev_dns_cb cb, void *udata);

/** @brief Free a DNS address list returned by ccev_dns_cb.
 *  @param addr  Head of the address list (NULL-safe). */
void ccev_dns_free(ccev_address_t *addr);

/* ════════════════════════════════════════════════════════════════
 *  Utilities
 * ════════════════════════════════════════════════════════════════ */

/** @brief Wake up the event loop from another thread.
 *
 *  Writes to an internal pipe to interrupt epoll_wait(). The loop
 *  will process pending events and re-enter epoll_wait().
 *  @param loop  Event-loop handle.
 *  @return CCEV_OK or CCEV_ERR.
 */
int ccev_wakeup(ccev_loop_t *loop);

/** @brief Get the number of active connections.
 *  @param loop  Event-loop handle.
 *  @return Connection count. */
int ccev_conn_count(ccev_loop_t *loop);

/** @brief Get the number of active timers.
 *  @param loop  Event-loop handle.
 *  @return Timer count. */
int ccev_timer_count(ccev_loop_t *loop);

/* ════════════════════════════════════════════════════════════════
 *  ICMP echo (ping) — requires CAP_NET_RAW / root on most systems
 * ════════════════════════════════════════════════════════════════ */

/** ICMP echo result. */
typedef struct ccev_icmp_result_s {
    char     ip[64];         /**< Target IP string. */
    double   rtt_ms;         /**< Round-trip time in milliseconds. */
    size_t   payload_len;    /**< Bytes echoed back. */
    int      ttl;            /**< Time-to-live from the response packet. */
} ccev_icmp_result_t;

/** ICMP echo completion callback. */
typedef void (*ccev_icmp_cb)(void *udata, ccev_icmp_result_t *result);

/** @brief Send an ICMP echo request (ping).
 *
  *  Uses ccicmp internally. Requires sufficient privileges
  *  (CAP_NET_RAW on Linux, root on most systems). On Linux 3.0+
 *  and macOS, a privilege-free SOCK_DGRAM+ICMP socket is tried first.
 *
 *  @param loop  Event-loop handle.
 *  @param host  Target hostname or IP address.
 *  @param cb    Completion callback. @p result is NULL on failure.
 *  @param udata User pointer passed to @p cb.
 *  @return CCEV_OK on successful initiation, CCEV_ERR on error.
 */
int ccev_icmp_echo(ccev_loop_t *loop, const char *host,
                    ccev_icmp_cb cb, void *udata);

#ifdef __cplusplus
}
#endif

#endif /* CCEV_H */
