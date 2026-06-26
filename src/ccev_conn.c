/**
 * @file ccev_conn.c
 * @brief Connection management — wrapping fds, I/O, buffering, and
 *        epoll event lifecycle (all ONESHOT, auto-rearm).
 */

#include "ccev_internal.h"
#include <string.h>
#include <fcntl.h>

#ifdef _WIN32
/* open/close for sendfile path */
#include <io.h>
#endif

/* Maximum iovec entries for scatter/gather sendv.
 * IOV_MAX from <limits.h> / <sys/uio.h> on POSIX; fallback for portability. */
#ifndef IOV_MAX
#  define IOV_MAX 128
#endif

int ccev__conn_mod_internal(ccev_loop_t *loop, ccev_conn_t *conn,
                             int events) {
    if (conn->closed) return CCEV_ERR;

    struct epoll_event ee;
    memset(&ee, 0, sizeof(ee));
    ee.events   = events | EPOLLONESHOT;
    ee.data.ptr = conn;

    int op = (conn->reg_events == 0) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    if (epoll_ctl(loop->epfd, op, (int)(intptr_t)conn->fd, &ee) < 0) {
        if (op == EPOLL_CTL_ADD) {
            /* Already exists — try MOD */
            op = EPOLL_CTL_MOD;
            if (epoll_ctl(loop->epfd, op, (int)(intptr_t)conn->fd, &ee) < 0)
                return CCEV_ERR;
        } else {
            return CCEV_ERR;
        }
    }
    conn->reg_events = events;
    return CCEV_OK;
}

void ccev__conn_rearm(ccev_loop_t *loop, ccev_conn_t *conn) {
    if (conn->closed) return;

    int want = 0;
    if (conn->recv_cb)                  want |= EPOLLIN;
    if (conn->pending_write)            want |= EPOLLOUT;
    if (conn->sendfile.sendfile_fd >= 0) want |= EPOLLOUT;

    if (want && want != conn->reg_events) {
        ccev__conn_mod_internal(loop, conn, want);
    }
}

/* ════════════════════════════════════════════════════════════════
 *  Write buffer management
 * ════════════════════════════════════════════════════════════════ */

static ccev_buf_t *ccev__buf_alloc(ccev_loop_t *loop, const void *data,
                                    size_t len) {
    /* Single allocation: ccev_buf_t header + data payload contiguous.
     * buf->data points into the same block, avoiding a second malloc. */
    ccev_buf_t *buf = (ccev_buf_t *)loop->realloc_fn(
        NULL, sizeof(ccev_buf_t) + len);
    if (!buf) return NULL;
    buf->data = (char*)buf + sizeof(ccev_buf_t);
    memcpy(buf->data, data, len);
    buf->len    = len;
    buf->offset = 0;
    return buf;
}

static void ccev__buf_free(ccev_loop_t *loop, ccev_buf_t *buf) {
    (void)loop;
    /* data is part of the same allocation — single free */
    loop->free_fn(buf);
}

static void ccev__invoke_send_cb(ccev_conn_t *conn) {
    if (conn->send_cb) {
        ccev_send_cb cb = conn->send_cb;
        void *ud = conn->send_udata;
        conn->send_cb = NULL;
        cb(ud);
    }
}

void ccev__conn_flush(ccev_loop_t *loop, ccev_conn_t *conn) {
    if (conn->closed || !conn->pending_write) return;

    /* Build iovec */
    ccsocket_iovec_t iov[IOV_MAX];
    int niov = 0;
    cclink_node_t *n = cclink_begin(&conn->wbuf_list);
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
        conn->pending_write = false;
        return;
    }

    int sent = 0;
    ccsocket_stcode_t rc = ccsocket_sendv(conn->fd, iov, niov, &sent);
    if (rc == CC_OPCODE_OK || rc == CC_OPCODE_WAIT) {
        int remaining_sent = sent;
        while (remaining_sent > 0) {
            cclink_node_t *head = cclink_begin(&conn->wbuf_list);
            if (!head) break;
            ccev_buf_t *b = (ccev_buf_t *)((char*)head - offsetof(ccev_buf_t, node));
            size_t avail = b->len - b->offset;
            if (avail <= (size_t)remaining_sent) {
                remaining_sent -= (int)avail;
                conn->wbuf_len -= avail;
                cclink_remove(&conn->wbuf_list, head);
                ccev__buf_free(loop, b);
            } else {
                b->offset += (size_t)remaining_sent;
                conn->wbuf_len -= (size_t)remaining_sent;
                remaining_sent = 0;
            }
        }

        if (cclink_empty(&conn->wbuf_list)) {
            conn->pending_write = false;
            ccev__invoke_send_cb(conn);
        } else {
            /* Still data — re-arm */
            conn->pending_write = true;
            ccev__conn_rearm(loop, conn);
        }
    } else {
        conn->pending_write = false;
        ccev__conn_schedule_close(loop, conn);
    }
}

