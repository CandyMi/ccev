/**
 * @file ccev.c
 * @brief Reactor core — event-loop lifecycle, listen, connect, dispatch.
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
 *   ccev__process_closing() (in ccev_sock.c) fires close_cb and frees.
 *
 * ccev_connect DNS integration:
 *   Hostnames are resolved via ccev_dns_resolve() before TCP connect.
 *   The total timeout spans both DNS and TCP connect phases.
 */

#include "ccev_internal.h"

/* Fallback compiler barrier — extern so compiler cannot see the body */
void ccev__compiler_barrier(void) {}

/* Default allocator (libc) */
static void *ccev_default_realloc(void *ptr, size_t sz) {
    if (sz == 0) { free(ptr); return NULL; }
    return realloc(ptr, sz);
}
static void ccev_default_free(void *ptr) { free(ptr); }

void *(*ccev__realloc_fn)(void*, size_t) = ccev_default_realloc;
void  (*ccev__free_fn)(void*)            = ccev_default_free;

void ccev_set_allocator(void *(*realloc_fn)(void*, size_t),
                        void  (*free_fn)(void*)) {
    if (realloc_fn) ccev__realloc_fn = realloc_fn;
    if (free_fn)    ccev__free_fn    = free_fn;
}

/* ════════════════════════════════════════════════════════════════
 *  Loop lifecycle
 * ════════════════════════════════════════════════════════════════ */

ccev_loop_t *ccev_loop_create(int max_events) {
    ccev_loop_t *loop = (ccev_loop_t *)ccev__realloc_fn(NULL, sizeof(ccev_loop_t));
    if (!loop) return NULL;

    memset(loop, 0, sizeof(ccev_loop_t));
    loop->wakefds[0] = loop->wakefds[1]     = (ccsocket_t)-1;
    loop->signal_pipe[0] = loop->signal_pipe[1] = (ccsocket_t)-1;
    loop->max_events = max_events > 0 ? max_events : 64;

    loop->epfd = epoll_create(1);
    if ((intptr_t)loop->epfd < 0) { ccev__free_fn(loop); return NULL; }

    loop->events = (struct epoll_event *)ccev__realloc_fn(
        NULL, (size_t)loop->max_events * sizeof(struct epoll_event));
    if (!loop->events) { epoll_close(loop->epfd); ccev__free_fn(loop); return NULL; }

    /* Init data structures */
    cclist_init(&loop->all_socks);
    cclist_init(&loop->closing);
    ccheap_init(&loop->timers, NULL);

    /* Init DNS state */
    ccev__dns_init(loop);
    cchashmap_init(&loop->dns_cache, NULL, NULL);
    cchashmap_init(&loop->dns_pending, NULL, NULL);
    ccev_dns_flush(loop);

    /* Wakeup pipe */
    if (ccsocket_pipe(loop->wakefds) < 0) {
        ccev__free_fn(loop->events);
        epoll_close(loop->epfd);
        ccev__free_fn(loop);
        return NULL;
    }
    ccsocket_set_nonblock(loop->wakefds[1], true);

    loop->wake_sock = NULL;
    ccev_sock_t *wake = ccev_sock_create(loop, loop->wakefds[0], NULL);
    if (wake) {
        loop->wake_sock = wake;
        ccev__sock_mod_internal(loop, wake, EPOLLIN);
    }

    return loop;
}

