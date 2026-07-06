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
#include "ccev_poll.h"

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <signal.h> /* sig_atomic_t */

#ifdef CCEV_USE_ATOMIC
#  include <stdatomic.h>
#endif

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

/* struct timespec (used by time-related internals) */
#include <time.h>

/* ── Compiler barrier (prevents reordering around volatile/atomic ops) ── */
#if defined(_WIN32)
#  if defined(_MSC_VER)
#    define CCEV_COMPILER_BARRIER() _ReadWriteBarrier()
#  else
#    define CCEV_COMPILER_BARRIER() __sync_synchronize()
#  endif
#elif defined(__GNUC__) || defined(__clang__) || defined(__INTEL_COMPILER)
#  define CCEV_COMPILER_BARRIER() __asm__ volatile("" ::: "memory")
#elif defined(__CC_ARM)
#  define CCEV_COMPILER_BARRIER() __schedule_barrier()
#else
#  define CCEV_COMPILER_BARRIER() do {} while(0)
#endif

/* ── Hardware acquire/release barrier (for weak-ordered CPUs) ──
 *
 * On x86/x64 loads have acquire semantics and stores have release
 * semantics naturally (strong memory model) — a compiler barrier
 * prevents reordering and suffices.  On ARM / PowerPC we need a
 * hardware DMB / lwsync instruction.
 *
 * Used by ccev_atomic_store / ccev_atomic_load when CCEV_USE_ATOMIC
 * is not defined.                                                ── */
#if defined(_WIN32)
#  if defined(_MSC_VER)
     /* MSVC native */
#    if defined(_M_ARM) || defined(_M_ARM64)
#      include <intrin.h>
#      define CCEV_ACQUIRE_BARRIER()  __dmb(_ARM_BARRIER_ISH)
#      define CCEV_RELEASE_BARRIER()  __dmb(_ARM_BARRIER_ISH)
#    else
       /* x86/x64: compiler barrier is sufficient */
#      define CCEV_ACQUIRE_BARRIER()  _ReadWriteBarrier()
#      define CCEV_RELEASE_BARRIER()  _ReadWriteBarrier()
#    endif
#  else
     /* MinGW (GCC) — __sync_synchronize is a full HW+compiler barrier */
#    define CCEV_ACQUIRE_BARRIER()    __sync_synchronize()
#    define CCEV_RELEASE_BARRIER()    __sync_synchronize()
#  endif
#elif defined(__GNUC__) || defined(__clang__) || defined(__INTEL_COMPILER)
#  if defined(__arm__) || defined(__aarch64__) || defined(_M_ARM) || defined(_M_ARM64)
     /* GCC/Clang on ARM — __atomic_thread_fence with acquire/release
      * is available since GCC 4.7 / Clang 3.1.  Emits DMB ISH. */
#    define CCEV_ACQUIRE_BARRIER()    __atomic_thread_fence(__ATOMIC_ACQUIRE)
#    define CCEV_RELEASE_BARRIER()    __atomic_thread_fence(__ATOMIC_RELEASE)
#  elif defined(__powerpc__) || defined(__ppc__) || defined(__PPC__)
     /* PowerPC — full sync barrier (lwsync would suffice but
      * __sync_synchronize is the portable choice). */
#    define CCEV_ACQUIRE_BARRIER()    __sync_synchronize()
#    define CCEV_RELEASE_BARRIER()    __sync_synchronize()
#  else
     /* x86, x64, s390x, RISC-V, etc. — strong or unknown ordering.
      * Compiler barrier prevents reordering; HW ordering suffices.
      * __sync_synchronize is a function-like builtin (expression, not
      * a statement), so it works safely inside the comma-operator
      * expression of ccev_atomic_load.  On x86 it emits an mfence
      * instruction (~40-80 cycles per call at most once per loop
      * iteration — negligible). */
#    define CCEV_ACQUIRE_BARRIER()    __sync_synchronize()
#    define CCEV_RELEASE_BARRIER()    __sync_synchronize()
#  endif
#elif defined(__CC_ARM)
   /* ARM RealView / Keil — __dmb with 0xB = DMB full system */
#  define CCEV_ACQUIRE_BARRIER()      __dmb(0xB)
#  define CCEV_RELEASE_BARRIER()      __dmb(0xB)
#else
   /* Unknown platform — ((void)0) is a no-op expression (not a
    * statement), safe inside comma-operator context.  Without any
    * barrier, ccev_atomic_load is a plain volatile read on weak
    * CPUs — architecture-specific ports should add the correct
    * barrier here. */
