/**
 * @file ccev_internal.h
 * @brief Internal data structures for the ccev reactor.
 *
 * This header is private to the ccev library. It must NOT be included
 * by user code.
 */

#ifndef CCEV_INTERNAL_H
#define CCEV_INTERNAL_H

#include "ccev.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <windows.h>
/* Windows lacks socklen_t; define as int for getsockopt compat */
typedef int socklen_t;
#else
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#include "epoll/epoll.h"
#include "ccsocket.h"

/* Linux timerfd — MUST be included outside struct definitions */
#if defined(__linux__) || defined(__ANDROID__)
#  include <sys/timerfd.h>
#endif

/* ccheap configuration: 4-ary heap (wider = shallower, fewer pops) */
#define CCHEAP_ARITY 4
/* Enable ccheap update with embedded index (BEFORE including ccheap.h) */
#define CCHEAP_NODE_INDEX heap_index

/* ccalg data structures */
#include "ccheap.h"
#include "cclist.h"
#include "cclink.h"

/*
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════
 *  Connection type tags
 * ════════════════════════════════════════════════════════════════ */

typedef enum {
    CCEV_CONN_UNUSED  = 0,
    CCEV_CONN_NORMAL,     /**< Regular connection (external fd / accept) */
    CCEV_CONN_LISTENER,   /**< TCP listener created by ccev_listen()     */
    CCEV_CONN_CONNECTING, /**< TCP connect in progress                   */
    CCEV_CONN_DNS,        /**< Internal DNS socket                       */
} ccev_conn_type_t;

/* ════════════════════════════════════════════════════════════════
 *  Connection structure
 * ════════════════════════════════════════════════════════════════ */

struct ccev_conn_s {
    cclist_node_t      lnode;          /**< List node (all conns)          */
    ccev_loop_t       *loop;           /**< Owning event loop              */
    ccev_conn_type_t   type;           /**< Connection type tag            */
    ccsocket_t         fd;             /**< Underlying file descriptor     */
    bool               closed;         /**< true once close initiated      */
    bool               in_closing;     /**< true if in the closing list    */

    /* ── Registered epoll events (internal ONESHOT tracking) ── */
    int                reg_events;     /**< Currently registered epoll events */
    /* ── Recv callback (one at a time, last set wins) ── */
    ccev_recv_cb       recv_cb;
    void              *recv_udata;

    /* ── Send callback (one at a time, last set wins) ── */
    ccev_send_cb       send_cb;
    void              *send_udata;

    /* ── Close callback ── */
    ccev_close_cb      close_cb;
    void              *close_udata;

    /* ── User-data pointer ── */
    void              *udata;

    /* ── Write buffering ── */
    cclink_t           wbuf_list;      /**< Linked list of pending buffers */
    size_t             wbuf_len;       /**< Total bytes buffered           */
    bool               pending_write;  /**< EPOLLOUT is currently armed    */
    /* ── Type-specific fields ── */
    union {
        struct {
            ccev_accept_cb cb;
            void          *udata;
        } listener;

        struct {
            ccev_connect_cb cb;
            void           *udata;
            ccev_timer_t   *timer;      /**< Connection timeout timer     */
        } connector;

    };
};

/* ════════════════════════════════════════════════════════════════
 *  Timer structure (includes heap_index for O(log n) update)
 * ════════════════════════════════════════════════════════════════ */

struct ccev_timer_s {
    ccheap_node_t       node;           /**< Heap node (embed, not pointer) */
    ccev_loop_t        *loop;           /**< Owning event loop              */

    uint64_t            interval;       /**< Repeat interval (0 for ONCE)   */
    ccev_timer_mode_t   mode;           /**< ONCE or REPEAT                 */
    ccev_timer_cb       cb;             /**< Expiry callback                */
    void               *udata;          /**< User data                      */
    bool                active;         /**< false = lazily deleted         */
};

/* ════════════════════════════════════════════════════════════════
 *  Write-buffer entry (linked via cclink)
 * ════════════════════════════════════════════════════════════════ */