void ccev_loop_destroy(ccev_loop_t *loop) {
    if (!loop) return;

    /* Process any pending closing queue first — close_cb may reference
     * the wake/signal pipes, so keep them open until callbacks finish. */
    ccev__process_closing(loop);

    /* Now close the write-ends of wake/signal pipes.  The read-ends
     * are closed below inside ccev__sock_free (wake_sock / signal_sock
     * are freed via the all_socks loop). */
    if (loop->wakefds[1] != (ccsocket_t)-1) ccsocket_close(loop->wakefds[1]);
    if (loop->signal_pipe[1] != (ccsocket_t)-1) ccsocket_close(loop->signal_pipe[1]);

    /* Free remaining socks (not in closing list).
     * Fire close_cb so any udata-backed resources (e.g. DNS query) are freed. */
    while (!cclist_empty(&loop->all_socks)) {
        cclist_node_t *n = cclist_pop_front(&loop->all_socks);
        if (n) {
            ccev_sock_t *sock = (ccev_sock_t *)((char*)n - offsetof(ccev_sock_t, lnode));
            if (sock->close_cb) sock->close_cb(sock->close_udata);
            ccev__sock_free(sock);
        }
    }

    /* Free all timers — O(n) direct data[] traversal instead of O(n log n)
     * ccheap_pop loop.  The heap is about to be destroyed so maintaining
     * the heap invariant during cleanup is unnecessary. */
    for (size_t _ti = 0; _ti < loop->timers.len; _ti++) {
        ccev_timer_t *t = CCHEAP_CONTAINER(loop->timers.data[_ti],
                                            ccev_timer_t, node);
        ccev__free_fn(t);
    }
    ccheap_destroy(&loop->timers);

    /* Free DNS cache */
    if (loop->dns_cache.buckets) {
        for (size_t _i = 0; _i < loop->dns_cache.cap; _i++) {
            cchashmap_node_t *_n = loop->dns_cache.buckets[_i];
            while (_n) {
                cchashmap_node_t *_next = _n->next;
                ccev_dns_cache_t *_e = CCHASHMAP_CONTAINER(_n, ccev_dns_cache_t, node);
                ccev__free_fn(_e);
                _n = _next;
            }
        }
    }
    cchashmap_destroy(&loop->dns_cache);

    /* Free DNS pending entries */
    if (loop->dns_pending.buckets) {
        for (size_t _i = 0; _i < loop->dns_pending.cap; _i++) {
            cchashmap_node_t *_n = loop->dns_pending.buckets[_i];
            while (_n) {
                cchashmap_node_t *_next = _n->next;
                ccev_dns_pending_t *_p = CCHASHMAP_CONTAINER(_n, ccev_dns_pending_t, node);
                cclink_node_t *_wn = cclink_begin(&_p->waiters);
                while (_wn) {
                    cclink_node_t *_wnext = cclink_next(_wn);
                    ccev_dns_waiter_t *_w = CCLINK_CONTAINER(_wn, ccev_dns_waiter_t, node);
                    ccev__free_fn(_w);
                    _wn = _wnext;
                }
                ccev__free_fn(_p);
                _n = _next;
            }
        }
    }
    cchashmap_destroy(&loop->dns_pending);

    /* DNS server string is embedded (no heap alloc) — nothing to free */

    ccev__free_fn(loop->events);
    epoll_close(loop->epfd);
    ccev__free_fn(loop);
}

ccev_loop_t *ccev_default_loop(void) {
    static ccev_loop_t *loop = NULL;
    if (loop) return loop;

    loop = ccev_loop_create(2048);
    if (!loop) return NULL;

    /* Create self-pipe for signal delivery */
    if (ccsocket_pipe(loop->signal_pipe) < 0) {
        ccev_loop_destroy(loop);
        return NULL;
    }
    ccsocket_set_nonblock(loop->signal_pipe[0], true);
    ccsocket_set_nonblock(loop->signal_pipe[1], true);

    /* Wrap read end as a sock so the loop dispatches it */
    ccev_sock_t *sc = ccev_sock_create(loop, loop->signal_pipe[0], NULL);
    if (!sc) {
        ccev_loop_destroy(loop);
        return NULL;
    }
    loop->signal_sock = sc;
    /* Always set rcb so the signal pipe is dispatched even before
     * ccev_signal_handle() is called.  Without it, a signal can fire
     * while ONESHOT is armed but rcb is NULL — the event is consumed
     * but the byte is never read, and the next re-arm has no rcb to
     * re-arm for.  On Windows epoll emulation this can manifest as
     * lost signals. */
    sc->rcb = ccev__signal_dispatch;
    ccev__sock_mod_internal(loop, sc, EPOLLIN);

    ccev_dns_flush(loop);
    return loop;
}

