/**
 * @file ccev_stream.c
 * @brief Stream — write buffering, sendfile, stream reader (readline/readnum).
 *
 * @author CandyMi
 * @license MIT
 *
 * The stream embeds a ccev_sock_t by value and takes over its rcb/wcb
 * to drive buffered I/O and the stream reader state machine.
 *
 * Write path:
 *   ccev_stream_write() appends data to wlist, then tries a non-blocking
 *   sendv.  If EAGAIN, arms EPOLLOUT.  Per-buffer callbacks fire when
 *   that specific buffer chunk has been flushed.
 *
 * Read path:
 *   ccev_stream_readline/readnum temporarily replace the sock's rcb
 *   with an internal accumulation function.  When the delimiter or
 *   requested byte count is reached, the user callback fires once and
 *   the original rcb is restored.  A timeout timer can be set to
 *   cancel the read if no data arrives within the deadline.
 *
 * Sendfile:
 *   Opens the file, calls ccsocket_sendfile() in a loop on EPOLLOUT.
 *
 * File: ccev_stream.c
 */

#include "ccev_internal.h"
#include <fcntl.h>

#ifdef _WIN32
#include <io.h>
#endif

#ifndef IOV_MAX
#  define IOV_MAX 128
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
    /* Single allocation: ccev_buf_t header + data payload contiguous */
    ccev_buf_t *buf = (ccev_buf_t *)ccev__realloc_fn(
        NULL, sizeof(ccev_buf_t) + len);
    if (!buf) return NULL;
    buf->data    = (char*)buf + sizeof(ccev_buf_t);
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
            ccsocket_set_iov_buf(&iov[niov], 0, (char*)b->data + b->offset);
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
 *  Stream reader — forward declarations
 * ════════════════════════════════════════════════════════════════ */

static void _stream_on_readable(ccev_sock_t *sock, int events);

/* ════════════════════════════════════════════════════════════════
 *  Stream reader — timeout callback
 * ════════════════════════════════════════════════════════════════ */

static void _stream_timeout_cb(void *udata) {
    ccev_stream_reader_t *rd = (ccev_stream_reader_t *)udata;
    ccev_sock_t *sock = rd->sock;
    if (!sock || sock->closed) return;

    ccev_stream_cb cb = rd->cb;
    void *cb_udata   = rd->udata;

    /* Restore original rcb */
    sock->rcb = rd->old_rcb;

    /* Free reader resources */
    rd->timer = NULL; /* timer is self-deleting (ONCE) or already deleted */
    ccev__free_fn(rd->buf);
    ccev__free_fn(rd);

    /* Clear reader on stream */
    ccev_stream_t *st = _sock_to_stream(sock);
    st->reader = NULL;

    if (cb) cb(cb_udata, NULL, 0, CCEV_ERR);
}

/* ════════════════════════════════════════════════════════════════
 *  Stream reader — dispatch
 * ════════════════════════════════════════════════════════════════ */

static void _reader_dispatch(ccev_stream_reader_t *rd,
                              size_t consumed, int status) {
    ccev_sock_t      *sock = rd->sock;
    ccev_stream_t    *st   = _sock_to_stream(sock);
    ccev_stream_cb    cb   = rd->cb;
    void             *ud   = rd->udata;
    size_t            remain = rd->len - consumed;

    /* Cancel timeout timer if active */
    if (rd->timer) {
        ccev_timer_del(sock->loop, rd->timer);
        rd->timer = NULL;
    }

    /* Restore original rcb */
    sock->rcb = rd->old_rcb;

    /* Save where the consumed data starts, then advance position. */
    size_t data_off = rd->pos;
    rd->pos += consumed;
    rd->len  = remain;
    rd->cb   = NULL; /* Mark idle — re-entrant readline/num can take over */

    /* Fire user callback — rd->buf[data_off .. data_off+consumed-1] is stable
     * because rd->buf is not realloced during the callback.  A re-entrant
     * _reader_start searches rd->buf[rd->pos ..] which is past the consumed
     * region, so the callback's pointer remains valid. */
    cb(ud, rd->buf + data_off, consumed, status);

    /* Compact: when pos has grown past half the buffer, move remaining
     * data to the front so the recv target area stays large. */
    if (rd->pos > rd->cap / 2 && rd->len > 0) {
        memmove(rd->buf, rd->buf + rd->pos, rd->len);
        rd->pos = 0;
    }

    /* Free reader if no remaining data and no re-entrant read started */
    if (remain == 0 && st->reader == rd && rd->cb == NULL) {
        ccev__free_fn(rd->buf);
        ccev__free_fn(rd);
        st->reader = NULL;
    }
}

