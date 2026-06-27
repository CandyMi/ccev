/**
 * @file ccev_sock.c
 * @brief Socket lifecycle — epoll registration, close queue, alloc/free.
 *
 * @author CandyMi
 * @license MIT
 *
 * ONESHOT compensation:
 *   When EPOLLONESHOT fires, the event is consumed.  After dispatch,
 *   ccev__sock_rearm() re-registers wanted events based on sock->events.
 *
 * Deferred close:
 *   ccev__sock_schedule_close() moves a sock to the closing list.
 *   The actual close_cb invocation and memory release happen after the
 *   dispatch loop finishes, preventing use-after-free in callbacks.
 */

#include "ccev_internal.h"

/* ════════════════════════════════════════════════════════════════
 *  Internal helpers
 * ════════════════════════════════════════════════════════════════ */

/* Convert CCEV_EVENT flags to epoll event mask.
 * CCEV_EVENT_READ/WRITE happen to match EPOLLIN/EPOLLOUT on all backends
 * (see uepoll.h / sys/epoll.h), so we can mask directly. */
static inline int _events_to_epoll(uint32_t events) {
    int e = 0;
    if (events & CCEV_EVENT_READ)  e |= EPOLLIN;
    if (events & CCEV_EVENT_WRITE) e |= EPOLLOUT;
    return e;
}

int ccev__sock_mod_internal(ccev_loop_t *loop, ccev_sock_t *sock,
                             int epoll_events) {
    if (sock->closed) return CCEV_ERR;

    struct epoll_event ee;
    memset(&ee, 0, sizeof(ee));
    ee.events   = epoll_events | EPOLLONESHOT;
    ee.data.ptr = sock;

    int op = (sock->events == 0) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    if (epoll_ctl(loop->epfd, op, (int)(intptr_t)sock->fd, &ee) < 0) {
        if (op == EPOLL_CTL_ADD) {
            op = EPOLL_CTL_MOD;
            if (epoll_ctl(loop->epfd, op, (int)(intptr_t)sock->fd, &ee) < 0)
                return CCEV_ERR;
        } else {
            return CCEV_ERR;
        }
    }
    sock->events = (uint32_t)epoll_events;
    return CCEV_OK;
}

void ccev__sock_rearm(ccev_loop_t *loop, ccev_sock_t *sock) {
    if (sock->closed) return;

    /* Compute desired epoll events from active callbacks.
     * This is simpler than tracking want_events separately — the
     * presence of rcb/wcb indicates interest in read/write. */
    int want = 0;
    if (sock->rcb) want |= EPOLLIN;
    if (sock->wcb) want |= EPOLLOUT;

    if (want && want != (int)sock->events) {
        ccev__sock_mod_internal(loop, sock, want);
    }
}

/* ════════════════════════════════════════════════════════════════
 *  Closing lifecycle
 * ════════════════════════════════════════════════════════════════ */

void ccev__sock_free(ccev_sock_t *sock) {
    if (!sock) return;
    if (sock->loop) sock->loop->sock_count--;

    /* Close the socket fd */
    if (sock->fd != (ccsocket_t)-1) ccsocket_close(sock->fd);
    sock->fd = (ccsocket_t)-1;

    /* Cancel connect timer and mark DNS as dead if still pending */
    if (sock->mode == CCEV_SOCK_CONNECT) {
        if (sock->connector.timer) {
            ccev_timer_del(sock->loop, sock->connector.timer);
            sock->connector.timer = NULL;
        }
        if (sock->connector.dns_cancelled) {
            *sock->connector.dns_cancelled = false;
        }
    }

    ccev__free_fn(sock);
}

void ccev__sock_schedule_close(ccev_loop_t *loop, ccev_sock_t *sock) {
    if (sock->closed || sock->in_closing) return;
    sock->closed     = true;
    sock->in_closing = true;

    /* Remove from epoll */
    if (sock->fd != (ccsocket_t)-1) {
        epoll_ctl(loop->epfd, EPOLL_CTL_DEL, (int)(intptr_t)sock->fd, NULL);
    }
    sock->events = 0;

    /* Remove from all_socks first, then push to closing list */
    cclist_remove(&loop->all_socks, &sock->lnode);
    cclist_push_back(&loop->closing, &sock->lnode);
}

