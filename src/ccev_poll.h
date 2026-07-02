/**
 * @file ccev_poll.h
 * @brief Platform-independent event notification layer.
 *
 * @author CandyMi
 * @license MIT
 *
 * Thin internal wrapper around epoll (Linux), kqueue (macOS/BSD), IOCP
 * (Windows).  Exposes only the 6 operations ccev actually needs — no
 * edge-triggering, no EPOLLPRI, no EPOLLEXCLUSIVE.
 *
 * User registrations use ONESHOT semantics internally: every event fires
 * at most once and must be re-armed (via add/mod) after dispatch.  This
 * matches ccev's dispatch-and-rearm pattern and avoids the race between
 * event notification and processing that level-triggered epoll can hit.
 * The internal wake pipe is the sole exception — level-triggered, no re-arm.
 *
 * Wake pipe:
 *   Each poll instance owns an internal pipe that drains silently inside
 *   ccev__poll_wait.  ccev__poll_wake() writes to the pipe to interrupt
 *   a blocking wait.  The wake fd is registered at create time and never
 *   leaked to the user.
 */

#ifndef CCEV_POLL_H
#define CCEV_POLL_H

#include "ccsocket.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════
 *  Monotonic clock — shared by poll layer, timer, DNS, and ICMP
 * ════════════════════════════════════════════════════════════════ */

/**
 * Monotonic milliseconds since an unspecified epoch.
 * Uses QueryPerformanceCounter (Windows), mach_absolute_time (macOS),
 * or clock_gettime(CLOCK_MONOTONIC) (POSIX).
 */
uint64_t ccev__now_ms(void);

/* ════════════════════════════════════════════════════════════════
 *  Event flags (platform-independent)
 * ════════════════════════════════════════════════════════════════ */

enum {
    CCEV_POLL_READ  = 1 << 0,  /**< fd readable       */
    CCEV_POLL_WRITE = 1 << 1,  /**< fd writable        */
    CCEV_POLL_ERR   = 1 << 2,  /**< fd error condition */
    CCEV_POLL_HUP   = 1 << 3,  /**< fd hung up         */
};

/* ════════════════════════════════════════════════════════════════
 *  Opaque type
 * ════════════════════════════════════════════════════════════════ */

typedef struct ccev_poll_s ccev_poll_t;

/* ════════════════════════════════════════════════════════════════
 *  Event struct & callback
 * ════════════════════════════════════════════════════════════════ */

struct ccev_poll_event {
    void          *udata;    /**< user data (from add/mod)     */
    int            events;   /**< CCEV_POLL_READ|WRITE|ERR|HUP */
};

/**
 * Callback invoked by ccev__poll_wait for each ready fd.
 * @param ev  Event details (stack-allocated, valid only during call)
 * @param arg  User-supplied context (passed through from wait)
 */
typedef void (*ccev_poll_cb)(struct ccev_poll_event *ev, void *arg);

/* ════════════════════════════════════════════════════════════════
 *  API
 * ════════════════════════════════════════════════════════════════ */

/**
 * Create a poll instance.
 * @param max_events  Capacity hint for epoll_wait / kevent result buffer.
 * @return  New instance, or NULL on failure.
 */
ccev_poll_t *ccev__poll_create(int max_events);

/**
 * Destroy a poll instance and release all resources.
 * All registered fds are implicitly removed.  Safe to call with NULL.
 */
void ccev__poll_destroy(ccev_poll_t *p);

/**
 * Register a new fd for event monitoring (behaves like epoll_ctl ADD).
 * The implementation always applies ONESHOT semantics — the event fires
 * once and must be re-armed via ccev__poll_mod after dispatch.
 *
 * If the fd is already registered, attempts MOD as a fallback
 * (mirrors the original ccev__sock_mod_internal ADD→MOD retry).
 *
 * @return 0 on success, -1 on failure.
 */
int ccev__poll_add(ccev_poll_t *p, ccsocket_t fd, void *udata, int events);

/**
 * Modify an existing fd's monitored events (behaves like epoll_ctl MOD).
 * @return 0 on success, -1 on failure.
 */
int ccev__poll_mod(ccev_poll_t *p, ccsocket_t fd, void *udata, int events);

/**
 * Remove an fd from monitoring (behaves like epoll_ctl DEL).
 * @return 0 on success, -1 on failure.
 */
int ccev__poll_del(ccev_poll_t *p, ccsocket_t fd);

/**
 * Wait for events.  Blocks up to timeout_ms milliseconds.
 *
 * For each ready fd, the callback cb is invoked with a populated
 * ccev_poll_event.  The wake pipe (owned by this instance) is drained
 * internally — the callback never sees it.
 *
 * @param p           Poll instance.
 * @param timeout_ms  -1 = block indefinitely, 0 = poll, >0 = ms.
 * @param cb          Per-event callback (must not be NULL).
 * @param arg         Opaque pointer forwarded to cb.
 * @return  Number of events dispatched, or -1 on error.
 */
int ccev__poll_wait(ccev_poll_t *p, int timeout_ms,
                     ccev_poll_cb cb, void *arg);

/**
 * Interrupt a blocking ccev__poll_wait (or ccev__poll_wait in progress).
 * Writes a byte to the internal wake pipe.
 */
void ccev__poll_wake(ccev_poll_t *p);

#ifdef __cplusplus
}
#endif

#endif /* CCEV_POLL_H */
