/**
 * @file ccev_stream.c
 * @brief Stream — write buffering, sendfile, stream read (raw / readnum / readline).
 *
 * @author CandyMi
 * @license MIT
 *
 * The stream embeds a ccev_sock_t by value and takes over its wcb
 * for buffered I/O.  The read path offers three modes, all zero-
 * allocation in the common case (data already in kernel buffer):
 *
 *   ccev_stream_read()     — raw dispatch
 *   ccev_stream_readnum()  — fixed-N bytes
 *   ccev_stream_readline() — delimiter-based
 *
 * Stack buffer size is configurable at compile time:
 *   -DCCEV_READ_STACK_SIZE=4096
 *
 * File: ccev_stream.c
 */

#include "ccev_internal.h"
#include <fcntl.h>

#ifdef _WIN32
#include <io.h>
#endif

#ifndef IOV_MAX
#  define IOV_MAX 1024
#endif

/* ════════════════════════════════════════════════════════════════
 *  Utility helpers
 * ════════════════════════════════════════════════════════════════ */

static inline ccev_stream_t *_sock_to_stream(ccev_sock_t *sock) {
    /* ccev_stream_t's first field is ccev_sock_t — safe cast.
     * Backed by the ccev_sock_any_t union (see ccev_internal.h). */
    return (ccev_stream_t *)sock;
}

static void _stream_invoke_send_cb(ccev_stream_t *st) {
    if (st->send_cb) {
        ccev_send_cb cb = st->send_cb;
        void *ud = st->send_udata;
        st->send_cb = NULL;
        cb(ud);
    }
}

/* ════════════════════════════════════════════════════════════════
 *  Write buffer management
 * ════════════════════════════════════════════════════════════════ */

static ccev_buf_t *_buf_alloc(const void *data,
                               size_t len, ccev_send_cb cb, void *udata) {
    /* Single allocation: ccev_buf_t header + flexible array payload.
     * offsetof(data) works with data[], data[0], and data[1] uniformly. */
    ccev_buf_t *buf = (ccev_buf_t *)ccev__realloc_fn(
        NULL, offsetof(ccev_buf_t, data) + len);
    if (!buf) return NULL;
    memcpy(buf->data, data, len);
    buf->len     = len;
    buf->offset  = 0;
    buf->cb      = cb;
    buf->cb_udata = udata;
    return buf;
}

static void _buf_free(ccev_buf_t *buf) {
    ccev__free_fn(buf);
}

/* ════════════════════════════════════════════════════════════════
 *  Flush write buffer (called from dispatch on EPOLLOUT)
 * ════════════════════════════════════════════════════════════════ */

static void _stream_on_writable(ccev_sock_t *sock, int events);
/* Forward declaration: _stream_on_writable calls this before its definition. */
static void ccev__stream_sendfile_continue(ccev_loop_t *loop, ccev_stream_t *st);