typedef struct ccev_buf_s {
    cclink_node_t node;                 /**< cclink intrusive node         */
    void         *data;                 /**< Heap-allocated buffer         */
    size_t        len;                  /**< Data length                   */
    size_t        offset;               /**< Consumed bytes (for iovec)    */
} ccev_buf_t;

/* ════════════════════════════════════════════════════════════════
 *  DNS state (per-loop, but shared globally as well)
 * ════════════════════════════════════════════════════════════════ */

typedef struct ccev_dns_state_s {
    const char *servers[4];             /**< DNS server addresses          */
    int         nservers;               /**< Number of servers             */
    int         port;                   /**< DNS server port               */
} ccev_dns_state_t;

/* ════════════════════════════════════════════════════════════════
 *  Loop structure
 * ════════════════════════════════════════════════════════════════ */

struct ccev_loop_s {
    /* ── Allocator hooks ── */
    void *(*realloc_fn)(void*, size_t);
    void  (*free_fn)(void*);

    /* ── epoll instance ── */
    HANDLE              epfd;           /**< epoll fd (per epoll.h types)  */
    int                 max_events;     /**< epoll_wait() event cap         */
    struct epoll_event *events;         /**< epoll_wait() result array      */
    volatile bool       stop_flag;      /**< Atomic stop flag               */

    /* ── Wakeup pipe ── */
    ccsocket_t          wakefds[2];     /**< Self-pipe for async wakeup    */
    ccev_conn_t        *wake_conn;      /**< Wrapper for wakeup fd         */

    /* ── Connection table (fd → conn) ── */
    cclist_t            all_conns;      /**< All conns (for iteration)     */
    int                 conn_count;     /**< Active connection count        */

    /* ── Closing queue (deferred free) ── */
    cclist_t            closing;        /**< Nodes to tear down after dispatch */

    /* ── Timer heap ── */
    ccheap_t            timers;         /**< D-ary min-heap (4-ary)        */
    int                 timer_count;    /**< Active timer count             */

    /* ── Timerfd (Linux/Android only, for ns precision) ── */
#if defined(__linux__) || defined(__ANDROID__)
    int                 timerfd;        /**< timerfd fd for hrtimer         */
#endif

    /* ── DNS state ── */
    ccev_dns_state_t    dns;

    /* ── Signal handling (default loop only) ── */
    ccsocket_t              signal_pipe[2]; /**< Self-pipe for signal delivery */
    ccev_conn_t            *signal_conn;    /**< Wrapper for signal pipe read end */
    struct {
        ccev_signal_cb cb;
        void          *udata;
    } signals[64];                           /**< signum → callback table */
};

/* ════════════════════════════════════════════════════════════════
 *  Internal function prototypes (shared across .c files)
 * ════════════════════════════════════════════════════════════════ */

/* epoll event mask conversion */

/* Internal epoll_ctl wrapper (always ONESHOT) */
int ccev__conn_mod_internal(ccev_loop_t *loop, ccev_conn_t *conn,
                             int events);

/* Re-arm logic — re-register events that were consumed by ONESHOT */
void ccev__conn_rearm(ccev_loop_t *loop, ccev_conn_t *conn);

/* Flush write buffer (called from dispatch on EPOLLOUT) */
void ccev__conn_flush(ccev_loop_t *loop, ccev_conn_t *conn);

/* Schedule a conn for deferred close */
void ccev__conn_schedule_close(ccev_loop_t *loop, ccev_conn_t *conn);

/* Connection cleanup (internal, frees memory) */
void ccev__conn_free(ccev_loop_t *loop, ccev_conn_t *conn);

/* Timer dispatch */
void ccev__timer_process(ccev_loop_t *loop, uint64_t now_ms);

/* Monotonic clock in milliseconds */
uint64_t ccev__now_ms(void);



#ifdef __cplusplus
}
#endif

#endif /* CCEV_INTERNAL_H */