/* ════════════════════════════════════════════════════════════════
 *  Process closing queue (called from ccev_loop_run)
 * ════════════════════════════════════════════════════════════════ */

void ccev__process_closing(ccev_loop_t *loop) {
    while (!cclist_empty(&loop->closing)) {
        cclist_node_t *cn = cclist_pop_front(&loop->closing);
        if (!cn) continue;
        ccev_sock_t *sock = (ccev_sock_t *)((char*)cn - offsetof(ccev_sock_t, lnode));
        sock->in_closing = false;
        if (sock->close_cb) sock->close_cb(sock->close_udata);
        ccev__sock_free(sock);
    }
}

/* ════════════════════════════════════════════════════════════════
 *  Public API
 * ════════════════════════════════════════════════════════════════ */

ccev_sock_t *ccev_sock_create(ccev_loop_t *loop, ccsocket_t fd, void *udata) {
    if (!loop || fd == (ccsocket_t)-1) return NULL;

    ccev_sock_t *sock = (ccev_sock_t *)ccev__realloc_fn(NULL, sizeof(ccev_sock_t));
    if (!sock) return NULL;
    memset(sock, 0, sizeof(ccev_sock_t));

    sock->loop   = loop;
    sock->fd     = fd;
    sock->udata  = udata;
    sock->mode   = CCEV_SOCK_INIT;

    ccsocket_set_nonblock(fd, true);
    ccsocket_set_cloexec(fd, true);

    /* Add to all_socks list */
    cclist_push_back(&loop->all_socks, &sock->lnode);
    loop->sock_count++;

    return sock;
}

int ccev_sock_close(ccev_sock_t *sock) {
    if (!sock || sock->closed) return CCEV_ERR;
    ccev__sock_schedule_close(sock->loop, sock);
    return CCEV_OK;
}

/* ── Read event management ── */

int ccev_sock_read_start(ccev_sock_t *sock, ccev_event_cb cb) {
    if (!sock || sock->closed) return CCEV_ERR;
    if (cb) sock->rcb = cb;
    ccev__sock_rearm(sock->loop, sock);
    return CCEV_OK;
}

int ccev_sock_read_stop(ccev_sock_t *sock) {
    if (!sock || sock->closed) return CCEV_ERR;
    sock->rcb = NULL;
    ccev__sock_rearm(sock->loop, sock);
    return CCEV_OK;
}

/* ── Write event management ── */

int ccev_sock_write_start(ccev_sock_t *sock, ccev_event_cb cb) {
    if (!sock || sock->closed) return CCEV_ERR;
    if (cb) sock->wcb = cb;
    ccev__sock_rearm(sock->loop, sock);
    return CCEV_OK;
}

int ccev_sock_write_stop(ccev_sock_t *sock) {
    if (!sock || sock->closed) return CCEV_ERR;
    sock->wcb = NULL;
    ccev__sock_rearm(sock->loop, sock);
    return CCEV_OK;
}

/* ── Close callback ── */

void ccev_sock_set_close_cb(ccev_sock_t *sock, ccev_close_cb cb, void *udata) {
    if (!sock) return;
    sock->close_cb   = cb;
    sock->close_udata = udata;
}

/* ── Metadata ── */

ccsocket_t ccev_sock_get_fd(const ccev_sock_t *sock) {
    return sock ? sock->fd : (ccsocket_t)-1;
}

void *ccev_sock_get_udata(const ccev_sock_t *sock) {
    return sock ? sock->udata : NULL;
}

void ccev_sock_set_udata(ccev_sock_t *sock, void *udata) {
    if (sock) sock->udata = udata;
}

int ccev_sock_count(ccev_loop_t *loop) {
    return loop ? loop->sock_count : 0;
}