static void ccev__stream_flush(ccev_loop_t *loop, ccev_stream_t *st) {
    (void)loop;
    if (st->sock.closed || !st->pending_write) return;

    /* Build iovec */
    ccsocket_iovec_t iov[IOV_MAX];
    int niov = 0;
    cclink_node_t *n = cclink_begin(&st->wlist);
    while (n && niov < IOV_MAX) {
        ccev_buf_t *b = (ccev_buf_t *)((char*)n - offsetof(ccev_buf_t, node));
        size_t remaining = b->len - b->offset;
        if (remaining > 0) {
            ccsocket_set_iov_buf(&iov[niov], 0, b->data + b->offset);
            ccsocket_set_iov_len(&iov[niov], 0, remaining);
            niov++;
        }
        n = cclink_next(n);
    }

    if (niov == 0) {
        st->pending_write = false;
        return;
    }

    int sent = 0;
    ccsocket_stcode_t rc = ccsocket_sendv(st->sock.fd, iov, niov, &sent);
    if (rc == CC_OPCODE_OK || rc == CC_OPCODE_WAIT) {
        size_t remaining_sent = (sent > 0) ? (size_t)sent : 0;
        while (remaining_sent > 0) {
            cclink_node_t *head = cclink_begin(&st->wlist);
            if (!head) break;
            ccev_buf_t *b = (ccev_buf_t *)((char*)head - offsetof(ccev_buf_t, node));
            size_t avail = b->len - b->offset;
            if (avail <= remaining_sent) {
                remaining_sent -= avail;
                st->wbuf_len -= avail;
                cclink_remove(&st->wlist, head);
                /* Fire per-buffer callback, then free */
                if (b->cb) b->cb(b->cb_udata);
                _buf_free(b);
            } else {
                b->offset += remaining_sent;
                st->wbuf_len -= remaining_sent;
                remaining_sent = 0;
            }
        }

        if (cclink_empty(&st->wlist)) {
            st->pending_write = false;
            _stream_invoke_send_cb(st);
        } else {
            st->pending_write = true;
            st->sock.wcb = _stream_on_writable;
            ccev__sock_rearm(loop, &st->sock);
        }
    } else {
        /* Error — close */
        st->pending_write = false;
        ccev__sock_schedule_close(st->sock.loop, &st->sock);
    }
}

/* ── Internal write dispatch (called from stream's wcb on EPOLLOUT) ── */
static void _stream_on_writable(ccev_sock_t *sock, int events) {
    (void)events;
    ccev_stream_t *st = _sock_to_stream(sock);
    ccev__stream_flush(sock->loop, st);

    /* Continue sendfile if in progress */
    if (st->sendfile_fd >= 0) {
        ccev__stream_sendfile_continue(sock->loop, st);
        /* If sendfile is still pending, keep wcb and return */
        if (st->sendfile_fd >= 0) return;
    }

    /* No pending writes or sendfile — disarm EPOLLOUT so the dispatch
     * loop does not busy-loop re-arming it on every iteration. */
    if (!st->pending_write)
        sock->wcb = NULL;
}

/* ════════════════════════════════════════════════════════════════
 *  Sendfile support
 * ════════════════════════════════════════════════════════════════ */

static void ccev__stream_sendfile_continue(ccev_loop_t *loop, ccev_stream_t *st) {
    if (st->sock.closed || st->sendfile_fd < 0) return;

    int fd = st->sendfile_fd;
    ccsocket_sendf_state_t rc = ccsocket_sendfile(st->sock.fd, fd);

    if (rc == CC_SENDALL) {
        close(fd);
        st->sendfile_fd = -1;
        if (st->sf_cb) {
            ccev_send_cb cb = st->sf_cb;
            void *ud = st->sf_udata;
            st->sf_cb = NULL;
            cb(ud);
        }
        ccev__sock_rearm(loop, &st->sock);
    } else if (rc == CC_SENDWAIT) {
        /* Still waiting — next EPOLLOUT will retry */
    } else {
        /* CC_SENDERROR */
        close(fd);
        st->sendfile_fd = -1;
        ccev__sock_schedule_close(loop, &st->sock);
    }
}

/* ════════════════════════════════════════════════════════════════
 *  Stream reader — internal recv callback (replaces sock->rcb)
 * ════════════════════════════════════════════════════════════════ */

#ifndef CCEV_READ_STACK_SIZE
#  define CCEV_READ_STACK_SIZE 16384
#endif
#define READ_STACK_SZ CCEV_READ_STACK_SIZE

static void _read_timeout_cb(ccev_timer_t *timer, void *udata) {
    ccev_stream_t *st = (ccev_stream_t *)udata;
    (void)timer;
    if (!st || st->sock.closed) return;
    st->read_timer = NULL;
    if (st->rn_heap) { ccev__free_fn(st->rn_heap); st->rn_heap = NULL; }
    st->want  = 0;
    st->delim = 0;
    if (st->read_cb) {
        ccev_stream_cb cb = st->read_cb;
        st->read_cb = NULL;
        cb(st->read_udata, NULL, 0, CCEV_ERR);
    }
    ccev__sock_schedule_close(st->sock.loop, &st->sock);
}

