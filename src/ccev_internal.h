/**
 * @file ccev_internal.h
 * @brief Internal data structures for the ccev reactor.
 *
 * @author CandyMi
 * @license MIT
 *
 * This header is private to the ccev library. It must NOT be included
 * by user code.
 */

#ifndef CCEV_INTERNAL_H
#define CCEV_INTERNAL_H

#include "ccev.h"
#include "ccsocket.h"
#include "epoll/epoll.h"

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <signal.h> /* sig_atomic_t */

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

/* struct timespec (used by ccev__now_ms on Linux/POSIX) */
#include <time.h>

/* ── Compiler barrier (prevents reordering around volatile/atomic ops) ── */
#if defined(_WIN32)
#   if defined(_MSC_VER)
#     define CCEV_COMPILER_BARRIER() _ReadWriteBarrier()
#   else
#     define CCEV_COMPILER_BARRIER() __sync_synchronize()
#   endif
#elif defined(__GNUC__) || defined(__clang__) || defined(__INTEL_COMPILER)
#   define CCEV_COMPILER_BARRIER() __asm__ volatile("" ::: "memory")
#elif defined(__CC_ARM)
#   define CCEV_COMPILER_BARRIER() __schedule_barrier()
#else
#   define CCEV_COMPILER_BARRIER() do {} while(0)
#endif

/* ── Global allocator (set via ccev_set_allocator, visible to all .c files) ── */
extern void *(*ccev__realloc_fn)(void*, size_t);
extern void  (*ccev__free_fn)(void*);

/* Route ccheap / cchashmap internal allocations through ccev's allocator */
#define CCHEAP_REALLOC(ptr, sz)     ccev__realloc_fn((ptr), (sz))
#define CCHEAP_FREE(ptr)            ccev__free_fn((ptr))
#define CCHASHMAP_REALLOC(ptr, sz)  ccev__realloc_fn((ptr), (sz))
#define CCHASHMAP_FREE(ptr)         ccev__free_fn((ptr))

/* ccheap configuration: 4-ary heap (wider = shallower, fewer pops) */
#define CCHEAP_ARITY 4
/* Enable ccheap update with embedded index (BEFORE including ccheap.h) */
#define CCHEAP_NODE_INDEX heap_index

/* Inline compare macro — replaces function-pointer dispatch.
 * (a).timeout < (b).timeout  →  a has higher priority (closer to root)
 * Subtracting int64_t is branchless for uint64_t keys. */
#define CCHEAP_COMPARE(a, b) \
    ((int64_t)((b)->timeout) - (int64_t)((a)->timeout))

/* ccalg data structures */
#include "ccheap.h"
#include "cclist.h"
#include "cclink.h"
#include "cchashmap.h"

/* Maximum connections to accept per EPOLLIN dispatch on a listener. */
#define CCEV_MAX_ACCEPT_BATCH 128

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════
 *  Socket mode tags (internal, used by dispatch)
 * ════════════════════════════════════════════════════════════════ */

typedef enum {
    CCEV_SOCK_INIT    = 0,   /**< Normal data socket.                    */
    CCEV_SOCK_LISTEN,         /**< TCP listener created by ccev_listen(). */
    CCEV_SOCK_CONNECT,        /**< TCP connect in progress.              */
} ccev_sock_mode_t;

/* ════════════════════════════════════════════════════════════════
 *  Stream reader (forward declaration for sock/stream structs)
 * ════════════════════════════════════════════════════════════════ */

typedef struct ccev_stream_reader_s ccev_stream_reader_t;

/* ════════════════════════════════════════════════════════════════
 *  Socket structure (low-level)
 * ════════════════════════════════════════════════════════════════ */

struct ccev_sock_s {
    cclist_node_t      lnode;      /**< List node (all_socks list)       */

    /* ── 8-byte fields first ── */
    ccev_loop_t       *loop;       /**< Owning event loop                */
    void              *udata;      /**< User-data pointer                */
    ccev_event_cb      rcb;        /**< Read event callback (INIT mode)  */
    ccev_event_cb      wcb;        /**< Write event callback (INIT mode) */
    ccev_close_cb      close_cb;   /**< Close / error callback           */
    void              *close_udata;/**< Close callback user data         */

    /* ── 4-byte fields ── */
    ccsocket_t         fd;         /**< Underlying file descriptor       */
    uint32_t           events;     /**< epoll currently registered events*/
    uint32_t           mode;       /**< ccev_sock_mode_t                 */