/* ════════════════════════════════════════════════════════════════
 *  Closing lifecycle
 * ════════════════════════════════════════════════════════════════ */

void ccev__conn_free(ccev_loop_t *loop, ccev_conn_t *conn) {
    if (!conn) return;
    /* Close sendfile file fd if still in progress */
    if (conn->sendfile.sendfile_fd >= 0) {
        close(conn->sendfile.sendfile_fd);
        conn->sendfile.sendfile_fd = -1;
    }
    /* DNS query state — allocated by ccev_dns_resolve, must free here */
    /* Free internal per-connection state for subsystem-owned conns */
    if (conn->recv_udata &&
        (conn->type == CCEV_CONN_DNS || conn->type == CCEV_CONN_ICMP))
        loop->free_fn(conn->recv_udata);
    /* Close socket */
    if (conn->fd != (ccsocket_t)-1) ccsocket_close(conn->fd);
    conn->fd = (ccsocket_t)-1;
    /* Free write buffers — buf->data is part of the same allocation
     * (see ccev__buf_alloc), so one free per buf suffices. */
    while (!cclink_empty(&conn->wbuf_list)) {
        cclink_node_t *bn = cclink_begin(&conn->wbuf_list);
        cclink_remove(&conn->wbuf_list, bn);
        ccev_buf_t *b = (ccev_buf_t *)((char*)bn - offsetof(ccev_buf_t, node));
        loop->free_fn(b);
    }
    loop->free_fn(conn);
}

void ccev__conn_schedule_close(ccev_loop_t *loop, ccev_conn_t *conn) {
    if (conn->closed || conn->in_closing) return;
    conn->closed     = true;
    conn->in_closing = true;

    /* Remove from epoll */
    epoll_ctl(loop->epfd, EPOLL_CTL_DEL, (int)(intptr_t)conn->fd, NULL);
    conn->reg_events = 0;

    loop->conn_count--;

    /* Remove from all_conns first, then push to closing.
     * This prevents the lnode from being in two lists simultaneously,
     * which would corrupt the doubly-linked list of all_conns. */
    cclist_remove(&loop->all_conns, &conn->lnode);
    cclist_push_back(&loop->closing, &conn->lnode);
}

/* ════════════════════════════════════════════════════════════════
 *  Public API
 * ════════════════════════════════════════════════════════════════ */

ccev_conn_t *ccev_conn_create(ccev_loop_t *loop, ccsocket_t fd, void *udata) {
    if (!loop || fd == (ccsocket_t)-1) return NULL;

    ccev_conn_t *conn = (ccev_conn_t *)loop->realloc_fn(NULL, sizeof(ccev_conn_t));
    if (!conn) return NULL;

    memset(conn, 0, sizeof(ccev_conn_t));
    conn->type   = CCEV_CONN_NORMAL;
    conn->loop   = loop;
    conn->fd     = fd;
    conn->udata  = udata;

    /* External fds should be non-blocking in a reactor */
    ccsocket_set_nonblock(fd, true);
    ccsocket_set_cloexec(fd, true);

    /* Init intrusive links */
    /* cclink is a singly-linked list; init the head */
    cclink_init(&conn->wbuf_list);
    /* Sendfile state: no file in progress */
    conn->sendfile.sendfile_fd = -1;

    /* Add to all_conns list (iteration) */
    cclist_push_back(&loop->all_conns, &conn->lnode);

    loop->conn_count++;
    return conn;
}

