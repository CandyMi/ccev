/**
 * @file ccev_poll.c
 * @brief Platform-independent event notification — epoll (Linux) / kqueue (macOS/BSD).
 *
 * @author CandyMi
 * @license MIT
 *
 * User registrations use ONESHOT semantics: every event fires once and
 * must be re-armed via ccev__poll_mod.  The internal wake pipe is
 * level-triggered (no ONESHOT) — no re-arm needed.
 *
 * Platform backends:
 *   Linux   — native epoll  (<sys/epoll.h>)
 *   macOS   — native kqueue (<sys/event.h>)
 *   BSD     — native kqueue (<sys/event.h>)
 *   Windows — native IOCP
 */

#include "ccev_poll.h"
#include <string.h>
#include <errno.h>
#include <time.h>          /* clock_gettime(CLOCK_MONOTONIC)  */
#include <fcntl.h>         /* F_SETFD, FD_CLOEXEC           */
#if defined(__APPLE__)
#  include <mach/mach_time.h>
#elif defined(_WIN32)
#  include <windows.h>     /* LARGE_INTEGER, QueryPerformance* */
#endif

/* Use ccev's global allocator (compatible with OOM test framework).
 * Forward-declared externs — ccev_internal.h not included (that would
 * leak ccev_loop_s and other internal types into this encapsulation). */
extern void *(*ccev__realloc_fn)(void*, size_t);
extern void  (*ccev__free_fn)(void*);

/* ════════════════════════════════════════════════════════════════
 *  Platform detection
 * ════════════════════════════════════════════════════════════════ */

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
#  define CCEV_POLL_BACKEND_KQUEUE 1
#  include <sys/types.h>
#  include <sys/event.h>
#  include <sys/time.h>
#  include <unistd.h>
#else
#  define CCEV_POLL_BACKEND_EPOLL  1
#  include <epoll/epoll.h>
#  ifndef _WIN32
#    include <unistd.h>
#  endif
#endif

/* ════════════════════════════════════════════════════════════════
 *  Monotonic clock — after platform detection so all types are visible
 * ════════════════════════════════════════════════════════════════ */

uint64_t ccev__now_ms(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER count;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)((count.QuadPart * 1000LL) / freq.QuadPart);
#elif defined(__APPLE__)
    uint64_t ns = mach_absolute_time();
    static mach_timebase_info_data_t info = {0};
    if (info.denom == 0) mach_timebase_info(&info);
    return (ns * info.numer) / (info.denom * 1000000ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
#endif
}

/* epoll_create1(EPOLL_CLOEXEC) is Linux-specific.  We use epoll_create(1)
 * (size=1, ignored on modern kernels) and set CLOEXEC via fcntl manually.
 * On Windows/wepoll, HANDLE is a pointer — NULL signals failure.          */
#if defined(_WIN32)
#  define EPOLL_FD_VALID(fd)  ((fd) != NULL)
#  define EPOLL_FD_INVALID(fd) ((fd) == NULL)
#else
#  define EPOLL_FD_VALID(fd)  ((intptr_t)(fd) >= 0)
#  define EPOLL_FD_INVALID(fd) ((intptr_t)(fd) < 0)
#endif

/* ════════════════════════════════════════════════════════════════
 *  Backend: epoll (Linux / Windows)
 * ════════════════════════════════════════════════════════════════ */

#if CCEV_POLL_BACKEND_EPOLL

/* ── Flag mapping ── */

#define CCEV_POLL2NATIVE(f)  (                                         \
    (((f) & CCEV_POLL_READ)  ? EPOLLIN    : 0) |                       \
    (((f) & CCEV_POLL_WRITE) ? EPOLLOUT   : 0) |                       \
    (((f) & CCEV_POLL_ERR)   ? EPOLLERR   : 0) |                       \
    (((f) & CCEV_POLL_HUP)   ? EPOLLHUP   : 0)                         \
)