    /* ── Flags ── */
    bool               closed;     /**< true once close initiated        */
    bool               in_closing; /**< true if in the closing list      */

    /* ── Listen-specific (only when mode == CCEV_SOCK_LISTEN) ── */
    struct {
        ccev_listen_cb cb;        /**< Accept callback for new conns    */
        void          *udata;
    } listener;

    /* ── Connect-specific (only when mode == CCEV_SOCK_CONNECT) ── */
    struct {
        char             host[CCEV_DOMAIN_MAXLEN];
        uint16_t         port;
        unsigned int     timeout_ms;
        ccev_timer_t    *timer;
        ccev_connect_cb  cb;
        void            *udata;
        bool            *dns_cancelled; /**< Set true before free so DNS
                                         *   callback skips the dead sock.*/
    } connector;
};

/* ════════════════════════════════════════════════════════════════
 *  Timer structure (includes heap_index for O(log n) update)
 * ════════════════════════════════════════════════════════════════ */

struct ccev_timer_s {
    ccheap_node_t       node;       /**< Heap node (embed, not pointer)  */
    cclink_node_t       tlist;      /**< Expired list node (extract/fire)*/
    ccev_loop_t        *loop;       /**< Owning event loop               */

    uint64_t            interval;   /**< Repeat interval (0 for ONCE)    */
    ccev_timer_mode_t   mode;       /**< ONCE or REPEAT                  */
    ccev_timer_cb       cb;         /**< Expiry callback                 */
    void               *udata;      /**< User data                       */
    bool                active;     /**< false = lazily deleted          */
};

/* ════════════════════════════════════════════════════════════════
 *  Write-buffer entry (linked via cclink, used by stream)
 * ════════════════════════════════════════════════════════════════ */

typedef struct ccev_buf_s {
    cclink_node_t node;             /**< cclink intrusive node           */
    void         *data;             /**< Heap-allocated buffer           */
    size_t        len;              /**< Data length                     */
    size_t        offset;           /**< Consumed bytes (for iovec)      */
    ccev_send_cb  cb;               /**< Per-buf write-complete callback*/
    void         *cb_udata;         /**< User pointer for @p cb         */
} ccev_buf_t;

/* ════════════════════════════════════════════════════════════════
 *  Stream reader (read-until / read-n)
 * ════════════════════════════════════════════════════════════════ */

struct ccev_stream_reader_s {
    char            *buf;           /**< Dynamic accumulation buffer.     */
    size_t           cap;           /**< Allocated capacity.              */
    size_t           pos;           /**< Consumed bytes offset in @p buf.*/
    size_t           len;           /**< Valid pending bytes in @p buf.  */
    size_t           want;          /**< maxlen (readline) or n (readnum)*/
    char             delim;         /**< 0 = readnum, else delimiter.    */
    bool             is_n;          /**< true = readnum, false = readline*/
    ccev_stream_cb   cb;            /**< User completion callback.       */
    void            *udata;         /**< User data for @p cb.            */
    ccev_event_cb    old_rcb;       /**< Saved read callback while active*/
    ccev_sock_t     *sock;          /**< Owning socket (back-pointer).   */

    /* ── Timeout support ── */
    ccev_timer_t    *timer;         /**< Read timeout timer, or NULL.    */
    int              timeout_ms;    /**< Saved timeout value.            */
};

/* ════════════════════════════════════════════════════════════════
 *  Stream structure (high-level, embeds ccev_sock_t)
 * ════════════════════════════════════════════════════════════════ */

struct ccev_stream_s {
    ccev_sock_t        sock;        /**< Embedded sock — MUST be first. */

    /* ── Write buffering ── */
    cclink_t           wlist;       /**< Linked list of pending buffers.*/
    size_t             wbuf_len;    /**< Total bytes buffered.           */
    ccev_send_cb       send_cb;     /**< Global flush-complete callback.*/
    void              *send_udata;
    bool               pending_write;/**< EPOLLOUT currently armed.     */

    /* ── Stream reader (optional) ── */
    ccev_stream_reader_t *reader;

    /* ── Sendfile state ── */
    int                sendfile_fd; /**< File fd, -1 when idle.         */
    ccev_send_cb       sf_cb;      /**< Sendfile completion callback.   */
    void              *sf_udata;
};

/* ════════════════════════════════════════════════════════════════
 *  DNS state + cache
 * ════════════════════════════════════════════════════════════════ */

