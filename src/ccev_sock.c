/**
 * @file ccev_sock.c
 * @brief Socket lifecycle — poll registration, close queue, alloc/free.
 *
 * @author CandyMi
 * @license MIT
 *
 * ONESHOT semantics:
 *   The poll layer uses ONESHOT internally — every event fires once and
 *   must be re-armed.  ccev__sock_rearm() re-registers wanted events
 *   based on sock->events after dispatch.
 *
 * Deferred close:
 *   ccev__sock_schedule_close() moves a sock to the closing list.
 *   The actual close_cb invocation and memory release happen after the
 *   dispatch loop finishes, preventing use-after-free in callbacks.
 */

#include "ccev_internal.h"

#ifdef _WIN32
#  include <io.h>
#  define unlink _unlink
#endif

/* ════════════════════════════════════════════════════════════════
 *  Internal helpers
 * ════════════════════════════════════════════════════════════════ */

int ccev__sock_mod_internal(ccev_loop_t *loop, ccev_sock_t *sock,
                             int poll_events) {
    if (sock->closed) return CCEV_ERR;

    int rc;
    if (sock->events == 0)
        rc = ccev__poll_add(loop->poll, sock->fd, sock, poll_events);
    else
        rc = ccev__poll_mod(loop->poll, sock->fd, sock, poll_events);
    if (rc < 0) return CCEV_ERR;

    sock->events = (uint32_t)poll_events;
    return CCEV_OK;
}

void ccev__sock_rearm(ccev_loop_t *loop, ccev_sock_t *sock) {
    if (sock->closed) return;

    /* Compute desired poll events from active callbacks.
     * This is simpler than tracking want_events separately — the
     * presence of rcb/wcb indicates interest in read/write. */
    int want = 0;
    if (sock->rcb) want |= CCEV_POLL_READ;
    if (sock->wcb) want |= CCEV_POLL_WRITE;

    if (want && want != (int)sock->events) {
        ccev__sock_mod_internal(loop, sock, want);
    }
}

/* ════════════════════════════════════════════════════════════════
 *  Closing lifecycle
 * ════════════════════════════════════════════════════════════════ */

void ccev__sock_free(ccev_sock_t *sock) {
    if (!sock) return;

    /* UDS listener: remove socket file from filesystem on close */
    if (sock->mode == CCEV_SOCK_LISTEN) {
        ccev_sock_any_t *any = (ccev_sock_any_t *)sock;
        if (any->listener.uds_path[0] != '\0')
            unlink(any->listener.uds_path);
    }

    if (sock->upgraded)
        ccev__stream_cleanup((ccev_stream_t *)sock);

    if (sock->loop) sock->loop->sock_count--;
    if (sock->fd != (ccsocket_t)-1) ccsocket_close(sock->fd);
    sock->fd = (ccsocket_t)-1;
    ccev__free_fn(sock);
}