int ccev_conn_recv(ccev_conn_t *conn, void *buf, size_t len,
                    ccev_recv_cb cb, void *udata) {
    if (!conn || conn->closed) return CCEV_ERR;

    /* Update callback */
    if (cb) { conn->recv_cb = cb; conn->recv_udata = udata; }

    /* buf=NULL, len=0, cb=NULL: clear callback + disarm */
    if (!buf && !len && !cb) {
        conn->recv_cb = NULL;
        ccev__conn_rearm(conn->loop, conn);
        return CCEV_OK;
    }

    /* buf=NULL, len=0, cb!=NULL: register event only */
    if (!buf && !len) {
        if (conn->recv_cb) ccev__conn_rearm(conn->loop, conn);
        return CCEV_OK;
    }

    /* Speculative non-blocking read */
    int n = 0;
    ccsocket_stcode_t rc = ccsocket_recv(conn->fd, buf, (int)len, &n);

    if (rc == CC_OPCODE_OK) {
        /* Data available */
        if (conn->recv_cb && cb) conn->recv_cb(conn->recv_udata);
        return n;
    }

    if (rc == CC_OPCODE_WAIT) {
        /* Would block — arm epoll if a callback is registered */
        if (conn->recv_cb) {
            ccev__conn_rearm(conn->loop, conn);
            /* No data yet; epoll re-armed. Caller must wait for the
             * callback to fire.  Return CCEV_OK (not 1) so callers
             * do not mistake the value for bytes read. */
            return CCEV_OK;
        }
        /* Mode 2: pure sync read, no callback — no way to be notified
         * when data arrives.  Return CCEV_ERR so the caller doesn't
         * misinterpret a positive return as having data in buf. */
        return CCEV_ERR;
    }

    /* n=0 on EOF (CC_OPCODE_OK, 0 bytes) */
    if (n == 0) return 0;

    return CCEV_ERR;
}

int ccev_conn_send(ccev_conn_t *conn, const void *data, size_t len,
                    ccev_send_cb cb, void *udata) {
    if (!conn || conn->closed) return CCEV_ERR;
    if (!data || !len) return 0;

    int sent = 0;
    ccsocket_stcode_t rc = ccsocket_send(conn->fd, data, (int)len, &sent);

    if (rc == CC_OPCODE_OK || rc == CC_OPCODE_WAIT) {
        if (cb) { conn->send_cb = cb; conn->send_udata = udata; }
        if (sent > 0 && (size_t)sent == len) {
            /* Fully written */
            ccev__invoke_send_cb(conn);
            return (int)len;
        }

        /* Buffer the rest */
        size_t remaining = len - (sent > 0 ? (size_t)sent : 0);
        if (remaining > 0) {
            const char *remaining_data = (const char *)data + (sent > 0 ? sent : 0);
            ccev_buf_t *buf = ccev__buf_alloc(conn->loop, remaining_data, remaining);
            if (!buf) return CCEV_ERR;
            cclink_push_back(&conn->wbuf_list, &buf->node);
            conn->wbuf_len += remaining;
            conn->pending_write = true;
            ccev__conn_rearm(conn->loop, conn);
        }
        return (int)len;
    }
    return CCEV_ERR;
}

int ccev_conn_sendall(ccev_conn_t *conn, const void *data, size_t len,
                       bool done, ccev_send_cb cb, void *udata) {
    if (!conn || conn->closed) return CCEV_ERR;

    if (!done) {
        if (cb) { conn->send_cb = cb; conn->send_udata = udata; }
        /* Buffer only */
        if (data && len > 0) {
            ccev_buf_t *buf = ccev__buf_alloc(conn->loop, data, len);
            if (!buf) return CCEV_ERR;
            cclink_push_back(&conn->wbuf_list, &buf->node);
            conn->wbuf_len += len;
        }
        return (int)len;
    }

    /* done=true: flush */
    if (cb) { conn->send_cb = cb; conn->send_udata = udata; }

    if (data && len > 0) {
        ccev_buf_t *buf = ccev__buf_alloc(conn->loop, data, len);
        if (!buf) return CCEV_ERR;
        cclink_push_back(&conn->wbuf_list, &buf->node);
        conn->wbuf_len += len;
    }

    if (!cclink_empty(&conn->wbuf_list)) {
        conn->pending_write = true;
        ccev__conn_flush(conn->loop, conn);
    } else {
        ccev__invoke_send_cb(conn);
    }

    return (int)len;
}