#  define CCEV_ACQUIRE_BARRIER()      ((void)0)
#  define CCEV_RELEASE_BARRIER()      ((void)0)
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
 *  Base socket — reactor primitive, no variant-specific fields
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
    uint32_t           events;     /**< CCEV_POLL flags currently armed*/
    uint32_t           mode;       /**< ccev_sock_mode_t                 */

    /* ── Flags (pack as 1+1 with 2-byte padding) ── */
    bool               closed;     /**< true once close initiated        */
    bool               in_closing; /**< true if in the closing list      */
};

/* ════════════════════════════════════════════════════════════════
 *  Listener variant (ccev_listen)
 * ════════════════════════════════════════════════════════════════ */

struct ccev_listener_s {
    ccev_sock_t        sock;
    ccev_listen_cb     cb;
    void              *udata;
};

/* ════════════════════════════════════════════════════════════════
 *  Connector variant (ccev_connect)
 * ════════════════════════════════════════════════════════════════ */

struct ccev_connector_s {
    ccev_sock_t        sock;
    uint16_t           port;
    ccev_timer_t      *timer;
    ccev_connect_cb    cb;
    void              *udata;
    bool               dns_alive;  /**< DNS resolution in-flight flag */
};

typedef struct ccev_listener_s  ccev_listener_t;
typedef struct ccev_connector_s ccev_connector_t;

/* ════════════════════════════════════════════════════════════════
 *  Timer structure (includes heap_index for O(log n) update)
 * ════════════════════════════════════════════════════════════════ */

struct ccev_timer_s {
    ccheap_node_t       node;       /**< Heap node (embed, not pointer)  */
    cclink_node_t       tlist;      /**< Expired list node (extract/fire)*/
    ccev_loop_t        *loop;       /**< Owning event loop               */

    uint64_t            interval;   /**< Repeat interval (0 for ONCE)    */
    ccev_timer_cb       cb;         /**< Expiry callback                 */
    void               *udata;      /**< User data                       */
    ccev_timer_mode_t   mode;       /**< ONCE or REPEAT                  */
    bool                active;     /**< false = lazily deleted          */
};

/* ════════════════════════════════════════════════════════════════
 *  Write-buffer entry (linked via cclink, used by stream)
 * ════════════════════════════════════════════════════════════════ */

typedef struct ccev_buf_s {
    cclink_node_t node;             /**< cclink intrusive node           */
    size_t        len;              /**< Data length                     */
    size_t        offset;           /**< Consumed bytes (for iovec)      */
    ccev_send_cb  cb;               /**< Per-buf write-complete callback*/
    void         *cb_udata;         /**< User pointer for @p cb         */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
    char data[];
#elif defined(__GNUC__) || defined(__clang__)
    char data[0];
#else
    char data[1];
#endif
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
    ccev_stream_cb   cb;            /**< User completion callback.       */
    void            *udata;         /**< User data for @p cb.            */
    ccev_event_cb    old_rcb;       /**< Saved read callback while active*/
    ccev_sock_t     *sock;          /**< Owning socket (back-pointer).   */
    ccev_timer_t    *timer;         /**< Read timeout timer, or NULL.    */

    /* ── Timeout / delim state ── */
    char             delim;         /**< 0 = readnum, else delimiter.    */
    bool             is_n;          /**< true = readnum, false = readline*/
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

    /* ── Stream reader (optional) ── */
    ccev_stream_reader_t *reader;

    /* ── Sendfile state ── */
    ccev_send_cb       sf_cb;      /**< Sendfile completion callback.   */
    void              *sf_udata;
    int                sendfile_fd; /**< File fd, -1 when idle.         */
    bool               pending_write;/**< EPOLLOUT currently armed.     */
};

/* ════════════════════════════════════════════════════════════════
 *  Socket allocation union — one allocation fits any variant
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Allocation union for all socket variants.
 *
 * ccev_sock_create() allocates one of these.  The pointer can be
 * reinterpreted as any variant (sock, stream, listener, connector)
 * because every variant has ccev_sock_t as its first field —
 * guaranteed safe by the common-initial-sequence rule (C11 §6.5.2.3p6).
 *
 * All variants share ccev_sock_t as their first field (C11 §6.5.2.3p6).
 * Add future variants (e.g. ccev_dgram_t) here; the union size grows automatically.
 */
typedef union {
    ccev_sock_t        sock;
    ccev_stream_t      stream;
    ccev_listener_t    listener;
    ccev_connector_t   connector;
    /* ccev_dgram_t    dgram; */
} ccev_sock_any_t;

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