void ccev_loop_stop(ccev_loop_t *loop) {
    if (!loop) return;
    loop->stop_flag = true;
    CCEV_COMPILER_BARRIER();
    ccev_wakeup(loop);
}

/* ════════════════════════════════════════════════════════════════
 *  Main event loop
 * ════════════════════════════════════════════════════════════════ */

/* ════════════════════════════════════════════════════════════════
 *  Event dispatch — process all ready fds from epoll_wait
 *
 *  Called once per loop iteration from ccev_loop_run.  Handles
 *  wake-pipe draining, HUP/ERR close, LISTEN accept batching,
 *  CONNECT completion, and normal read/write event callbacks
 *  with ONESHOT re-arm.
 * ════════════════════════════════════════════════════════════════ */

static void _dispatch_events(ccev_loop_t *loop, int n) {
    for (int i = 0; i < n; i++) {
        struct epoll_event *ev = &loop->events[i];
        ccev_sock_t *sock = (ccev_sock_t *)ev->data.ptr;
        if (!sock || sock->closed) continue;

        uint32_t fired = ev->events;

        /* Wakeup pipe — drain */
        if (sock == loop->wake_sock) {
            char buf[64];
            while (ccsocket_recv(sock->fd, buf, sizeof(buf), NULL) == CC_OPCODE_OK) {}
            continue;
        }

        /* Convert epoll events to CCEV_EVENT flags */
        int ccev_events = 0;
        if (fired & EPOLLIN)  ccev_events |= CCEV_EVENT_READ;
        if (fired & EPOLLOUT) ccev_events |= CCEV_EVENT_WRITE;
        if (fired & (EPOLLERR | EPOLLHUP))
            ccev_events |= CCEV_EVENT_HUP;

        /* HUP/ERR fast-path — schedule deferred close */
        if (ccev_events & CCEV_EVENT_HUP) {
            ccev__sock_schedule_close(loop, sock);
            continue;
        }

        /* Reset registered events (ONESHOT consumed them) */
        sock->events = 0;

        /* Dispatch by mode */
        if (fired & EPOLLIN) {
            if (sock->mode == CCEV_SOCK_LISTEN) {
                /* Listener — batch accept */
                int count = 0;
                while (count < CCEV_MAX_ACCEPT_BATCH) {
                    char addr_buf[64];
                    uint16_t port = 0;
                    ccsocket_t afd = ccsocket_accept2(sock->fd, addr_buf, &port, 0);
                    if (afd == (ccsocket_t)0) break;   /* EAGAIN */
                    if (afd == (ccsocket_t)-1) break;  /* error */
                    if (sock->listener.cb) {
                        ccev_sock_t *client = ccev_sock_create(loop, afd, NULL);
                        if (client) {
                            sock->listener.cb(sock->listener.udata,
                                               client, addr_buf, (int)port);
                        } else {
                            ccsocket_close(afd);
                        }
                    } else {
                        ccsocket_close(afd);
                    }
                    count++;
                }
                ccev__sock_mod_internal(loop, sock, EPOLLIN);
                continue;
            }

            /* Normal read — fire rcb */
            if (sock->rcb) sock->rcb(sock, ccev_events);
        }

        if (fired & EPOLLOUT) {
            if (sock->mode == CCEV_SOCK_CONNECT) {
                /* Connect completion */
                int so_err = 0;
                socklen_t errlen = sizeof(so_err);
                getsockopt((int)(intptr_t)sock->fd, SOL_SOCKET,
                            SO_ERROR, (char*)&so_err, &errlen);
                if (so_err == 0) {
                    sock->mode = CCEV_SOCK_INIT;
                    /* Clear connect timer */
                    if (sock->connector.timer) {
                        ccev_timer_del(loop, sock->connector.timer);
                        sock->connector.timer = NULL;
                    }
                    if (sock->connector.cb)
                        sock->connector.cb(sock->connector.udata, sock, CCEV_OK);
                } else {
                    if (sock->connector.cb)
                        sock->connector.cb(sock->connector.udata, sock, CCEV_ERR);
                    ccev__sock_schedule_close(loop, sock);
                }
                continue;
            }

            /* Normal write — fire wcb */
            if (sock->wcb) sock->wcb(sock, ccev_events);
        }

        /* Re-arm after ONESHOT consumption */
        if (sock->rcb || sock->wcb)
            ccev__sock_rearm(loop, sock);
    }
}