void ccev__sock_schedule_close(ccev_loop_t *loop, ccev_sock_t *sock) {
    if (sock->closed || sock->in_closing) return;

    /* Delete connect timer for CONNECT-mode sockets — prevents
     * double-callback when a connect timeout fires after an error or
     * user-initiated close has already terminated the connection. */
    if (sock->mode == CCEV_SOCK_CONNECT) {
        ccev_sock_any_t *any = (ccev_sock_any_t *)sock;
        if (any->connector.timer) {
            ccev_timer_del(loop, any->connector.timer);
            any->connector.timer = NULL;
        }
    }

    sock->closed     = true;
    sock->in_closing = true;

    /* Remove from poll */
    if (sock->fd != (ccsocket_t)-1) {
        ccev__poll_del(loop->poll, sock->fd);
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

    /* Allocate via ccev_sock_any_t union so the same pointer can later
     * be reinterpreted as a ccev_stream_t (or future variants) without
     * reallocating.  All variants share ccev_sock_t as their first field. */
    ccev_sock_any_t *any = (ccev_sock_any_t *)ccev__realloc_fn(NULL, sizeof(ccev_sock_any_t));
    if (!any) return NULL;
    memset(any, 0, sizeof(ccev_sock_any_t));
    any->stream.sendfile_fd = -1;

    ccev_sock_t *sock = &any->sock;

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

    /* Detect address family — for UDS paths that don't exist yet,
     * ccsocket_get_version cannot stat() them, so we fall back to
     * heuristic: '/' indicates a file-system UDS path, '@' is a
     * portable prefix that strips to a relative UDS path. */
    ccsocket_family_t family = ccsocket_get_version(addr);
    if (family == CC_FAMILY_INVALID) {
        if (addr[0] == '/' || addr[0] == '@')
            family = CC_UNIX;
    }
    if (family == CC_FAMILY_INVALID) return NULL;

    /* '@' prefix: strip to use the rest as a relative UDS path.
     * Must happen before the addr ref for the wildcard rewrite below. */
    if (addr[0] == '@') addr++;

    if (family == CC_INET4 || family == CC_INET6) {
        if (addr[0] == '\0') { addr = "::"; family = CC_INET6; }
    }

    /* UDS requires port == 0 */
    if (family == CC_UNIX && port != 0) return NULL;

    int proto = CC_TCP;
    ccsocket_t fd = ccsocket1(family, proto, CC_CLOEXEC | CC_NONBLOCK);
    if (fd == (ccsocket_t)-1) return NULL;

    /* UDS: unlink stale socket file before bind to avoid EADDRINUSE.
     * Only unlink paths that already exist AS a socket file — root
     * privileges mean unlink("/etc/passwd") would succeed, so a stat()
     * S_ISSOCK guard prevents deleting non-socket files.  ENOENT is
     * harmless (unlink just fails).  Directory protection comes from
     * OS: unlink(2) refuses directories with EPERM/EISDIR. */
    if (family == CC_UNIX) {
        struct stat _st;
        if (stat(addr, &_st) == 0 && S_ISSOCK(_st.st_mode))
            unlink(addr);
    }

    if (family == CC_UNIX || !(flags & (CCEV_REUSEADDR | CCEV_REUSEPORT))) {
        if (!ccsocket_listen(fd, addr, port, backlog)) {
            if (family == CC_UNIX) unlink(addr);
            ccsocket_close(fd); return NULL;
        }
    } else {
        if (!ccsocket_listen1(fd, addr, port, backlog)) { ccsocket_close(fd); return NULL; }
    }

    if ((flags & CCEV_ACCEPT_DEFER) && family != CC_UNIX)
        ccsocket_enable_accept_defer(fd);

    ccev_sock_t *sock = ccev_sock_create(loop, fd, udata);
    if (!sock) {
        if (family == CC_UNIX) unlink(addr);
        ccsocket_close(fd); return NULL;
    }

    ccev_sock_any_t *any  = (ccev_sock_any_t *)sock;
    any->sock.mode        = CCEV_SOCK_LISTEN;
    any->listener.cb      = cb;
    any->listener.udata   = udata;

    /* Save UDS path so ccev__sock_free can unlink on close */
    if (family == CC_UNIX) {
        size_t _len = strlen(addr);
        if (_len >= sizeof(any->listener.uds_path)) {
            ccev__sock_schedule_close(loop, sock);
            return NULL;
        }
        memcpy(any->listener.uds_path, addr, _len + 1);
    }

    ccev__sock_mod_internal(loop, sock, CCEV_POLL_READ);
    return sock;
}

/* ════════════════════════════════════════════════════════════════
 *  Async connect (with internal DNS)
 * ════════════════════════════════════════════════════════════════ */

/* ── Connector close callback — cancels in-flight DNS resolution ── */
static void _connector_close_cb(void *udata) {
    ccev_sock_any_t *any = (ccev_sock_any_t *)udata;
    any->connector.dns_alive = false;
}

/* ── Connect timeout callback ── */
static void _connect_timeout_cb(ccev_timer_t *timer, void *udata) {
    ccev_sock_any_t *any = (ccev_sock_any_t *)udata;
    if (any->sock.mode != CCEV_SOCK_CONNECT) return;
    any->connector.timer = NULL;

    /* Cancel in-flight DNS — _connect_dns_cb will see dns_alive == false
     * and return immediately without touching the dying sock.  The DNS
     * response can arrive in the same loop iteration (timer fires first,
     * then epoll dispatches the DNS recv callback), so we must set the
     * flag here rather than relying on deferred close. */
    any->connector.dns_alive = false;

    if (any->connector.cb)
        any->connector.cb(any->connector.udata, &any->sock, CCEV_ERR);
    ccev__sock_schedule_close(any->sock.loop, &any->sock);
}

/* ── Create socket, initiate non-blocking connect, register with reactor ── */
/* Returns:  0 = connect pending (EPOLLOUT will fire cb later)
 *          -1 = fd creation failed
 *
 * Connection result is always delivered asynchronously via EPOLLOUT
 * dispatch — the event loop's connect-completion handler in ccev.c
 * calls ccsocket_is_connected() to distinguish success from error,
 * avoiding any synchronous probe that races with async connect(2). */
static int _connect_try_register(ccev_sock_t *sock,
                                  ccsocket_family_t family,
                                  const char *address, uint16_t port) {
    ccsocket_t fd = ccsocket1(family, CC_TCP, CC_CLOEXEC | CC_NONBLOCK);
    if (fd == (ccsocket_t)-1) return -1;

    /* Apply TCP_NODELAY if requested — TCP only, UDS ignores silently */
    if ((family == CC_INET4 || family == CC_INET6) &&
        (((ccev_sock_any_t *)sock)->connector.flags & CCEV_TCP_NODELAY))
        ccsocket_set_nodelay(fd, true);

    /* Initiate non-blocking connect, then defer completion detection
     * to the event loop via EPOLLOUT.  EINPROGRESS is not an error for
     * non-blocking sockets — the dispatch handler probes state with
     * ccsocket_is_connected(). */
    ccsocket_connect(fd, address, port);

    sock->fd = fd;
    ccev__sock_mod_internal(sock->loop, sock, CCEV_POLL_WRITE);
    return 0;
}

/* ── Resume connect after DNS resolution ── */
static void _connect_dns_cb(void *udata, const char *address, int status) {
    ccev_sock_any_t *any = (ccev_sock_any_t *)udata;
    if (!any->connector.dns_alive) return;

    ccev_sock_t *sock = &any->sock;
    any->connector.dns_alive = false;
    if (status != CCEV_OK || !address || address[0] == '\0')
        goto err;

    ccsocket_family_t family = ccsocket_get_version(address);
    if (family == CC_FAMILY_INVALID)
        goto err;

    int rc = _connect_try_register(sock, family, address, any->connector.port);
    if (rc < 0)
        goto err;
    return;

err:
    if (any->connector.timer) {
        ccev_timer_del(sock->loop, any->connector.timer);
        any->connector.timer = NULL;
    }
    if (any->connector.cb)
        any->connector.cb(any->connector.udata, sock, CCEV_ERR);
    ccev__sock_schedule_close(sock->loop, sock);
}

/* ── Public connect API ── */

ccev_sock_t *ccev_connect(ccev_loop_t *loop, const char *host, uint16_t port,
                            unsigned int timeout_ms, ccev_flag_t flags,
                            ccev_connect_cb cb, void *udata) {
    if (!loop || !host || !cb) return NULL;

    /* Detect address family before allocating the socket union.
     * This avoids wasted allocation on invalid input. */
    bool host_is_at_uds = (host[0] == '@');
    const char *connect_addr = host;
    if (host_is_at_uds) connect_addr++;

    ccsocket_family_t family = ccsocket_get_version(connect_addr);
    if (family == CC_FAMILY_INVALID) {
        /* Check if the path exists as a socket file.  This catches UDS
         * paths on platforms where ccsocket_get_version skips stat()
         * (Windows), and verifies existence before heuristic assignment. */
        struct stat _st;
        if (stat(connect_addr, &_st) == 0 && S_ISSOCK(_st.st_mode))
            family = CC_UNIX;
        else if (connect_addr[0] == '/' || host_is_at_uds)
            family = CC_UNIX;
        /* else: remains CC_FAMILY_INVALID → hostname, falls through to DNS */
    }
    /* Now safe to allocate */
    ccev_sock_any_t *any = (ccev_sock_any_t *)ccev__realloc_fn(NULL, sizeof(ccev_sock_any_t));
    if (!any) return NULL;
    memset(any, 0, sizeof(ccev_sock_any_t));
    any->stream.sendfile_fd = -1;

    ccev_sock_t *sock = &any->sock;
    sock->loop   = loop;
    sock->fd     = (ccsocket_t)-1;
    sock->mode   = CCEV_SOCK_CONNECT;
    sock->udata  = udata;

    any->connector.port      = port;
    any->connector.cb        = cb;
    any->connector.udata     = udata;
    any->connector.flags     = flags;

    cclist_push_back(&loop->all_socks, &sock->lnode);
    loop->sock_count++;

    if (timeout_ms > 0) {
        any->connector.timer = ccev_timer_add(loop, timeout_ms,
                                                CCEV_TIMER_ONCE,
                                                _connect_timeout_cb, sock);
    }

    if (family == CC_INET4 || family == CC_INET6 || family == CC_UNIX) {
        int rc = _connect_try_register(sock, family, connect_addr, port);
        if (rc < 0) {
            if (any->connector.timer)
                ccev_timer_del(loop, any->connector.timer);
            cclist_remove(&loop->all_socks, &sock->lnode);
            loop->sock_count--;
            ccev__free_fn(any);
            return NULL;
        }
        /* rc == 0: connect initiated — result delivered via EPOLLOUT callback */
    } else {
        /* When timeout_ms is 0 (no timeout), pass 0 through to DNS.
         * Otherwise clamp to [1000, 10000] so a single-digit timeout
         * has a reasonable chance to complete a DNS exchange. */
        unsigned int dns_timeout = timeout_ms;
        if (dns_timeout > 0) {
            if (dns_timeout > 10000) dns_timeout = 10000;
            if (dns_timeout < 1000)  dns_timeout = 1000;
        }

        any->connector.dns_alive = true;

        /* Register close callback so ccev_sock_close() before DNS
         * resolves marks dns_alive = false, preventing _connect_dns_cb
         * from acting on a dying sock. */
        ccev_sock_set_close_cb(sock, _connector_close_cb, any);

        if (ccev_dns_resolve(loop, host, dns_timeout,
                              CCEV_DNS_A | CCEV_DNS_AAAA,
                              _connect_dns_cb, any) != CCEV_OK) {
            any->connector.dns_alive = false;
            if (any->connector.timer)
                ccev_timer_del(loop, any->connector.timer);
            cclist_remove(&loop->all_socks, &sock->lnode);
            loop->sock_count--;
            ccev__free_fn(any);
            return NULL;
        }
    }

    return sock;
}
