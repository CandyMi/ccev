/**
 * @file ccev.c
 * @brief Reactor core — event-loop lifecycle, listen, connect, dispatch.
 *
 * ONESHOT compensation:
 *   When EPOLLONESHOT fires for EPOLLOUT but the user also registered
 *   EPOLLIN, the EPOLLIN event is consumed by ONESHOT. After each
 *   dispatch iteration, ccev__conn_rearm() re-registers wanted events
 *   for every active connection.
 *
 * Deferred close:
 *   ccev__conn_schedule_close() moves a connection to the closing list.
 *   The actual close_cb invocation and memory release happen after the
 *   dispatch loop finishes, preventing use-after-free in callbacks.
 */

#include "ccev_internal.h"
#include <string.h>

/* Default allocator (libc) */
static void *ccev_default_realloc(void *ptr, size_t sz) {
    if (sz == 0) { free(ptr); return NULL; }
    return realloc(ptr, sz);
}
static void ccev_default_free(void *ptr) { free(ptr); }

static void *(*g_realloc_fn)(void*, size_t) = ccev_default_realloc;
static void  (*g_free_fn)(void*)            = ccev_default_free;

void ccev_set_allocator(void *(*realloc_fn)(void*, size_t),
                        void  (*free_fn)(void*)) {
    if (realloc_fn) g_realloc_fn = realloc_fn;
    if (free_fn)    g_free_fn    = free_fn;
}

/* ════════════════════════════════════════════════════════════════
 *  Loop lifecycle
 * ════════════════════════════════════════════════════════════════ */

ccev_loop_t *ccev_loop_create(int max_events) {
    ccev_loop_t *loop = (ccev_loop_t *)g_realloc_fn(NULL, sizeof(ccev_loop_t));
    if (!loop) return NULL;

    memset(loop, 0, sizeof(ccev_loop_t));
    loop->realloc_fn = g_realloc_fn;
    loop->free_fn    = g_free_fn;
    loop->max_events = max_events > 0 ? max_events : 64;

    loop->epfd = epoll_create(1);
    if ((intptr_t)loop->epfd < 0) { g_free_fn(loop); return NULL; }

    loop->events = (struct epoll_event *)loop->realloc_fn(
        NULL, (size_t)loop->max_events * sizeof(struct epoll_event));
    if (!loop->events) { epoll_close(loop->epfd); g_free_fn(loop); return NULL; }

    /* Init data structures */
    cclist_init(&loop->all_conns);
    cclist_init(&loop->closing);
    ccheap_init(&loop->timers, NULL);

    /* Init DNS state */
    loop->dns.servers[0] = "1.1.1.1";
    loop->dns.nservers   = 1;
    loop->dns.port       = 53;

    /* Wakeup pipe */
    if (ccsocket_pipe(loop->wakefds) < 0) {
        loop->free_fn(loop->events);
        epoll_close(loop->epfd);
        g_free_fn(loop);
        return NULL;
    }

    loop->wake_conn = NULL;
    ccev_conn_t *wake = ccev_conn_create(loop, loop->wakefds[0], NULL);
    if (wake) {
        loop->wake_conn = wake;
        ccev__conn_mod_internal(loop, wake, EPOLLIN);
    }

    /* Linux timerfd */
#if defined(__linux__) || defined(__ANDROID__)
    loop->timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (loop->timerfd >= 0) {
        ccev_conn_t *tc = ccev_conn_create(loop, (ccsocket_t)(intptr_t)loop->timerfd, NULL);
        if (tc) ccev__conn_mod_internal(loop, tc, EPOLLIN);
    }
#endif

    return loop;
}

void ccev_loop_destroy(ccev_loop_t *loop) {
    if (!loop) return;

    /* Let the all_conns loop handle all fd and conn freeing */

#if defined(__linux__) || defined(__ANDROID__)
    if (loop->timerfd >= 0) close(loop->timerfd);
#endif

    /* Free all connections via the all_conns list */
    while (!cclist_empty(&loop->all_conns)) {
        cclist_node_t *n = cclist_pop_front(&loop->all_conns);
        if (n) {
            ccev_conn_t *conn = (ccev_conn_t *)((char*)n - offsetof(ccev_conn_t, lnode));
            ccev__conn_free(loop, conn);
        }
    }

    /* Free all timers */
    while (1) {
        ccheap_node_t *min = ccheap_peek(&loop->timers);
        if (!min) break;
        ccheap_pop(&loop->timers);
        ccev_timer_t *t = CCHEAP_CONTAINER(min, ccev_timer_t, node);
        loop->free_fn(t);
    }

    /* Free timer heap internal array */
    ccheap_destroy(&loop->timers);

    loop->free_fn(loop->events);
    epoll_close(loop->epfd);
    loop->free_fn(loop);
}

void ccev_loop_stop(ccev_loop_t *loop) {
    if (!loop) return;
    loop->stop_flag = true;
    ccev_wakeup(loop);
}

/* ════════════════════════════════════════════════════════════════
 *  Main event loop
 * ════════════════════════════════════════════════════════════════ */