int ccev_loop_run(ccev_loop_t *loop, ccev_run_mode_t mode) {
    if (!loop) return CCEV_ERR;
    /* Honour any stop request that was set before entering the loop
     * (e.g. from a synchronous DNS or ICMP callback). */
    CCEV_COMPILER_BARRIER();
    if (loop->stop_flag) return 0;
    int n = 0;

    do {
        /* 0. Per-iteration callback (runs first each iteration).
         *     Registered via ccev_each().  When ecb is NULL (the common
         *     case) the branch is predicted not-taken — zero cost. */
        if (loop->ecb) loop->ecb(loop);

#if defined(_WIN32)
        /* On Windows the signal handler stores the signum in
         * loop->sig_pending and wakes the loop.  Check it here
         * on every iteration. */
        ccev__signal_dispatch(NULL, 0);
#endif
        /* P1: skip clock_gettime when no timers are registered.
         *     ccev__now_ms() is a vdso call on Linux (~30ns) but a full
         *     syscall on older kernels/containers. */
        int next_ms;
        uint64_t now;
        if (loop->timer_count > 0) {
            now = ccev__now_ms();
            next_ms = ccev__timer_process(loop, now);
        } else {
            next_ms = -1;
        }
        CCEV_COMPILER_BARRIER();
        if (loop->stop_flag) break;

        /* 2. Compute epoll timeout
         *    RUN_ONCE → 0 (poll);  timer pending → next_ms;
         *    otherwise → -1 (block until I/O). */
        int timeout;
        if (mode == CCEV_RUN_ONCE) {
            timeout = 0;
        } else if (next_ms < 0) {
            timeout = -1;
        } else {
            timeout = next_ms;
        }

        /* 3. epoll_wait (retry on EINTR — don't re-enter loop body) */
        do {
            n = epoll_wait(loop->epfd, loop->events, loop->max_events, timeout);
        } while (n < 0 && errno == EINTR);

        /* 4. Dispatch events — extracted to _dispatch_events */
        _dispatch_events(loop, n);

        /* 5. Re-arm wake_sock for next ccev_wakeup / ccev_loop_stop */
        if (loop->wake_sock && !loop->wake_sock->closed)
            ccev__sock_mod_internal(loop, loop->wake_sock, EPOLLIN);

        /* 6. Process closing queue */
        ccev__process_closing(loop);

        CCEV_COMPILER_BARRIER();
        if (loop->stop_flag) break;

    } while (mode == CCEV_RUN_FOREVER);

    /* Clear the stop flag that was set during dispatch (e.g. by
     * ccev_loop_stop in a signal callback) so the next call to
     * ccev_loop_run doesn't see a stale flag and skip. */
    CCEV_COMPILER_BARRIER();
    loop->stop_flag = false;
    CCEV_COMPILER_BARRIER();
    return mode == CCEV_RUN_ONCE ? n : 0;
}

/* ════════════════════════════════════════════════════════════════
 *  Per-iteration callback
 * ════════════════════════════════════════════════════════════════ */

int ccev_each(ccev_loop_t *loop, ccev_loop_each_cb cb) {
    if (!loop) return CCEV_ERR;
    loop->ecb = cb;
    return CCEV_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  Wakeup
 * ════════════════════════════════════════════════════════════════ */

/* ── Public wakeup API ── */

int ccev_wakeup(ccev_loop_t *loop) {
    if (!loop) return CCEV_ERR;
    if (loop->wakefds[1] == (ccsocket_t)-1) return CCEV_ERR;
    char c = 1;
    return (ccsocket_send(loop->wakefds[1], &c, 1, NULL) == CC_OPCODE_OK)
           ? CCEV_OK : CCEV_ERR;
}
