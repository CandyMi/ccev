/**
 * @file ccev.c
 * @brief Reactor core — event-loop lifecycle, listen, connect, dispatch.
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
 *   ccev__process_closing() (in ccev_sock.c) fires close_cb and frees.
 *
 * ccev_connect DNS integration:
 *   Hostnames are resolved via ccev_dns_resolve() before TCP connect.
 *   The total timeout spans both DNS and TCP connect phases.
 */

#include "ccev_internal.h"

/* ════════════════════════════════════════════════════════════════
 *  Loop lifecycle
 * ════════════════════════════════════════════════════════════════ */

ccev_loop_t *ccev_loop_create(int max_events) {
    ccev_loop_t *loop = (ccev_loop_t *)ccev__realloc_fn(NULL, sizeof(ccev_loop_t));
    if (!loop) return NULL;

    memset(loop, 0, sizeof(ccev_loop_t));
    loop->signal_pipe[0] = loop->signal_pipe[1] = (ccsocket_t)-1;

    loop->poll = ccev__poll_create(max_events);
    if (!loop->poll) { ccev__free_fn(loop); return NULL; }

    /* Init data structures */
    cclist_init(&loop->all_socks);
    cclist_init(&loop->closing);
    cclink_init(&loop->signal_queue);
    ccheap_init(&loop->timers, NULL);

    /* Init DNS state */
    ccev__dns_init(loop);
    cchashmap_init(&loop->dns_cache, NULL, NULL);
    cchashmap_init(&loop->dns_pending, NULL, NULL);
    ccev_dns_flush(loop);

    return loop;
}

void ccev_loop_destroy(ccev_loop_t *loop) {
    if (!loop) return;

    /* Process any pending closing queue first — close_cb may reference
     * the wake/signal pipes, so keep them open until callbacks finish. */
    ccev__process_closing(loop);

    /* Now close the write-end of the signal pipe.  The read-end is
     * closed below inside ccev__sock_free (signal_sock is freed via
     * the all_socks loop).  The wake pipe is owned by the poll layer
     * and is released inside ccev__poll_destroy. */
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

    /* Free pending signal events */
    while (!cclink_empty(&loop->signal_queue)) {
        cclink_node_t *_n = cclink_pop_front(&loop->signal_queue);
        ccev__free_fn(_n);
    }

    ccev__poll_destroy(loop->poll);
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
    ccev__sock_mod_internal(loop, sc, CCEV_POLL_READ);

    return loop;
}

void ccev_loop_stop(ccev_loop_t *loop) {
    if (!loop) return;
    ccev_atomic_store(loop->stop_flag, 1);
    ccev_wakeup(loop);
}

/* ════════════════════════════════════════════════════════════════
 *  Main event loop
 * ════════════════════════════════════════════════════════════════ */

/* ════════════════════════════════════════════════════════════════
 *  Event dispatch — per-event callback from poll layer
 *
 *  Called by ccev__poll_wait for each ready fd.  Handles HUP/ERR
 *  close, LISTEN accept batching, CONNECT completion, and normal
 *  read/write event callbacks with ONESHOT re-arm.
 *
 *  The poll layer owns the wake pipe and drains it internally —
 *  this callback never sees wake events.
 * ════════════════════════════════════════════════════════════════ */

static void _dispatch_one(struct ccev_poll_event *ev, void *arg) {
    ccev_loop_t *loop = (ccev_loop_t *)arg;
    ccev_sock_t *sock = (ccev_sock_t *)ev->udata;
    if (!sock || sock->closed) return;

    int fired = ev->events;

    /* Convert poll events to CCEV_EVENT flags */
    int ccev_events = 0;
    if (fired & CCEV_POLL_READ)  ccev_events |= CCEV_EVENT_READ;
    if (fired & CCEV_POLL_WRITE) ccev_events |= CCEV_EVENT_WRITE;
    if (fired & (CCEV_POLL_ERR | CCEV_POLL_HUP))
        ccev_events |= CCEV_EVENT_HUP;

    /* HUP/ERR — handle by socket mode.
     *   CONNECT: probe connection state, fire connect_cb, then close.
     *   INIT:    deliver HUP via rcb so user can read final data, then close.
     *   LISTEN:  just close. */
    if (ccev_events & CCEV_EVENT_HUP) {
        if (sock->mode == CCEV_SOCK_CONNECT) {
            ccev_sock_any_t *any = (ccev_sock_any_t *)sock;
            ccsocket_conn_state_t st = ccsocket_is_connected(sock->fd);
            if (any->connector.cb)
                any->connector.cb(any->connector.udata, sock,
                                   st == CC_CONNECTED ? CCEV_OK : CCEV_ERR);
            ccev__sock_schedule_close(loop, sock);
            return;
        }
        if (sock->mode == CCEV_SOCK_INIT) {
            sock->events = 0;
            if (sock->rcb) sock->rcb(sock, ccev_events);
            ccev__sock_schedule_close(loop, sock);
            return;
        }
        /* LISTEN (or unknown) — just close */
        ccev__sock_schedule_close(loop, sock);
        return;
    }

    /* Reset registered events (ONESHOT consumed them) */
    sock->events = 0;

    /* Dispatch by mode */
    if (fired & CCEV_POLL_READ) {
        if (sock->mode == CCEV_SOCK_LISTEN) {
            /* Listener — batch accept */
            int count = 0;
            while (count < CCEV_MAX_ACCEPT_BATCH) {
                char addr_buf[64];
                uint16_t port = 0;
                ccsocket_t afd = ccsocket_accept2(sock->fd, addr_buf, &port, 0);
                if (afd == (ccsocket_t)0) break;   /* EAGAIN */
                if (afd == (ccsocket_t)-1) break;  /* error */
                ccev_sock_any_t *any = (ccev_sock_any_t *)sock;
                if (any->listener.cb) {
                    ccev_sock_t *client = ccev_sock_create(loop, afd, NULL);
                    if (client) {
                        any->listener.cb(any->listener.udata,
                                          client, addr_buf, (int)port);
                    } else {
                        ccsocket_close(afd);
                    }
                } else {
                    ccsocket_close(afd);
                }
                count++;
            }
            ccev__sock_mod_internal(loop, sock, CCEV_POLL_READ);
            return;
        }

        /* Normal read — fire rcb */
        if (sock->rcb) sock->rcb(sock, ccev_events);
    }

    if (fired & CCEV_POLL_WRITE) {
        if (sock->mode == CCEV_SOCK_CONNECT) {
            /* Connect completion — pure POLL_WRITE (no HUP) means the
             * connect operation has finished.  Use ccsocket_is_connected
             * for a cross-platform read-only probe. */
            ccev_sock_any_t *any = (ccev_sock_any_t *)sock;
            ccsocket_conn_state_t st = ccsocket_is_connected(sock->fd);
            if (st == CC_CONNECTED) {
                sock->mode = CCEV_SOCK_INIT;
                /* Clear connect timer */
                if (any->connector.timer) {
                    ccev_timer_del(loop, any->connector.timer);
                    any->connector.timer = NULL;
                }
                if (any->connector.cb)
                    any->connector.cb(any->connector.udata, sock, CCEV_OK);
            } else {
                if (any->connector.cb)
                    any->connector.cb(any->connector.udata, sock, CCEV_ERR);
                ccev__sock_schedule_close(loop, sock);
            }
            return;
        }

        /* Normal write — fire wcb */
        if (sock->wcb) sock->wcb(sock, ccev_events);
    }

    /* Re-arm after ONESHOT consumption */
    if (sock->rcb || sock->wcb)
        ccev__sock_rearm(loop, sock);
}