/* ════════════════════════════════════════════════════════════════
 *  Stream reader — start (internal)
 * ════════════════════════════════════════════════════════════════ */

static int _reader_start(ccev_stream_t *st, size_t want,
                          char delim, bool is_n,
                          int timeout_ms,
                          ccev_stream_cb cb, void *udata) {
    if (!st || !cb || want == 0 || st->sock.closed) return CCEV_ERR;

    ccev_stream_reader_t *rd = st->reader;

    /* Cancel active reader if one exists */
    if (rd && rd->cb) {
        ccev_stream_read_stop(st);
        rd = NULL;
    }

    /* Create or reuse reader */
    if (!rd) {
        rd = (ccev_stream_reader_t *)ccev__realloc_fn(NULL,
                                        sizeof(ccev_stream_reader_t));
        if (!rd) return CCEV_ERR;
        memset(rd, 0, sizeof(*rd));
        rd->buf = (char *)ccev__realloc_fn(NULL, want + 1);
        if (!rd->buf) { ccev__free_fn(rd); return CCEV_ERR; }
        rd->cap          = want + 1;
        rd->old_rcb      = st->sock.rcb;
        rd->sock         = &st->sock;
        st->reader       = rd;
    } else {
        /* Idle reader — grow buffer if needed */
        if (rd->cap < want + 1) {
            char *nb = (char *)ccev__realloc_fn(rd->buf, want + 1);
            if (!nb) { ccev_stream_read_stop(st); return CCEV_ERR; }
            rd->buf = nb;
            rd->cap = want + 1;
        }
        rd->old_rcb = st->sock.rcb;
    }

    rd->want      = want;
    rd->delim     = delim;
    rd->is_n      = is_n;
    rd->cb        = cb;
    rd->udata     = udata;

    /* Try to satisfy from existing buffered data */
    size_t consumed = 0;
    if (is_n) {
        if (rd->len >= want) consumed = want;
    } else {
        for (size_t i = 0; i < rd->len; i++) {
            if (rd->buf[rd->pos + i] == delim) { consumed = i + 1; break; }
        }
        if (consumed == 0 && rd->len >= want) {
            consumed = want;
            _reader_dispatch(rd, consumed, CCEV_ERR);
            return CCEV_OK;
        }
    }

    if (consumed > 0) {
        _reader_dispatch(rd, consumed, CCEV_OK);
        return CCEV_OK;
    }

    /* Not satisfied — compact buffer to maximise recv space, then arm */
    if (rd->pos > 0 && rd->len > 0) {
        memmove(rd->buf, rd->buf + rd->pos, rd->len);
    }
    rd->pos = 0;
    st->sock.rcb = _stream_on_readable;
    ccev__sock_rearm(st->sock.loop, &st->sock);

    /* Set read timeout */
    if (timeout_ms > 0) {
        rd->timer = ccev_timer_add(st->sock.loop, (uint64_t)timeout_ms,
                                    CCEV_TIMER_ONCE, _stream_timeout_cb, rd);
    }

    return CCEV_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  Stream reader — internal recv callback (replaces sock->rcb)
 * ════════════════════════════════════════════════════════════════ */

static void _stream_on_readable(ccev_sock_t *sock, int events) {
    (void)events;
    if (!sock || sock->closed) return;
    ccev_stream_t *st = _sock_to_stream(sock);
    ccev_stream_reader_t *rd = st->reader;
    if (!rd) return;

    int nread = 0;
    ccsocket_stcode_t rc = ccsocket_recv(sock->fd, rd->buf + rd->pos + rd->len,
                                          rd->cap - rd->pos - rd->len, &nread);

    /* EAGAIN — re-arm and return */
    if (rc == CC_OPCODE_WAIT) {
        ccev__sock_rearm(sock->loop, sock);
        return;
    }

    /* Error or EOF */
    if (rc != CC_OPCODE_OK || nread <= 0) {
        ccev_stream_cb cb = rd->cb;
        void *cb_udata    = rd->udata;
        /* Cancel timeout timer */
        if (rd->timer) {
            ccev_timer_del(sock->loop, rd->timer);
            rd->timer = NULL;
        }
        sock->rcb = rd->old_rcb;
        ccev__free_fn(rd->buf);
        ccev__free_fn(rd);
        st->reader = NULL;
        if (cb) cb(cb_udata, NULL, 0, CCEV_ERR);
        return;
    }

    rd->len += (size_t)nread;

    /* Check condition */
    size_t consumed = 0;
    int    status   = CCEV_OK;

    if (rd->is_n) {
        if (rd->len >= rd->want) {
            consumed = rd->want;
        } else {
            ccev__sock_rearm(sock->loop, sock);
            return;
        }
    } else {
        for (size_t i = 0; i < rd->len; i++) {
            if (rd->buf[rd->pos + i] == rd->delim) {
                consumed = i + 1;
                break;
            }
        }
        if (consumed == 0) {
            if (rd->len >= rd->want) {
                consumed = rd->want;
                status   = CCEV_ERR;
            } else {
                ccev__sock_rearm(sock->loop, sock);
                return;
            }
        }
    }

    _reader_dispatch(rd, consumed, status);
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

    cclink_init(&st->wlist);
    st->sendfile_fd = -1;
    st->reader      = NULL;

    /* Take over sock's event callbacks */
    st->sock.rcb = _stream_on_readable;
    st->sock.wcb = _stream_on_writable;

    return st;
}

void ccev__stream_cleanup(ccev_stream_t *st) {
    if (!st) return;

    /* Free write buffers */
    while (!cclink_empty(&st->wlist)) {
        cclink_node_t *bn = cclink_begin(&st->wlist);
        cclink_remove(&st->wlist, bn);
        ccev_buf_t *b = (ccev_buf_t *)((char*)bn - offsetof(ccev_buf_t, node));
        _buf_free(b);
    }

    /* Close sendfile fd if in progress */
    if (st->sendfile_fd >= 0) {
        close(st->sendfile_fd);
        st->sendfile_fd = -1;
    }

    /* Free stream reader if active */
    if (st->reader) {
        if (st->reader->timer)
            ccev_timer_del(st->sock.loop, st->reader->timer);
        ccev__free_fn(st->reader->buf);
        ccev__free_fn(st->reader);
        st->reader = NULL;
    }
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
 *  Public API — stream reader
 * ════════════════════════════════════════════════════════════════ */

void ccev_stream_read_stop(ccev_stream_t *st) {
    if (!st || !st->reader) return;
    ccev_stream_reader_t *rd = st->reader;

    /* Cancel timeout timer */
    if (rd->timer) {
        ccev_timer_del(st->sock.loop, rd->timer);
        rd->timer = NULL;
    }

    st->sock.rcb = rd->old_rcb;
    ccev__free_fn(rd->buf);
    ccev__free_fn(rd);
    st->reader = NULL;
}

int ccev_stream_readline(ccev_stream_t *st, char delim, size_t maxlen,
                          int timeout_ms, ccev_stream_cb cb, void *udata) {
    return _reader_start(st, maxlen, delim, false, timeout_ms, cb, udata);
}

int ccev_stream_readnum(ccev_stream_t *st, size_t n,
                         int timeout_ms, ccev_stream_cb cb, void *udata) {
    return _reader_start(st, n, 0, true, timeout_ms, cb, udata);
}

void ccev_stream_set_close_cb(ccev_stream_t *st, ccev_close_cb cb, void *udata) {
    if (!st) return;
    ccev_sock_set_close_cb(&st->sock, cb, udata);
}