#define NATIVE2CCEV_POLL(f)  (                                         \
    (((f) & EPOLLIN)    ? CCEV_POLL_READ  : 0) |                       \
    (((f) & EPOLLOUT)   ? CCEV_POLL_WRITE : 0) |                       \
    (((f) & EPOLLERR)   ? CCEV_POLL_ERR   : 0) |                       \
    (((f) & EPOLLHUP)   ? CCEV_POLL_HUP   : 0)                         \
)

/* ── Internal struct ── */

struct ccev_poll_s {
    HANDLE              epfd;         /**< epoll fd (int on Linux, void* on Win) */
    int                 max_events;   /**< events[] capacity             */
    struct epoll_event *events;       /**< epoll_wait result buffer      */
    ccsocket_t          wakefds[2];   /**< [0]=read end, [1]=write end  */
};

/* ── create / destroy ── */

ccev_poll_t *ccev__poll_create(int max_events) {
    struct ccev_poll_s *p = (struct ccev_poll_s *)
        ccev__realloc_fn(NULL, sizeof(struct ccev_poll_s));
    if (!p) return NULL;
    memset(p, 0, sizeof(struct ccev_poll_s));

    if (max_events <= 0) max_events = 64;
    p->max_events = max_events;
    p->epfd        = (HANDLE)0;
    p->wakefds[0]  = p->wakefds[1] = (ccsocket_t)-1;

    p->epfd = epoll_create(1);
    if (EPOLL_FD_INVALID(p->epfd)) goto fail;
#if !defined(_WIN32)
    fcntl(p->epfd, F_SETFD, FD_CLOEXEC);
#endif
    p->events = (struct epoll_event *)
        ccev__realloc_fn(NULL, (size_t)p->max_events * sizeof(struct epoll_event));
    if (!p->events) goto fail;

    /* Wake pipe */
    if (ccsocket_pipe(p->wakefds) < 0) goto fail;
    ccsocket_set_nonblock(p->wakefds[0], true);
    ccsocket_set_nonblock(p->wakefds[1], true);

    /* Register wake pipe (level-triggered, no ONESHOT) */
    {
        struct epoll_event wee;
        memset(&wee, 0, sizeof(wee));
        wee.events   = EPOLLIN;
        wee.data.ptr = NULL;
        if (epoll_ctl(p->epfd, EPOLL_CTL_ADD, p->wakefds[0], &wee) < 0)
            goto fail;
    }

    return p;

fail:;
    int saved_errno = errno;
    if (p->events)    ccev__free_fn(p->events);
    if (EPOLL_FD_VALID(p->epfd)) epoll_close(p->epfd);
    if (p->wakefds[0] != (ccsocket_t)-1) ccsocket_close(p->wakefds[0]);
    if (p->wakefds[1] != (ccsocket_t)-1) ccsocket_close(p->wakefds[1]);
    ccev__free_fn(p);
    errno = saved_errno;
    return NULL;
}

void ccev__poll_destroy(ccev_poll_t *p) {
    if (!p) return;
    if (p->wakefds[0] != (ccsocket_t)-1) ccsocket_close(p->wakefds[0]);
    if (p->wakefds[1] != (ccsocket_t)-1) ccsocket_close(p->wakefds[1]);
    if (p->events) ccev__free_fn(p->events);
    if (EPOLL_FD_VALID(p->epfd)) epoll_close(p->epfd);
    ccev__free_fn(p);
}

/* ── add / mod / del ── */

int ccev__poll_add(ccev_poll_t *p, ccsocket_t fd, void *udata, int events) {
    struct epoll_event ee;
    memset(&ee, 0, sizeof(ee));
    ee.events   = CCEV_POLL2NATIVE(events) | EPOLLONESHOT;
    ee.data.ptr = udata;

    if (epoll_ctl(p->epfd, EPOLL_CTL_ADD, fd, &ee) < 0) {
        /* EEXIST — retry as MOD */
        if (epoll_ctl(p->epfd, EPOLL_CTL_MOD, fd, &ee) < 0)
            return -1;
    }
    return 0;
}