int ccev_loop_run(ccev_loop_t *loop, ccev_run_mode_t mode) {
    if (!loop) return CCEV_ERR;
    if (ccev_atomic_load(loop->stop_flag)) return 0;
    int n = 0;

    do {
        /* Drain all pending callbacks until stable.
         * ecb, signal, timer, and closing callbacks can all produce
         * sub-events (close → closing, timer_add, chain close, ...).
         * Loop repeatedly until nothing remains, then block in poll. */
        bool pending;
        do {
            pending = false;

            /* ecb: dispatcher, runs first each inner iteration.
             * Does not set pending directly — its sub-events are
             * detected by signal/timer/closing steps below. */
            if (loop->ecb) {
                loop->ecb(loop, loop->ecb_args);
            }
            if (ccev_atomic_load(loop->stop_flag)) break;

            /* Drain signal queue — signal_cb may close sockets. */
            if (!cclink_empty(&loop->signal_queue)) {
                ccev__signal_process_queue(loop);
                pending = true;
            }
            if (ccev_atomic_load(loop->stop_flag)) break;

            /* Fire expired timers — timer_cb may close or add timers. */
            {
                uint64_t now = ccev__now_ms();
                if (ccev__timer_next_ms(loop, now) == 0) {
                    ccev__timer_process(loop, now);
                    pending = true;
                }
            }
            if (ccev_atomic_load(loop->stop_flag)) break;

            /* Drain closing queue — close_cb may chain-close. */
            if (!cclist_empty(&loop->closing)) {
                ccev__process_closing(loop);
                pending = true;
            }
            if (ccev_atomic_load(loop->stop_flag)) break;

        } while (pending);

        if (ccev_atomic_load(loop->stop_flag)) break;

        /* Compute poll timeout — timer-driven only, no special cases. */
        int timeout;
        if (mode == CCEV_RUN_ONCE) {
            timeout = 0;
        } else {
            timeout = ccev__timer_next_ms(loop, ccev__now_ms());
        }

        /* Poll for I/O events.  dispatch_one() may queue closing and
         * signal items as a side-effect.  Drain them immediately so
         * RUN_ONCE callers see callbacks fire before return. */
        n = ccev__poll_wait(loop->poll, timeout, _dispatch_one, loop);
        if (n < 0) n = 0;

        if (!cclist_empty(&loop->closing)) ccev__process_closing(loop);
        if (!cclink_empty(&loop->signal_queue)) ccev__signal_process_queue(loop);

        if (ccev_atomic_load(loop->stop_flag)) break;

    } while (mode == CCEV_RUN_FOREVER);

    ccev_atomic_store(loop->stop_flag, 0);
    return mode == CCEV_RUN_ONCE ? n : 0;
}

/* ════════════════════════════════════════════════════════════════
 *  Per-iteration callback
 * ════════════════════════════════════════════════════════════════ */

int ccev_each(ccev_loop_t *loop, ccev_loop_each_cb cb, void *args) {
    if (!loop) return CCEV_ERR;
    loop->ecb      = cb;
    loop->ecb_args = cb ? args : NULL;
    return CCEV_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  Wakeup
 * ════════════════════════════════════════════════════════════════ */

/* ── Public wakeup API ── */

int ccev_wakeup(ccev_loop_t *loop) {
    if (!loop) return CCEV_ERR;
    if (!loop->poll) return CCEV_ERR;
    ccev__poll_wake(loop->poll);
    return CCEV_OK;
}
