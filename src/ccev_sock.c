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
 *  Compile-time assertions
 * ════════════════════════════════════════════════════════════════ */

/* C99-compatible static assertion (typedef cannot be negative). */
#define CCEV_STATIC_ASSERT(cond, tag) \
    typedef char ccev__static_assert_##tag[(cond) ? 1 : -1]

/* ccev_stream_t embeds ccev_sock_t as its first field; ccev_sock_create
 * allocates sizeof(ccev_stream_t) so the pointer can be safely cast to
 * ccev_stream_t later by ccev_stream_open().  Verify the layout. */
CCEV_STATIC_ASSERT(sizeof(ccev_sock_t) <= sizeof(ccev_stream_t),
    sock_fits_in_stream);
CCEV_STATIC_ASSERT(offsetof(ccev_stream_t, sock) == 0,
    stream_first_field_is_sock);

/* ccev_buf_t embeds cclink_node_t at offset 0 — verify container_of
 * via offsetof(ccev_buf_t, node) is used, which is always correct. */

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

    /* Allocate sizeof(ccev_stream_t) even for a plain sock so that
     * ccev_stream_open() can reinterpret the same pointer as a stream
     * without reallocating — ccev_stream_t embeds ccev_sock_t as its
     * first field.  Verified by CCEV_STATIC_ASSERT above. */
    ccev_sock_t *sock = (ccev_sock_t *)ccev__realloc_fn(NULL, sizeof(ccev_stream_t));
    if (!sock) return NULL;
    memset(sock, 0, sizeof(ccev_stream_t));

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

int ccev_sock_count(const ccev_loop_t *loop) {
    return loop ? loop->sock_count : 0;
}

/* ════════════════════════════════════════════════════════════════
 *  Listener
 * ════════════════════════════════════════════════════════════════ */

ccev_sock_t *ccev_listen(ccev_loop_t *loop, const char *addr, uint16_t port,
                           int backlog, ccev_flag_t flags,
                           ccev_listen_cb cb, void *udata) {
    if (!loop || !addr || !cb) return NULL;

    ccsocket_family_t family = ccsocket_get_version(addr);
    if (family == CC_FAMILY_INVALID) return NULL;
    if (family == CC_INET4 || family == CC_INET6) {
        if (addr[0] == '\0') { addr = "::"; family = CC_INET6; }
    }

    int proto = CC_TCP;
    ccsocket_t fd = ccsocket1(family, proto, CC_CLOEXEC | CC_NONBLOCK);
    if (fd == (ccsocket_t)-1) return NULL;

    if (family == CC_UNIX || !(flags & (CCEV_REUSEADDR | CCEV_REUSEPORT))) {
        if (!ccsocket_listen(fd, addr, port)) { ccsocket_close(fd); return NULL; }
    } else {
        if (!ccsocket_listen1(fd, addr, port)) { ccsocket_close(fd); return NULL; }
    }

    if ((flags & CCEV_ACCEPT_DEFER) && family != CC_UNIX)
        ccsocket_enable_accept_defer(fd);

    ccev_sock_t *sock = ccev_sock_create(loop, fd, udata);
    if (!sock) { ccsocket_close(fd); return NULL; }

    sock->mode           = CCEV_SOCK_LISTEN;
    sock->listener.cb    = cb;
    sock->listener.udata = udata;

    ccev__sock_mod_internal(loop, sock, EPOLLIN);
    return sock;
}

/* ════════════════════════════════════════════════════════════════
 *  Async connect (with internal DNS)
 * ════════════════════════════════════════════════════════════════ */

/*
 * DNS callback wrapper — prevents use-after-free when the user closes
 * a connecting sock while DNS is in flight.  The wrapper outlives the
 * sock; ccev__sock_free marks it dead via dns_cancelled.
 */
typedef struct {
    ccev_sock_t *sock;
    bool         alive;
} _connect_dns_ctx_t;

/* ── Connect timeout callback ── */
static void _connect_timeout_cb(void *udata) {
    ccev_sock_t *sock = (ccev_sock_t *)udata;
    if (sock->mode != CCEV_SOCK_CONNECT) return;
    sock->connector.timer = NULL;

    if (sock->connector.cb)
        sock->connector.cb(sock->connector.udata, sock, CCEV_ERR);
    ccev__sock_schedule_close(sock->loop, sock);
}

/* ── Create socket, initiate non-blocking connect, register with reactor ── */
/* Returns:  1 = connected immediately (cb already fired)
 *           0 = connect pending (EPOLLOUT will fire cb later)
 *          -1 = fd creation failed
 *
 * Connection state is checked via ccsocket_is_connected(), NOT the return
 * value of ccsocket_connect() — on non-blocking sockets the connect call
 * may return false for EINPROGRESS, not just errors. */
static int _connect_try_register(ccev_sock_t *sock,
                                  ccsocket_family_t family,
                                  const char *address, uint16_t port) {
    ccsocket_t fd = ccsocket1(family, CC_TCP, CC_CLOEXEC | CC_NONBLOCK);
    if (fd == (ccsocket_t)-1) return -1;

    /* Initiate non-blocking connect — discard return value (EINPROGRESS is
     * not an error for non-blocking sockets).  Use ccsocket_is_connected to
     * check actual connection state. */
    ccsocket_connect(fd, address, port);

    if (ccsocket_is_connected(fd) == CC_CONNECTED) {
        sock->fd   = fd;
        sock->mode = CCEV_SOCK_INIT;
        if (sock->connector.timer) {
            ccev_timer_del(sock->loop, sock->connector.timer);
            sock->connector.timer = NULL;
        }
        if (sock->connector.cb)
            sock->connector.cb(sock->connector.udata, sock, CCEV_OK);
        return 1;
    }

    /* Still connecting or error — register EPOLLOUT; the dispatch handler
     * in ccev_loop_run calls ccsocket_is_connected to distinguish success
     * from error. */
    sock->fd = fd;
    ccev__sock_mod_internal(sock->loop, sock, EPOLLOUT);
    return 0;
}