int ccev_loop_run(ccev_loop_t *loop, ccev_run_mode_t mode) {
    if (!loop) return CCEV_ERR;
    loop->stop_flag = false;
    int n = 0;

    do {
        uint64_t now = ccev__now_ms();

        /* 1. Process expired timers */
        ccev__timer_process(loop, now);
        if (loop->stop_flag) break;

        /* 2. Compute epoll timeout */
        int timeout = (mode == CCEV_RUN_ONCE) ? 0 : -1;
        if (mode == CCEV_RUN_FOREVER) {
            ccheap_node_t *earliest = ccheap_peek(&loop->timers);
            if (earliest) {
                ccev_timer_t *t = CCHEAP_CONTAINER(earliest, ccev_timer_t, node);
                if (t->expiry > now) {
                    uint64_t diff = t->expiry - now;
                    timeout = (diff > (uint64_t)INT_MAX) ? -1 : (int)diff;
                } else {
                    timeout = 0;
                }
            }
        }

        /* 3. epoll_wait */
        n = epoll_wait(loop->epfd, loop->events, loop->max_events, timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* 4. Dispatch events */
        for (int i = 0; i < n; i++) {
            struct epoll_event *ev = &loop->events[i];
            ccev_conn_t *conn = (ccev_conn_t *)ev->data.ptr;
            if (!conn || conn->closed) continue;

            uint32_t fired = ev->events;

            /* Wakeup pipe */
            if (conn == loop->wake_conn) {
                char buf[64];
                while (ccsocket_recv(conn->fd, buf, sizeof(buf), NULL) == CC_OPCODE_OK) {}
                ccev__conn_mod_internal(loop, conn, EPOLLIN);
                continue;
            }

            /* Error / hangup */
            if (fired & (EPOLLERR | EPOLLHUP)) {
                if (conn->close_cb) conn->close_cb(conn->close_udata);
                ccev__conn_schedule_close(loop, conn);
                continue;
            }

            bool fired_read  = (fired & EPOLLIN) != 0;
            bool fired_write = (fired & EPOLLOUT) != 0;

            /* ONESHOT consumed the event — reset so re-arm fires */
            conn->reg_events = 0;

            if (fired_read) {
                /* Listener */
                if (conn->type == CCEV_CONN_LISTENER) {
                    ccsocket_t afd;
                    char addr_buf[64];
                    uint16_t port = 0;
                    afd = ccsocket_accept2(conn->fd, addr_buf, &port, 0);
                    if (afd != (ccsocket_t)0 && afd != (ccsocket_t)-1
                        && conn->listener.cb) {
                        ccev_conn_t *new_conn = ccev_conn_create(loop, afd, NULL);
                        if (new_conn) {
                            conn->listener.cb(conn->listener.udata,
                                               new_conn, addr_buf, (int)port);
                        } else {
                            ccsocket_close(afd);
                        }
                    }
                    ccev__conn_mod_internal(loop, conn, EPOLLIN);
                    continue;
                }

                /* DNS + normal recv: fire callback then re-arm */
                if (conn->recv_cb) {
                    conn->recv_cb(conn->recv_udata);
                    ccev__conn_rearm(loop, conn);
                }
            }

            if (fired_write) {
                /* Connect completion */
                if (conn->type == CCEV_CONN_CONNECTING) {
                    int so_err = 0;
                    socklen_t errlen = sizeof(so_err);
                    getsockopt((int)(intptr_t)conn->fd, SOL_SOCKET,
                                SO_ERROR, (char*)&so_err, &errlen);
                    if (so_err == 0 && conn->connector.cb) {
                        conn->type = CCEV_CONN_NORMAL;
                        conn->connector.cb(conn->connector.udata, conn, CCEV_OK);
                        /* Connect succeeded — re-arm for I/O */
                        ccev__conn_rearm(loop, conn);
                    } else if (conn->connector.cb) {
                        conn->connector.cb(conn->connector.udata, conn, CCEV_ERR);
                        ccev__conn_schedule_close(loop, conn);
                    }
                    continue;
                }

                /* Flush write buffer (ccev__conn_flush re-arms internally) */
                if (conn->pending_write) ccev__conn_flush(loop, conn);
            }
        }

        /* 5. Re-arm wake_conn for next ccev_wakeup / ccev_loop_stop */
        if (loop->wake_conn && !loop->wake_conn->closed)
            ccev__conn_mod_internal(loop, loop->wake_conn, EPOLLIN);

        /* 6. Process closing queue */
        while (!cclist_empty(&loop->closing)) {
            cclist_node_t *cn = cclist_pop_front(&loop->closing);
            if (!cn) continue;
            ccev_conn_t *conn = (ccev_conn_t *)((char*)cn - offsetof(ccev_conn_t, lnode));
            conn->in_closing = false;
            /* Remove from all_conns */
            cclist_remove(&loop->all_conns, &conn->lnode);
            /* Fire close callback */
            if (conn->close_cb) conn->close_cb(conn->close_udata);
            ccev__conn_free(loop, conn);
        }

        if (loop->stop_flag) break;

    } while (mode == CCEV_RUN_FOREVER);

    return mode == CCEV_RUN_ONCE ? n : 0;
}

/* ════════════════════════════════════════════════════════════════
 *  Listener
 * ════════════════════════════════════════════════════════════════ */

ccev_conn_t *ccev_listen(ccev_loop_t *loop, const char *addr, uint16_t port,
                           int backlog, ccev_flag_t flags,
                           ccev_accept_cb on_accept, void *udata) {
    if (!loop || !addr || !on_accept) return NULL;

    /* Determine address family from string format */
    ccsocket_family_t family = ccsocket_get_version(addr);
    if (family == CC_FAMILY_INVALID) return NULL;
    if (family == CC_INET4 || family == CC_INET6) {
        if (addr[0] == '\0') { addr = "::"; family = CC_INET6; }
    }

    int proto = (flags & CCEV_UDP) ? CC_UDP : CC_TCP;
    ccsocket_t fd = ccsocket1(family, proto, CC_CLOEXEC | CC_NONBLOCK);
    if (fd == (ccsocket_t)-1) return NULL;

    /* Apply flags (UDS ignores most socket options) */
    if (family != CC_UNIX) {
        if (flags & CCEV_REUSEADDR) ccsocket_set_reuseaddr(fd, true);
        if (flags & CCEV_REUSEPORT) ccsocket_set_reuseport(fd, true);
        if (flags & CCEV_TCP_NODELAY) ccsocket_set_nodelay(fd, true);
    }

    if (!ccsocket_listen(fd, addr, port)) { ccsocket_close(fd); return NULL; }

    ccev_conn_t *conn = ccev_conn_create(loop, fd, udata);
    if (!conn) { ccsocket_close(fd); return NULL; }

    conn->type           = CCEV_CONN_LISTENER;
    conn->listener.cb    = on_accept;
    conn->listener.udata = udata;

    ccev__conn_mod_internal(loop, conn, EPOLLIN);
    return conn;
}

/* ════════════════════════════════════════════════════════════════
 *  Async connect (uses ccsocket)
 * ════════════════════════════════════════════════════════════════ */

/* Timer callback for connect timeout */
static void connect_timeout_cb(void *udata) {
    ccev_conn_t *conn = (ccev_conn_t *)udata;
    if (conn->type == CCEV_CONN_CONNECTING && conn->connector.cb) {
        conn->connector.cb(conn->connector.udata, conn, CCEV_ERR);
        ccev__conn_schedule_close(conn->loop, conn);
    }
}

int ccev_connect(ccev_loop_t *loop, const char *addr, uint16_t port,
                  unsigned int timeout_ms, ccev_flag_t flags,
                  ccev_connect_cb on_connect, void *udata) {
    if (!loop || !addr || !on_connect) return CCEV_ERR;

    ccsocket_family_t family = ccsocket_get_version(addr);
    if (family == CC_FAMILY_INVALID) return CCEV_ERR;

    int proto = (flags & CCEV_UDP) ? CC_UDP : CC_TCP;
    ccsocket_t fd = ccsocket1(family, proto, CC_CLOEXEC | CC_NONBLOCK);
    if (fd == (ccsocket_t)-1) return CCEV_ERR;

    /* UDP / UDS datagram: immediate connect (non-blocking) */
    if (proto == CC_UDP || family == CC_UNIX) {
        if (ccsocket_connect(fd, addr, port)) {
            ccev_conn_t *conn = ccev_conn_create(loop, fd, udata);
            if (!conn) { ccsocket_close(fd); return CCEV_ERR; }
            conn->type = CCEV_CONN_NORMAL;
            on_connect(udata, conn, CCEV_OK);
            return CCEV_OK;
        }
        return CCEV_ERR;
    }

    /* TCP: non-blocking connect */
    bool connected = ccsocket_connect(fd, addr, port);
    if (connected) {
        ccev_conn_t *conn = ccev_conn_create(loop, fd, udata);
        if (!conn) { ccsocket_close(fd); return CCEV_ERR; }
        conn->type = CCEV_CONN_NORMAL;
        on_connect(udata, conn, CCEV_OK);
        return CCEV_OK;
    }

    /* EINPROGRESS — wait for EPOLLOUT */
    ccev_conn_t *conn = ccev_conn_create(loop, fd, udata);
    if (!conn) { ccsocket_close(fd); return CCEV_ERR; }

    conn->type            = CCEV_CONN_CONNECTING;
    conn->connector.cb    = on_connect;
    conn->connector.udata = udata;

    /* Register for EPOLLOUT (fires on connect completion) */
    ccev__conn_mod_internal(loop, conn, EPOLLOUT);

    if (timeout_ms > 0) {
        conn->connector.timer = ccev_timer_add(
            loop, timeout_ms, CCEV_TIMER_ONCE,
            connect_timeout_cb, conn);
    }

    return CCEV_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  Wakeup
 * ════════════════════════════════════════════════════════════════ */

int ccev_wakeup(ccev_loop_t *loop) {
    if (!loop) return CCEV_ERR;
    char c = 1;
    return (ccsocket_send(loop->wakefds[1], &c, 1, NULL) == CC_OPCODE_OK)
           ? CCEV_OK : CCEV_ERR;
}