/* Reset idle timer on data arrival. */
static void _read_timer_bump(ccev_stream_t *st) {
    if (st->read_timer)
        ccev_timer_reset(st->sock.loop, st->read_timer,
                          (uint64_t)st->timeout_ms);
}

/* Release all read state.  Does NOT fire cb. */
static void _read_drop(ccev_stream_t *st) {
    if (st->rn_heap) { ccev__free_fn(st->rn_heap); st->rn_heap = NULL; }
    st->rn_total = 0;
    st->want     = 0;
    st->delim    = 0;
    if (st->read_timer) {
        ccev_timer_del(st->sock.loop, st->read_timer);
        st->read_timer = NULL;
    }
}

/* ── completion helpers: clear state before callback, free after ── */

static void _readnum_done(ccev_stream_t *st, char *heap, size_t n) {
    st->rn_heap = NULL; st->rn_total = 0; st->want = 0; st->delim = 0;
    st->read_cb(st->read_udata, heap, n, CCEV_OK);
    ccev__free_fn(heap);
}

static void _readline_done(ccev_stream_t *st, char *heap,
                            size_t line_len, size_t total) {
    size_t tail = total - line_len;
    char  *tail_ptr = heap + line_len;

    st->rn_heap = NULL; st->rn_total = 0; st->want = 0; st->delim = 0;
    st->read_cb(st->read_udata, heap, line_len, CCEV_OK);

    /* Callback may have switched mode.  If heap had bytes after the
     * dispatched line, preserve them for the new mode. */
    if (tail > 0 && st->want > 0) {
        st->rn_heap = (char *)ccev__realloc_fn(NULL, st->want);
        if (st->rn_heap) {
            size_t cp = tail < st->want ? tail : st->want;
            memcpy(st->rn_heap, tail_ptr, cp);
            st->rn_total = cp;
        }
    }
    ccev__free_fn(heap);
}

/* ════════════════════════════════════════════════════════════════
 *  Stream reader — internal recv callback (replaces sock->rcb)
 * ════════════════════════════════════════════════════════════════ */