int ccev__poll_mod(ccev_poll_t *p, ccsocket_t fd, void *udata, int events) {
    struct epoll_event ee;
    memset(&ee, 0, sizeof(ee));
    ee.events   = CCEV_POLL2NATIVE(events) | EPOLLONESHOT;
    ee.data.ptr = udata;

    if (epoll_ctl(p->epfd, EPOLL_CTL_MOD, fd, &ee) < 0)
        return -1;
    return 0;
}

int ccev__poll_del(ccev_poll_t *p, ccsocket_t fd) {
    if (epoll_ctl(p->epfd, EPOLL_CTL_DEL, fd, NULL) < 0)
        return -1;
    return 0;
}

/* ── wait ── */

static inline int _backend_wait(ccev_poll_t *p, int timeout_ms,
                                 ccev_poll_cb cb, void *arg)
{
    int n;
    uint64_t t0;

    if (timeout_ms > 0) {
        /* Finite timeout — retry EINTR with timeout decay */
        t0 = ccev__now_ms();
        for (;;) {
            n = epoll_wait(p->epfd, p->events, p->max_events, timeout_ms);
            if (n >= 0 || errno != EINTR) break;
            int64_t elapsed = (int64_t)(ccev__now_ms() - t0);
            if (elapsed > 0) {
                timeout_ms -= (int)elapsed;
                if (timeout_ms <= 0) { n = 0; break; }
            }
            t0 = ccev__now_ms();
        }
    } else {
        /* Poll (0) or indefinite (-1) — bounded EINTR retry */
        int retries = 0;
        do {
            n = epoll_wait(p->epfd, p->events, p->max_events, timeout_ms);
        } while (n < 0 && errno == EINTR && ++retries < 10);
    }

    if (n < 0) return -1;

    int dispatched = 0;
    for (int i = 0; i < n; i++) {
        struct epoll_event *ev = &p->events[i];

        if (ev->data.ptr == NULL) {
            /* Wake pipe — drain silently */
            char buf[64];
            while (ccsocket_recv(p->wakefds[0], buf, sizeof(buf), NULL) == CC_OPCODE_OK) {}
            continue;
        }

        struct ccev_poll_event pev;
        pev.udata  = ev->data.ptr;
        pev.events = NATIVE2CCEV_POLL(ev->events);

        cb(&pev, arg);
        dispatched++;
    }

    return dispatched;
}

/* ════════════════════════════════════════════════════════════════
 *  Backend: kqueue (macOS / BSD)
 * ════════════════════════════════════════════════════════════════ */

#elif CCEV_POLL_BACKEND_KQUEUE

/* ── Flag mapping ──
 *
 * kqueue uses separate EVFILT_READ / EVFILT_WRITE filters rather than
 * bitmask flags.  We translate CCEV_POLL flags to filter IDs for
 * registration, and combine them back when receiving events.
 *
 * Errors: on kqueue, EOF bytes (EV_EOF) map to CCEV_POLL_HUP.
 * Pure error notification via EVFILT_EXCEPT (macOS) is not used —
 * ccev's level-triggered + ONESHOT pattern doesn't need it.         */

#define CCEV_POLL_HAS_READ(f)  ((f) & CCEV_POLL_READ)
#define CCEV_POLL_HAS_WRITE(f) ((f) & CCEV_POLL_WRITE)

/* ── Internal struct ── */

struct ccev_poll_s {
    int                 kq;           /**< kqueue fd                     */
    int                 max_events;   /**< kevent result buffer capacity */
    struct kevent      *events;       /**< kevent result buffer          */
    ccsocket_t          wakefds[2];   /**< [0]=read end, [1]=write end  */
};

/* ── Helper: submit 1–2 kevent changes (for add/mod/del) ── */