/* ── Signal queue event ── */
typedef struct ccev_signal_event_s {
    cclink_node_t node;
    int           signum;
} ccev_signal_event_t;

/* ════════════════════════════════════════════════════════════════
 *  Loop structure
 * ════════════════════════════════════════════════════════════════ */

struct ccev_loop_s {
    /* ── Poll instance (platform-independent event notification) ── */
    ccev_poll_t        *poll;        /**< Platform-independent poll obj  */
#ifdef CCEV_USE_ATOMIC
    atomic_int           stop_flag; /**< Atomic stop flag (C11)         */
#else
    volatile sig_atomic_t stop_flag;/**< Atomic stop flag (fallback)    */
#endif

/* ── Atomic store/load helpers (C11 atomics when available) ── */
#ifdef CCEV_USE_ATOMIC
#  define ccev_atomic_store(p, v)  atomic_store(&(p), (v))
#  define ccev_atomic_load(p)      atomic_load(&(p))
#else
/*
 * Non-C11 path — acquire/release semantics via platform barriers.
 *
 * Store: release barrier BEFORE the store ensures all prior memory
 * operations are globally visible before (p).  Pairs with the acquire
 * barrier that precedes ccev_atomic_load on the reader side.
 *
 * Load: acquire barrier BEFORE the read prevents subsequent memory
 * operations from being reordered before the load of (p).  On strong-
 * ordered CPUs (x86, x64) this is a compiler barrier only; on ARM /
 * PowerPC it emits a DMB / lwsync hardware instruction.
 *
 * Both barriers are embedded in the macro so callers don't need to
 * remember to pair them manually — unlike the earlier design that
 * required an explicit CCEV_COMPILER_BARRIER() before each load.    */
#  define ccev_atomic_store(p, v)  do {   \
    CCEV_RELEASE_BARRIER();               \
    (p) = (v);                            \
} while(0)

#  define ccev_atomic_load(p)             \
    (CCEV_ACQUIRE_BARRIER(), (p))
#endif

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
    void               *ecb_args;

    /* ── Signal handling (default loop only) ── */
    ccsocket_t          signal_pipe[2];
    ccev_sock_t        *signal_sock;
    struct {
        ccev_signal_cb cb;
        void          *udata;
    } signals[64];
    cclink_t           signal_queue; /**< Pending signal events (ccev_signal_event_t) */
};

/* ════════════════════════════════════════════════════════════════
 *  Internal function prototypes
 * ════════════════════════════════════════════════════════════════ */

/* ── sock core (ccev_sock.c) ── */

/** Internal poll registration wrapper (always ONESHOT internally). */
int ccev__sock_mod_internal(ccev_loop_t *loop, ccev_sock_t *sock,
                             int poll_events);

/** Re-arm logic — re-register events consumed by ONESHOT. */
void ccev__sock_rearm(ccev_loop_t *loop, ccev_sock_t *sock);

/** Schedule a sock for deferred close. */
void ccev__sock_schedule_close(ccev_loop_t *loop, ccev_sock_t *sock);

/** Process the closing queue (called from ccev.c). */
void ccev__process_closing(ccev_loop_t *loop);

/** Free a sock (internal, called after close_cb). */
void ccev__sock_free(ccev_sock_t *sock);

/* ── stream I/O (ccev_stream.c) ── */

/** Free stream-specific resources (wlist, reader, sendfile).
 *  Called by ccev_stream_close() before scheduling the sock for close. */
void ccev__stream_cleanup(ccev_stream_t *st);

/* ── signal (ccev_signal.c) ── */

/** Signal pipe dispatch callback. */
void ccev__signal_dispatch(ccev_sock_t *sock, int events);

/** Process signal events — poll Windows sig_pending + drain signal_queue. */
void ccev__signal_process_queue(ccev_loop_t *loop);

/* ── timer (ccev_timer.c) ── */

/** Process expired timers. */
int ccev__timer_process(ccev_loop_t *loop, uint64_t now_ms);

/** Peek next timer expiry without firing callbacks.
 *  Returns ms until next timer, 0 if already expired, -1 if none. */
int ccev__timer_next_ms(ccev_loop_t *loop, uint64_t now_ms);

/* ccev__now_ms() declared in ccev_poll.h — shared by poll, timer, DNS, ICMP */

/* ── DNS (ccev_dns.c) ── */

/** DNS init — parse /etc/resolv.conf, fall back to 1.1.1.1. */
void ccev__dns_init(ccev_loop_t *loop);

#ifdef __cplusplus
}
#endif

#endif /* CCEV_INTERNAL_H */