static void _stream_on_readable(ccev_sock_t *sock, int events) {
    (void)events;
    if (!sock || sock->closed) return;
    ccev_stream_t *st = _sock_to_stream(sock);
    char stack[READ_STACK_SZ];

    /* Retry point: mode-switch guard may have populated rn_heap
     * without a kernel recv.  Loop back to process it immediately. */
retry_heap:

    /* ── Raw mode ── */
    if (st->want == 0 && st->delim == 0) {
        if (!st->read_cb) return;
        for (;;) {
            int to_read = (st->limit == 0) ? (int)sizeof(stack)
                         : (st->limit < sizeof(stack) ? (int)st->limit
                                                       : (int)sizeof(stack));
            int nread = 0;
            ccsocket_stcode_t rc = ccsocket_recv(sock->fd, stack,
                                                   to_read, &nread);
            if (rc == CC_OPCODE_OK && nread > 0) {
                st->read_cb(st->read_udata, stack, (size_t)nread, CCEV_OK);
                _read_timer_bump(st);
                if (st->want != 0 || st->delim != 0) return; /* switched */
                continue;
            }
            if (rc == CC_OPCODE_WAIT) { ccev__sock_rearm(sock->loop, sock); return; }
            st->read_cb(st->read_udata, NULL, 0, CCEV_ERR);
            _read_drop(st);
            ccev__sock_schedule_close(sock->loop, sock);
            return;
        }
    }

    /* ── Readline mode (delim != 0) ── */
    if (st->delim != 0) {
        char   saved_delim = st->delim;
        size_t saved_want  = st->want;

        /* Partial heap continuation */
        if (st->rn_heap) {
            /* Check existing heap data first (may have been populated
             * by mode-switch guard without a new recv). */
            {
                char *found = (char *)memchr(st->rn_heap, saved_delim,
                                              st->rn_total);
                if (found) {
                    _readline_done(st, st->rn_heap,
                                    (size_t)(found - st->rn_heap) + 1,
                                    st->rn_total);
                    /* Fall through — retry_heap will process any
                     * preserved tail bytes without a new recv. */
                    goto retry_heap;
                }
                if (st->rn_total >= saved_want) {
                    st->read_cb(st->read_udata, st->rn_heap,
                                 saved_want, CCEV_ERR);
                    _read_drop(st);
                    ccev__sock_schedule_close(sock->loop, sock);
                    return;
                }
            }
            int nread = 0;
            size_t remain = st->want - st->rn_total;
            ccsocket_stcode_t rc = ccsocket_recv(sock->fd,
                                                  st->rn_heap + st->rn_total,
                                                  remain, &nread);
            if (rc == CC_OPCODE_OK && nread > 0) {
                size_t prev = st->rn_total;
                st->rn_total += (size_t)nread;
                char *found = (char *)memchr(st->rn_heap + prev,
                                              saved_delim,
                                              st->rn_total - prev);
                if (found) {
                    _readline_done(st, st->rn_heap,
                                    (size_t)(found - st->rn_heap) + 1,
                                    st->rn_total);
                    ccev__sock_rearm(sock->loop, sock);
                    return;
                }
                if (st->rn_total >= saved_want) {
                    st->read_cb(st->read_udata, st->rn_heap,
                                 saved_want, CCEV_ERR);
                    _read_drop(st);
                    ccev__sock_schedule_close(sock->loop, sock);
                    return;
                }
            }
            if (rc == CC_OPCODE_WAIT) { ccev__sock_rearm(sock->loop, sock); return; }
            st->read_cb(st->read_udata, NULL, 0, CCEV_ERR);
            _read_drop(st);
            ccev__sock_schedule_close(sock->loop, sock);
            return;
        }

        /* First recv: dispatch complete lines from stack */
        {
            int nread = 0;
            int to_read = (saved_want < sizeof(stack)) ? (int)saved_want
                                                         : (int)sizeof(stack);
            ccsocket_stcode_t rc = ccsocket_recv(sock->fd, stack, to_read, &nread);
            if (rc == CC_OPCODE_OK && nread > 0) {
                char *p   = stack;
                char *end = stack + nread;
                while (p < end) {
                    char *nl = (char *)memchr(p, saved_delim,
                                               (size_t)(end - p));
                    if (!nl) break;
                    st->read_cb(st->read_udata, p,
                                 (size_t)(nl - p + 1), CCEV_OK);
                    _read_timer_bump(st);
                    p = nl + 1;

                    /* Check if callback changed the read mode */
                    if (st->delim != saved_delim ||
                        st->want  != saved_want  ||
                        st->want  == 0) {
                        if (p < end && st->want > 0) {
                            size_t n = (size_t)(end - p);
                            if (st->delim != 0) {
                                /* Switched to readline: search tail */
                                char *nl2 = (char *)memchr(p, st->delim, n);
                                if (nl2) {
                                    /* Dispatch one line, advance, keep tail */
                                    size_t line_len = (size_t)(nl2 - p) + 1;
                                    ccev_stream_cb cb2 = st->read_cb;
                                    void *ud2 = st->read_udata;
                                    char *old = st->rn_heap;
                                    st->rn_heap = NULL; st->rn_total = 0;
                                    st->want = 0; st->delim = 0;
                                    cb2(ud2, p, line_len, CCEV_OK);
                                    if (old) ccev__free_fn(old);
                                    p = nl2 + 1;
                                }
                            } else if (n >= st->want) {
                                /* Switched to readnum: dispatch, keep tail */
                                size_t sw = st->want;
                                ccev_stream_cb cb2 = st->read_cb;
                                void *ud2 = st->read_udata;
                                char *old = st->rn_heap;
                                st->rn_heap = NULL; st->rn_total = 0;
                                st->want = 0; st->delim = 0;
                                cb2(ud2, p, sw, CCEV_OK);
                                if (old) ccev__free_fn(old);
                                p += sw;
                            }
                            /* Save any remaining bytes to new mode's heap */
                            if (p < end && st->want > 0) {
                                size_t rem = (size_t)(end - p);
                                st->rn_heap = (char *)ccev__realloc_fn(
                                    NULL, st->want);
                                if (st->rn_heap) {
                                    size_t cp = rem < st->want ? rem : st->want;
                                    memcpy(st->rn_heap, p, cp);
                                    st->rn_total = cp;
                                }
                            }
                        }
                        return;
                    }
                    saved_delim = st->delim;
                    saved_want  = st->want;
                }

                /* Residual tail (no delimiter found) */
                if (p < end) {
                    size_t tail = (size_t)(end - p);
                    if (tail >= saved_want) {
                        st->read_cb(st->read_udata, p, saved_want, CCEV_ERR);
                        _read_drop(st);
                        ccev__sock_schedule_close(sock->loop, sock);
                        return;
                    }
                    st->rn_heap = (char *)ccev__realloc_fn(NULL, saved_want);
                    if (!st->rn_heap) {
                        st->read_cb(st->read_udata, NULL, 0, CCEV_ERR);
                        _read_drop(st);
                        ccev__sock_schedule_close(sock->loop, sock);
                        return;
                    }
                    memcpy(st->rn_heap, p, tail);
                    st->rn_total = tail;
                }
                ccev__sock_rearm(sock->loop, sock);
                return;
            }
            if (rc == CC_OPCODE_WAIT) { ccev__sock_rearm(sock->loop, sock); return; }
            st->read_cb(st->read_udata, NULL, 0, CCEV_ERR);
            _read_drop(st);
            ccev__sock_schedule_close(sock->loop, sock);
        }
        return;
    }

    /* ── Readnum mode (want > 0, delim == 0) ── */

    /* Partial heap continuation */
    if (st->rn_heap) {
        /* Check existing heap data first */
        if (st->rn_total >= st->want) {
            _readnum_done(st, st->rn_heap, st->want);
            goto retry_heap;
        }
        int nread = 0;
        size_t remain = st->want - st->rn_total;
        ccsocket_stcode_t rc = ccsocket_recv(sock->fd,
                                              st->rn_heap + st->rn_total,
                                              remain, &nread);
        if (rc == CC_OPCODE_OK && nread > 0) {
            st->rn_total += (size_t)nread;
            if (st->rn_total >= st->want) {
                _readnum_done(st, st->rn_heap, st->want);
                ccev__sock_rearm(sock->loop, sock);
                return;
            }
        }
        if (rc == CC_OPCODE_WAIT) { ccev__sock_rearm(sock->loop, sock); return; }
        st->read_cb(st->read_udata, NULL, 0, CCEV_ERR);
        _read_drop(st);
        ccev__sock_schedule_close(sock->loop, sock);
        return;
    }

    /* First recv: try stack */
    {
        int nread = 0;
        int to_read = (st->want < sizeof(stack)) ? (int)st->want
                                                  : (int)sizeof(stack);
        ccsocket_stcode_t rc = ccsocket_recv(sock->fd, stack, to_read, &nread);
        if (rc == CC_OPCODE_OK && nread > 0) {
            if ((size_t)nread >= st->want) {
                st->read_cb(st->read_udata, stack, st->want, CCEV_OK);
                _read_drop(st);
                ccev__sock_rearm(sock->loop, sock);
                return;
            }
            char *heap = (char *)ccev__realloc_fn(NULL, st->want);
            if (!heap) {
                st->read_cb(st->read_udata, NULL, 0, CCEV_ERR);
                _read_drop(st);
                ccev__sock_schedule_close(sock->loop, sock);
                return;
            }
            memcpy(heap, stack, (size_t)nread);
            st->rn_heap  = heap;
            st->rn_total = (size_t)nread;
            ccev__sock_rearm(sock->loop, sock);
            return;
        }
        if (rc == CC_OPCODE_WAIT) { ccev__sock_rearm(sock->loop, sock); return; }
        st->read_cb(st->read_udata, NULL, 0, CCEV_ERR);
        _read_drop(st);
        ccev__sock_schedule_close(sock->loop, sock);
    }

    /* Mode-switch guard may have populated rn_heap from stack data.
     * If heap has pending bytes, loop back to process them immediately
     * instead of waiting for the next epoll event. */
    if (st->rn_heap && st->rn_total > 0)
        goto retry_heap;
}