static int _kevent_ctl(struct ccev_poll_s *p, ccsocket_t fd,
                        void *udata, int ccev_events, uint16_t flags)
{
    struct kevent kev[2];
    int n = 0;

    if (ccev_events & CCEV_POLL_READ)
        EV_SET(&kev[n++], (uintptr_t)fd, EVFILT_READ, flags, 0, 0, udata);
    if (ccev_events & CCEV_POLL_WRITE)
        EV_SET(&kev[n++], (uintptr_t)fd, EVFILT_WRITE, flags, 0, 0, udata);

    if (n == 0) return 0;
    if (kevent(p->kq, kev, n, NULL, 0, NULL) < 0) return -1;
    return 0;
}

/* ── create / destroy ── */

ccev_poll_t *ccev__poll_create(int max_events) {
    struct ccev_poll_s *p = (struct ccev_poll_s *)
        ccev__realloc_fn(NULL, sizeof(struct ccev_poll_s));
    if (!p) return NULL;
    memset(p, 0, sizeof(struct ccev_poll_s));

    if (max_events <= 0) max_events = 64;
    p->max_events = max_events;
    p->kq          = -1;
    p->wakefds[0]  = p->wakefds[1] = (ccsocket_t)-1;

    p->kq = kqueue();
    if (p->kq < 0) goto fail;
    fcntl(p->kq, F_SETFD, FD_CLOEXEC);

    p->events = (struct kevent *)
        ccev__realloc_fn(NULL, (size_t)p->max_events * sizeof(struct kevent));
    if (!p->events) goto fail;

    /* Wake pipe */
    if (ccsocket_pipe(p->wakefds) < 0) goto fail;
    ccsocket_set_nonblock(p->wakefds[0], true);
    ccsocket_set_nonblock(p->wakefds[1], true);

    /* Register wake pipe read-end (level-triggered, no ONESHOT) */
    {
        struct kevent kev;
        EV_SET(&kev, (uintptr_t)p->wakefds[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
        if (kevent(p->kq, &kev, 1, NULL, 0, NULL) < 0) goto fail;
    }

    return p;

fail:;
    int saved_errno = errno;
    if (p->events)   ccev__free_fn(p->events);
    if (p->kq >= 0)  close(p->kq);
    if (p->wakefds[0] != (ccsocket_t)-1) ccsocket_close(p->wakefds[0]);
    if (p->wakefds[1] != (ccsocket_t)-1) ccsocket_close(p->wakefds[1]);
    ccev__free_fn(p);
    errno = saved_errno;
    return NULL;
}

void ccev__poll_destroy(ccev_poll_t *p) {
    if (!p) return;
    if (p->wakefds[0] != (ccsocket_t)-1) ccsocket_close(p->wakefds[0]);
    if (p->wakefds[1] != (ccsocket_t)-1) ccsocket_close(p->wakefds[1]);
    if (p->events) ccev__free_fn(p->events);
    if (p->kq >= 0) close(p->kq);
    ccev__free_fn(p);
}

/* ── add / mod / del ── */

int ccev__poll_add(ccev_poll_t *p, ccsocket_t fd, void *udata, int events) {
    /* EV_ADD: register; if already registered, kqueue treats it as a
     * modification (unlike epoll which returns EEXIST).  For safety we
     * also set EV_RECEIPT for error reporting, then check the result. */
    return _kevent_ctl(p, fd, udata, events, EV_ADD | EV_ONESHOT);
}

int ccev__poll_mod(ccev_poll_t *p, ccsocket_t fd, void *udata, int events) {
    /* On kqueue, EV_ADD already acts as ADD-or-MOD.  We use EV_ADD
     * here too so re-arming after ONESHOT consumption works correctly.
     * The EV_ONESHOT flag was consumed by the previous event delivery;
     * re-adding with EV_ONESHOT arms it for the next event. */
    return _kevent_ctl(p, fd, udata, events, EV_ADD | EV_ONESHOT);
}

int ccev__poll_del(ccev_poll_t *p, ccsocket_t fd) {
    /* Delete both filters unconditionally (EV_DELETE on a non-existent
     * filter is silently ignored by the kernel — no error). */
    struct kevent kev[2];
    int n = 0;
    EV_SET(&kev[n++], (uintptr_t)fd, EVFILT_READ,  EV_DELETE, 0, 0, NULL);
    EV_SET(&kev[n++], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(p->kq, kev, n, NULL, 0, NULL);
    return 0; /* EV_DELETE on a missing fd is not an error on kqueue */
}

/* ── wait ── */

static inline int _backend_wait(ccev_poll_t *p, int timeout_ms,
                                 ccev_poll_cb cb, void *arg)
{
    struct timespec ts;
    struct timespec *tsp = NULL;
    int n;
    uint64_t t0;

    if (timeout_ms > 0) {
        /* Finite timeout — retry EINTR with timeout decay */
        t0 = ccev__now_ms();
        for (;;) {
            ts.tv_sec  = timeout_ms / 1000;
            ts.tv_nsec = (timeout_ms % 1000) * 1000000;
            tsp = &ts;

            n = kevent(p->kq, NULL, 0, p->events, p->max_events, tsp);
            if (n >= 0 || errno != EINTR) break;

            int64_t elapsed = (int64_t)(ccev__now_ms() - t0);
            if (elapsed > 0) {
                timeout_ms -= (int)elapsed;
                if (timeout_ms <= 0) { n = 0; break; }
            }
            t0 = ccev__now_ms();
        }
    } else {
        /* Poll (0) or indefinite (-1) — set tsp once, bounded EINTR retry */
        if (timeout_ms == 0) {
            ts.tv_sec = ts.tv_nsec = 0;
            tsp = &ts;
        }
        int retries = 0;
        do {
            n = kevent(p->kq, NULL, 0, p->events, p->max_events, tsp);
        } while (n < 0 && errno == EINTR && ++retries < 10);
    }

    if (n < 0) return -1;

    int dispatched = 0;
    for (int i = 0; i < n; i++) {
        struct kevent *ev = &p->events[i];

        /* Wake pipe — drain silently */
        if (ev->udata == NULL) {
            char buf[64];
            while (ccsocket_recv(p->wakefds[0], buf, sizeof(buf), NULL) == CC_OPCODE_OK) {}
            continue;
        }

        struct ccev_poll_event pev;
        pev.udata  = ev->udata;
        pev.events = 0;

        if (ev->filter == EVFILT_READ)  pev.events |= CCEV_POLL_READ;
        if (ev->filter == EVFILT_WRITE) pev.events |= CCEV_POLL_WRITE;
        if (ev->flags  & EV_EOF)        pev.events |= CCEV_POLL_HUP;
        /* EV_ERROR sets ev->data to errno; treat as generic error */
        if (ev->flags  & EV_ERROR)      pev.events |= CCEV_POLL_ERR;

        cb(&pev, arg);
        dispatched++;
    }

    return dispatched;
}

#endif /* CCEV_POLL_BACKEND_* */

/* ════════════════════════════════════════════════════════════════
 *  ccev__poll_wait — dispatches to backend-specific _backend_wait
 * ════════════════════════════════════════════════════════════════ */

int ccev__poll_wait(ccev_poll_t *p, int timeout_ms,
                     ccev_poll_cb cb, void *arg)
{
    return _backend_wait(p, timeout_ms, cb, arg);
}

/* ════════════════════════════════════════════════════════════════
 *  ccev__poll_wake — common to all backends (write to internal pipe)
 * ════════════════════════════════════════════════════════════════ */

void ccev__poll_wake(ccev_poll_t *p) {
    if (!p) return;
    if (p->wakefds[1] == (ccsocket_t)-1) return;
    char c = 1;
    ccsocket_send(p->wakefds[1], &c, 1, NULL);
}