int ccev_conn_close(ccev_conn_t *conn) {
    if (!conn || conn->closed) return CCEV_ERR;
    /* Unified close path: schedule deferred cleanup.
     * close_cb fires once in step 6 of the dispatch loop;
     * the fd is closed and memory freed by ccev__conn_free. */
    ccev__conn_schedule_close(conn->loop, conn);
    return CCEV_OK;
}

void ccev_conn_set_close_cb(ccev_conn_t *conn, ccev_close_cb cb, void *udata) {
    if (!conn) return;
    conn->close_cb   = cb;
    conn->close_udata = udata;
}

void *ccev_conn_get_udata(ccev_conn_t *conn) {
    return conn ? conn->udata : NULL;
}

void ccev_conn_set_udata(ccev_conn_t *conn, void *udata) {
    if (conn) conn->udata = udata;
}

/* ── Sendfile continuation (called from dispatch on EPOLLOUT) ── */

void ccev__conn_sendfile_continue(ccev_loop_t *loop, ccev_conn_t *conn) {
    if (conn->closed || conn->sendfile.sendfile_fd < 0) return;

    int fd = conn->sendfile.sendfile_fd;
    ccsocket_sendf_state_t rc = ccsocket_sendfile(conn->fd, fd);

    if (rc == CC_SENDALL) {
        close(fd);
        conn->sendfile.sendfile_fd = -1;
        if (conn->sendfile.cb) {
            ccev_send_cb send_cb = conn->sendfile.cb;
            void *send_udata = conn->sendfile.udata;
            conn->sendfile.cb = NULL;
            send_cb(send_udata);
        }
        /* Re-arm to clear EPOLLOUT if no other pending I/O */
        ccev__conn_rearm(loop, conn);
    } else if (rc == CC_SENDWAIT) {
        /* Still waiting — next EPOLLOUT will retry */
    } else {
        /* CC_SENDERROR */
        close(fd);
        conn->sendfile.sendfile_fd = -1;
        ccev__conn_schedule_close(loop, conn);
    }
}

int ccev_conn_sendfile(ccev_conn_t *conn, const char *path,
                        ccev_send_cb cb, void *udata) {
    if (!conn || conn->closed) return CCEV_ERR;

    /* Open the file (read-only, close-on-exec) */
    int fd;
#ifdef _WIN32
    fd = open(path, O_RDONLY | O_BINARY);
#else
    fd = open(path, O_RDONLY | O_CLOEXEC);
#endif
    if (fd < 0) return CCEV_ERR;

    /* Store completion callback */
    if (cb) { conn->sendfile.cb = cb; conn->sendfile.udata = udata; }

    /* Try non-blocking sendfile once.  On CC_SENDWAIT the kernel
     * buffer is full — store the fd and wait for EPOLLOUT. */
    ccsocket_sendf_state_t rc = ccsocket_sendfile(conn->fd, fd);

    if (rc == CC_SENDALL) {
        close(fd);
        if (conn->sendfile.cb) {
            ccev_send_cb send_cb = conn->sendfile.cb;
            void *send_udata = conn->sendfile.udata;
            conn->sendfile.cb = NULL;
            send_cb(send_udata);
        }
        return CCEV_OK;
    }

    if (rc == CC_SENDWAIT) {
        conn->sendfile.sendfile_fd = fd;
        /* Arm EPOLLOUT (combined with any existing EPOLLIN) */
        ccev__conn_rearm(conn->loop, conn);
        return CCEV_OK;
    }

    /* CC_SENDERROR */
    close(fd);
    return CCEV_ERR;
}

ccsocket_t ccev_conn_fd(ccev_conn_t *conn) {
    return conn ? conn->fd : INVALID_SOCKET;
}

int ccev_conn_count(ccev_loop_t *loop) {
    return loop ? loop->conn_count : 0;
}