/* ════════════════════════════════════════════════════════════════
 *  Public API — stream lifecycle
 * ════════════════════════════════════════════════════════════════ */

ccev_stream_t *ccev_stream_open(ccev_sock_t *sock) {
    if (!sock || sock->closed || sock->in_closing) return NULL;

    /* Safe cast: ccev_sock_create() allocates via the ccev_sock_any_t
     * union, ensuring enough space for ccev_stream_t.  Since
     * ccev_sock_t is the first field of ccev_stream_t, the address
     * is the same.  Future variants join the same union. */
    ccev_stream_t *st = (ccev_stream_t *)sock;

    /* Zero out fields beyond the embedded sock */
    memset((char*)&st->sock + sizeof(ccev_sock_t), 0,
           sizeof(ccev_stream_t) - sizeof(ccev_sock_t));

    st->sock.upgraded = true;

    cclink_init(&st->wlist);
    st->sendfile_fd = -1;

    /* Take over sock's write callback.  Read callback is set
     * by ccev_stream_read() / ccev_stream_readnum(). */
    st->sock.wcb = _stream_on_writable;

    return st;
}

void ccev__stream_cleanup(ccev_stream_t *st) {
    if (!st) return;

    /* LISTENER and CONNECT sockets share the same union; their
     * stream fields are zero-initialized, not valid stream state.
     * Closing the listener via ccev_stream_close should not enter
     * stream resource cleanup. */
    if (st->sock.mode != CCEV_SOCK_INIT) return;

    /* Free write buffers */
    while (!cclink_empty(&st->wlist)) {
        cclink_node_t *bn = cclink_begin(&st->wlist);
        cclink_remove(&st->wlist, bn);
        ccev_buf_t *b = (ccev_buf_t *)((char*)bn - offsetof(ccev_buf_t, node));
        _buf_free(b);
    }

    /* Close sendfile fd if in progress */
    if (st->sendfile_fd > 0) {
        close(st->sendfile_fd);
        st->sendfile_fd = -1;
    }

    /* Free read state */
    _read_drop(st);
}