/* ── Resume connect after DNS resolution ── */
static void _connect_dns_cb(void *udata, const char *address, int status) {
    _connect_dns_ctx_t *ctx = (_connect_dns_ctx_t *)udata;
    if (!ctx->alive) { ccev__free_fn(ctx); return; }

    ccev_sock_t *sock = ctx->sock;
    sock->connector.dns_cancelled = NULL;
    ccev__free_fn(ctx);
    if (status != CCEV_OK || !address || address[0] == '\0') {
        if (sock->connector.cb)
            sock->connector.cb(sock->connector.udata, sock, CCEV_ERR);
        ccev__sock_schedule_close(sock->loop, sock);
        return;
    }

    ccsocket_family_t family = ccsocket_get_version(address);
    if (family == CC_FAMILY_INVALID) {
        if (sock->connector.cb)
            sock->connector.cb(sock->connector.udata, sock, CCEV_ERR);
        ccev__sock_schedule_close(sock->loop, sock);
        return;
    }

    int rc = _connect_try_register(sock, family, address, sock->connector.port);
    if (rc < 0) {
        if (sock->connector.cb)
            sock->connector.cb(sock->connector.udata, sock, CCEV_ERR);
        ccev__sock_schedule_close(sock->loop, sock);
        return;
    }
}

/* ── Public connect API ── */

ccev_sock_t *ccev_connect(ccev_loop_t *loop, const char *host, uint16_t port,
                            unsigned int timeout_ms, ccev_flag_t flags,
                            ccev_connect_cb cb, void *udata) {
    if (!loop || !host || !cb) return NULL;

    ccev_sock_t *sock = (ccev_sock_t *)ccev__realloc_fn(NULL, sizeof(ccev_stream_t));
    if (!sock) return NULL;
    memset(sock, 0, sizeof(ccev_stream_t));

    sock->loop   = loop;
    sock->fd     = (ccsocket_t)-1;
    sock->mode   = CCEV_SOCK_CONNECT;
    sock->udata  = udata;

    size_t hlen = strlen(host);
    if (hlen >= sizeof(sock->connector.host))
        hlen = sizeof(sock->connector.host) - 1;
    memcpy(sock->connector.host, host, hlen);
    sock->connector.host[hlen] = '\0';
    sock->connector.port      = port;
    sock->connector.timeout_ms = timeout_ms;
    sock->connector.cb        = cb;
    sock->connector.udata     = udata;

    cclist_push_back(&loop->all_socks, &sock->lnode);
    loop->sock_count++;

    if (timeout_ms > 0) {
        sock->connector.timer = ccev_timer_add(loop, timeout_ms,
                                                 CCEV_TIMER_ONCE,
                                                 _connect_timeout_cb, sock);
    }

    ccsocket_family_t family = ccsocket_get_version(host);
    if (family == CC_INET4 || family == CC_INET6 || family == CC_UNIX) {
        int rc = _connect_try_register(sock, family, host, port);
        if (rc < 0) {
            if (sock->connector.timer)
                ccev_timer_del(loop, sock->connector.timer);
            cclist_remove(&loop->all_socks, &sock->lnode);
            loop->sock_count--;
            ccev__free_fn(sock);
            return NULL;
        }
        if (rc > 0) return sock;  /* connected immediately — cb already fired */
        /* rc == 0: pending — will fire cb via EPOLLOUT */
    } else {
        /* When timeout_ms is 0 (no timeout), pass 0 through to DNS.
         * Otherwise clamp to [1000, 10000] so a single-digit timeout
         * has a reasonable chance to complete a DNS exchange. */
        unsigned int dns_timeout = timeout_ms;
        if (dns_timeout > 0) {
            if (dns_timeout > 10000) dns_timeout = 10000;
            if (dns_timeout < 1000)  dns_timeout = 1000;
        }

        _connect_dns_ctx_t *dns_ctx = (_connect_dns_ctx_t *)
            ccev__realloc_fn(NULL, sizeof(_connect_dns_ctx_t));
        if (!dns_ctx) {
            if (sock->connector.timer)
                ccev_timer_del(loop, sock->connector.timer);
            cclist_remove(&loop->all_socks, &sock->lnode);
            loop->sock_count--;
            ccev__free_fn(sock);
            return NULL;
        }
        dns_ctx->sock     = sock;
        dns_ctx->alive    = true;
        sock->connector.dns_cancelled = &dns_ctx->alive;

        if (ccev_dns_resolve(loop, host, dns_timeout,
                              CCEV_DNS_A | CCEV_DNS_AAAA,
                              _connect_dns_cb, dns_ctx) != CCEV_OK) {
            sock->connector.dns_cancelled = NULL;
            ccev__free_fn(dns_ctx);
            if (sock->connector.timer)
                ccev_timer_del(loop, sock->connector.timer);
            cclist_remove(&loop->all_socks, &sock->lnode);
            loop->sock_count--;
            ccev__free_fn(sock);
            return NULL;
        }
    }

    return sock;
}