typedef struct ccev_dns_state_s {
    char     server[CCEV_DOMAIN_MAXLEN]; /**< DNS server IP string (embedded, no alloc). */
    uint16_t port;                       /**< DNS server port (typically 53). */
    bool     initialized;                /**< true once configured. */
} ccev_dns_state_t;

/** DNS cache entry — domain → IP mapping */
typedef struct ccev_dns_cache_s {
    cchashmap_node_t  node;
    char              domain[256];
    char              ip[65];
    uint64_t          expires;      /**< expiry timestamp (ms), UINT64_MAX = never (hosts). */
} ccev_dns_cache_t;

/** Pending DNS waiter */
typedef struct ccev_dns_waiter_s {
    cclink_node_t  node;
    ccev_dns_cb    cb;
    void          *udata;
} ccev_dns_waiter_t;

/** Pending DNS resolution */
typedef struct ccev_dns_pending_s {
    cchashmap_node_t  node;
    char              domain[256];
    cclink_t          waiters;
} ccev_dns_pending_t;

/* ════════════════════════════════════════════════════════════════
 *  Loop structure
 * ════════════════════════════════════════════════════════════════ */

struct ccev_loop_s {
    /* ── epoll instance ── */
    HANDLE              epfd;        /**< epoll fd (per epoll.h types)  */
    int                 max_events;  /**< epoll_wait() event cap         */
    struct epoll_event *events;      /**< epoll_wait() result array      */
    volatile sig_atomic_t stop_flag; /**< Atomic stop flag               */

    /* ── Wakeup pipe ── */
    ccsocket_t          wakefds[2];  /**< Self-pipe for async wakeup    */
    ccev_sock_t        *wake_sock;   /**< Wrapper for wakeup fd         */

    /* ── Socket table ── */
    cclist_t            all_socks;   /**< All socks (for iteration)     */
    int                 sock_count;  /**< Active socket count           */

    /* ── Closing queue (deferred free) ── */
    cclist_t            closing;     /**< Socks to tear down after dispatch */

    /* ── Timer heap ── */
    ccheap_t            timers;      /**< D-ary min-heap (4-ary)        */
    int                 timer_count; /**< Active timer count             */

    /* ── DNS state ── */
    ccev_dns_state_t    dns;
    cchashmap_t         dns_cache;
    cchashmap_t         dns_pending;

    /* ── Per-iteration callback (optional, set via ccev_each) ── */
    ccev_loop_each_cb   ecb;

    /* ── Signal handling (default loop only) ── */
    volatile sig_atomic_t sig_pending; /**< Non-zero = signal pending (Win)*/
    ccsocket_t          signal_pipe[2];
    ccev_sock_t        *signal_sock;
    struct {
        ccev_signal_cb cb;
        void          *udata;
    } signals[64];
};

/* ════════════════════════════════════════════════════════════════
 *  Internal function prototypes
 * ════════════════════════════════════════════════════════════════ */

/* ── sock core (ccev_sock.c) ── */

/** Internal epoll_ctl wrapper (always ONESHOT). */
int ccev__sock_mod_internal(ccev_loop_t *loop, ccev_sock_t *sock,
                             int epoll_events);

/** Re-arm logic — re-register events consumed by ONESHOT. */
void ccev__sock_rearm(ccev_loop_t *loop, ccev_sock_t *sock);

/** Schedule a sock for deferred close. */
void ccev__sock_schedule_close(ccev_loop_t *loop, ccev_sock_t *sock);

/** Process the closing queue (called from ccev.c). */
void ccev__process_closing(ccev_loop_t *loop);

/** Free a sock (internal, called after close_cb). */
void ccev__sock_free(ccev_sock_t *sock);

/* ── stream I/O (ccev_stream.c) ── */

/* ── signal (ccev_signal.c) ── */

/** Signal pipe dispatch callback. */
void ccev__signal_dispatch(ccev_sock_t *sock, int events);

/* ── timer (ccev_timer.c) ── */

/** Process expired timers. */
int ccev__timer_process(ccev_loop_t *loop, uint64_t now_ms);

/** Monotonic clock in milliseconds. */
uint64_t ccev__now_ms(void);

/* ── DNS (ccev_dns.c) ── */

/** DNS init — parse /etc/resolv.conf, fall back to 1.1.1.1. */
void ccev__dns_init(ccev_loop_t *loop);

#ifdef __cplusplus
}
#endif

#endif /* CCEV_INTERNAL_H */