int ccev_stream_close(ccev_stream_t *st) {
    if (!st) return CCEV_ERR;

    /* Release stream resources, then schedule the sock for deferred close.
     * The union pointer is freed later by ccev__sock_free (base cleanup only). */
    bool already_closed = st->sock.closed;
    ccev__stream_cleanup(st);

    if (!already_closed)
        ccev__sock_schedule_close(st->sock.loop, &st->sock);
    return already_closed ? CCEV_ERR : CCEV_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  Public API — write
 * ════════════════════════════════════════════════════════════════ */

int ccev_stream_write(ccev_stream_t *st, const void *data, size_t len,
                       ccev_send_cb cb, void *udata) {
    if (!st || st->sock.closed) return CCEV_ERR;
    if (!data || !len) return 0;

    /* Fast path: wlist empty, no callback → try direct send.
     * Avoids alloc+memcpy+sendv+free for the common small-response case. */
    if (cclink_empty(&st->wlist) && !st->pending_write && !cb) {
        int sent = 0;
        ccsocket_stcode_t rc = ccsocket_send(st->sock.fd, data,
                                              len, &sent);
        if (rc == CC_OPCODE_OK && (size_t)sent == (size_t)len) {
            _stream_invoke_send_cb(st);     /* fire global drain cb */
            return (int)len;                /* zero allocation */
        }
        /* Partial or EAGAIN → fall through to normal buffered path.
         * CC_OPCODE_WAIT with partial bytes: the already-sent portion
         * is consumed; only the remainder needs buffering. */
        if (rc == CC_OPCODE_OK && sent > 0) {
            data = (const char *)data + sent;
            len -= (size_t)sent;
        }
    }

    /* Buffer the data */
    ccev_buf_t *buf = _buf_alloc(data, len, cb, udata);
    if (!buf) return CCEV_ERR;

    cclink_push_back(&st->wlist, &buf->node);
    st->wbuf_len += len;

    /* Try flush.  Set pending_write first so ccev__stream_flush does
     * not bail on the !st->pending_write guard (re-entrancy safety). */
    if (!st->pending_write) {
        st->pending_write = true;
        ccev__stream_flush(st->sock.loop, st);
    }

    return (int)len;
}

int ccev_stream_write_batch(ccev_stream_t *st, const void *data, size_t len,
                             bool done, ccev_send_cb cb, void *udata) {
    if (!st || st->sock.closed) return CCEV_ERR;

    if (data && len > 0) {
        ccev_buf_t *buf = _buf_alloc(data, len, cb, udata);
        if (!buf) return CCEV_ERR;
        cclink_push_back(&st->wlist, &buf->node);
        st->wbuf_len += len;
    }

    if (done) {
        if (!cclink_empty(&st->wlist)) {
            st->pending_write = true;
            ccev__stream_flush(st->sock.loop, st);
        } else {
            if (cb) cb(udata);
        }
    }

    return (int)len;
}

int ccev_stream_flush(ccev_stream_t *st) {
    if (!st || st->sock.closed) return CCEV_ERR;

    if (!cclink_empty(&st->wlist)) {
        st->pending_write = true;
        ccev__stream_flush(st->sock.loop, st);
    }

    return CCEV_OK;
}

int ccev_stream_sendfile(ccev_stream_t *st, const char *path,
                          ccev_send_cb cb, void *udata) {
    if (!st || st->sock.closed) return CCEV_ERR;

    int fd;
#ifdef _WIN32
    fd = open(path, O_RDONLY | O_BINARY);
#else
    fd = open(path, O_RDONLY | O_CLOEXEC);
#endif
    if (fd < 0) return CCEV_ERR;

    if (cb) { st->sf_cb = cb; st->sf_udata = udata; }

    ccsocket_sendf_state_t rc = ccsocket_sendfile(st->sock.fd, fd);

    if (rc == CC_SENDALL) {
        close(fd);
        if (st->sf_cb) {
            ccev_send_cb scb = st->sf_cb;
            void *sud = st->sf_udata;
            st->sf_cb = NULL;
            scb(sud);
        }
        return CCEV_OK;
    }

    if (rc == CC_SENDWAIT) {
        st->sendfile_fd = fd;
        ccev__sock_rearm(st->sock.loop, &st->sock);
        return CCEV_OK;
    }

    close(fd);
    return CCEV_ERR;
}

void ccev_stream_set_send_cb(ccev_stream_t *st, ccev_send_cb cb, void *udata) {
    if (!st) return;
    st->send_cb   = cb;
    st->send_udata = udata;
}

size_t ccev_stream_wbuf_len(const ccev_stream_t *st) {
    return st ? st->wbuf_len : 0;
}

/* ════════════════════════════════════════════════════════════════
 *  Read setup — cross-mode heap reuse
 * ════════════════════════════════════════════════════════════════ */

static int _read_setup(ccev_stream_t *st, size_t want, char delim,
                        int timeout_ms, ccev_stream_cb cb, void *udata) {
    /* Try to satisfy the new mode from an existing partial heap. */
    if (st->rn_heap && st->rn_total > 0) {
        if (delim != 0) {
            /* Switching to readline — search existing data */
            char *found = (char *)memchr(st->rn_heap, delim, st->rn_total);
            if (found) {
                _readline_done(st, st->rn_heap,
                                (size_t)(found - st->rn_heap) + 1,
                                st->rn_total);
                return CCEV_OK;       /* synchronously satisfied */
            }
            if (st->rn_total >= want) {
                /* Overflow: no delim, already at/past limit */
                st->read_cb(st->read_udata, st->rn_heap, want, CCEV_ERR);
                _read_drop(st);
                return CCEV_OK;
            }
        } else {
            /* Switching to readnum — check if already enough */
            if (st->rn_total >= want) {
                _readnum_done(st, st->rn_heap, want);
                return CCEV_OK;       /* synchronously satisfied */
            }
        }
        /* Not satisfied yet — grow heap if needed, continue accumulating */
        if (st->want < want) {
            char *nb = (char *)ccev__realloc_fn(st->rn_heap, want);
            if (!nb) { _read_drop(st); return CCEV_ERR; }
            st->rn_heap = nb;
        }
    } else {
        _read_drop(st);
    }

    st->read_cb    = cb;
    st->read_udata = udata;
    st->want       = want;
    st->delim      = delim;
    st->timeout_ms = timeout_ms;
    st->sock.rcb   = _stream_on_readable;

    if (timeout_ms > 0 && !st->read_timer)
        st->read_timer = ccev_timer_add(st->sock.loop, (uint64_t)timeout_ms,
                                         CCEV_TIMER_ONCE, _read_timeout_cb, st);

    /* Re-arm only when not already armed (re-entrant callbacks from
     * _stream_on_readable already did the re-arm on completion). */
    if (!(st->sock.events & CCEV_POLL_READ))
        ccev__sock_rearm(st->sock.loop, &st->sock);
    return CCEV_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  Public API — stream reader
 * ════════════════════════════════════════════════════════════════ */

int ccev_stream_read(ccev_stream_t *st, size_t limit, int timeout_ms,
                      ccev_stream_cb cb, void *udata) {
    if (!st || !cb || st->sock.closed) return CCEV_ERR;
    _read_drop(st);                          /* raw never accumulates */
    st->read_cb    = cb;
    st->read_udata = udata;
    st->limit      = limit;
    st->timeout_ms = timeout_ms;
    if (timeout_ms > 0)
        st->read_timer = ccev_timer_add(st->sock.loop, (uint64_t)timeout_ms,
                                         CCEV_TIMER_ONCE, _read_timeout_cb, st);
    st->sock.rcb = _stream_on_readable;
    ccev__sock_rearm(st->sock.loop, &st->sock);
    return CCEV_OK;
}

int ccev_stream_readnum(ccev_stream_t *st, size_t n, int timeout_ms,
                         ccev_stream_cb cb, void *udata) {
    if (!st || !cb || st->sock.closed || n == 0) return CCEV_ERR;
    return _read_setup(st, n, 0, timeout_ms, cb, udata);
}

int ccev_stream_readline(ccev_stream_t *st, char delim, size_t maxlen,
                          int timeout_ms, ccev_stream_cb cb, void *udata) {
    if (!st || !cb || st->sock.closed || maxlen == 0) return CCEV_ERR;
    return _read_setup(st, maxlen, delim, timeout_ms, cb, udata);
}

void ccev_stream_read_stop(ccev_stream_t *st) {
    if (!st) return;
    _read_drop(st);
    st->read_cb = NULL;
}

void ccev_stream_set_close_cb(ccev_stream_t *st, ccev_close_cb cb, void *udata) {
    if (!st) return;
    ccev_sock_set_close_cb(&st->sock, cb, udata);
}
