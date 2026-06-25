/**
 * @file ccev_conn.c
 * @brief Connection management — wrapping fds, I/O, buffering, and
 *        epoll event lifecycle (all ONESHOT, auto-rearm).
 */

#include "ccev_internal.h"
#include <string.h>

/* ════════════════════════════════════════════════════════════════
 *  cchashmap callbacks — keyed by fd
 * ════════════════════════════════════════════════════════════════ */

static uint64_t conn_hash_fn(const cchashmap_node_t *n, size_t seed) {
    (void)seed;
    ccev_conn_t *conn = CCHASHMAP_CONTAINER(n, ccev_conn_t, hnode);
    return (uint64_t)(intptr_t)conn->fd;
}
static bool conn_equal_fn(const cchashmap_node_t *a, const cchashmap_node_t *b) {
    ccev_conn_t *ca = CCHASHMAP_CONTAINER(a, ccev_conn_t, hnode);
    ccev_conn_t *cb = CCHASHMAP_CONTAINER(b, ccev_conn_t, hnode);
    return ca->fd == cb->fd;
}

/* ════════════════════════════════════════════════════════════════
 *  Event flag conversion (internal only)
 * ════════════════════════════════════════════════════════════════ */

int ccev__epoll_from_ccev(int flags) {
    int e = 0;
    if (flags & CCEV_INTERNAL_READ)  e |= EPOLLIN;
    if (flags & CCEV_INTERNAL_WRITE) e |= EPOLLOUT;
    return e;
}

int ccev__epoll_to_ccev(int epoll_events) {
    int f = 0;
    if (epoll_events & EPOLLIN)   f |= CCEV_INTERNAL_READ;
    if (epoll_events & EPOLLOUT)  f |= CCEV_INTERNAL_WRITE;
    return f;
}

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
    if (conn->recv_cb)        want |= EPOLLIN;
    if (conn->pending_write)  want |= EPOLLOUT;

    conn->want_events = want;

    if (want && want != conn->reg_events) {
        ccev__conn_mod_internal(loop, conn, want);
    }
}

/* ════════════════════════════════════════════════════════════════
 *  Write buffer management
 * ════════════════════════════════════════════════════════════════ */

static ccev_buf_t *ccev__buf_alloc(ccev_loop_t *loop, const void *data,
                                    size_t len) {
    ccev_buf_t *buf = (ccev_buf_t *)loop->realloc_fn(NULL, sizeof(ccev_buf_t));
    if (!buf) return NULL;
    buf->data = loop->realloc_fn(NULL, len);
    if (!buf->data) { loop->free_fn(buf); return NULL; }
    memcpy(buf->data, data, len);
    buf->len    = len;
    buf->offset = 0;
    return buf;
}

static void ccev__buf_free(ccev_loop_t *loop, ccev_buf_t *buf) {
    if (buf->data) loop->free_fn(buf->data);
    loop->free_fn(buf);
}

void ccev__conn_flush(ccev_loop_t *loop, ccev_conn_t *conn) {
    if (conn->closed || !conn->pending_write) return;

    /* Build iovec */
    ccsocket_iovec_t iov[64];
    int niov = 0;
    cclink_node_t *n = cclink_begin(&conn->wbuf_list);
    while (n && niov < 64) {
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
            conn->batch_done    = false;
            if (conn->send_cb) {
                ccev_send_cb cb = conn->send_cb;
                void *ud = conn->send_udata;
                conn->send_cb = NULL;
                cb(ud);
            }
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
    /* Close socket */
    if (conn->fd != (ccsocket_t)-1) ccsocket_close(conn->fd);
    conn->fd = (ccsocket_t)-1;
    /* Free write buffers */
    while (!cclink_empty(&conn->wbuf_list)) {
        cclink_node_t *bn = cclink_begin(&conn->wbuf_list);
        cclink_remove(&conn->wbuf_list, bn);
        ccev_buf_t *b = (ccev_buf_t *)((char*)bn - offsetof(ccev_buf_t, node));
        if (b->data) loop->free_fn(b->data);
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

    /* Remove from hashmap */
    cchashmap_del(&loop->conns, &conn->hnode);
    loop->conn_count--;

    /* Add lnode (cclist_node_t) to closing list */
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

    /* Init intrusive links */
    /* cclink is a singly-linked list; init the head */
    cclink_init(&conn->wbuf_list);

    /* Initialize hashmap on first use */
    if (cchashmap_size(&loop->conns) == 0 && loop->conn_count == 0)
        cchashmap_init(&loop->conns, conn_hash_fn, conn_equal_fn);

    /* Add to hashmap (fd → conn lookup) — using fd as hash key */
    cchashmap_set(&loop->conns, &conn->hnode, NULL);

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
        /* Would block — arm epoll */
        if (conn->recv_cb) ccev__conn_rearm(conn->loop, conn);
        return 1;  /* no data but epoll re-armed; >0 but not a byte count */
    }

    /* n=0 on EOF (CC_OPCODE_OK, 0 bytes) */
    if (n == 0) return 0;

    return CCEV_ERR;
}

int ccev_conn_send(ccev_conn_t *conn, const void *data, size_t len,
                    ccev_send_cb cb, void *udata) {
    if (!conn || conn->closed) return CCEV_ERR;
    if (!data || !len) return 0;

    if (cb) { conn->send_cb = cb; conn->send_udata = udata; }

    int sent = 0;
    ccsocket_stcode_t rc = ccsocket_send(conn->fd, data, (int)len, &sent);

    if (rc == CC_OPCODE_OK || rc == CC_OPCODE_WAIT) {
        if (sent > 0 && (size_t)sent == len) {
            /* Fully written */
            if (conn->send_cb) {
                ccev_send_cb scb = conn->send_cb;
                void *su = conn->send_udata;
                conn->send_cb = NULL;
                scb(su);
            }
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

    if (cb) { conn->send_cb = cb; conn->send_udata = udata; }

    if (!done) {
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
    conn->batch_done = true;

    if (data && len > 0) {
        ccev_buf_t *buf = ccev__buf_alloc(conn->loop, data, len);
        if (!buf) return CCEV_ERR;
        cclink_push_back(&conn->wbuf_list, &buf->node);
        conn->wbuf_len += len;
    }

    if (!cclink_empty(&conn->wbuf_list)) {
        conn->pending_write = true;
        ccev__conn_flush(conn->loop, conn);
    } else if (conn->send_cb) {
        ccev_send_cb scb = conn->send_cb;
        void *su = conn->send_udata;
        conn->send_cb = NULL;
        scb(su);
    }

    return (int)len;
}

int ccev_conn_close(ccev_conn_t *conn) {
    if (!conn || conn->closed) return CCEV_ERR;

    /* Shutdown + close socket */
    if (conn->fd != (ccsocket_t)-1) {
        ccsocket_close(conn->fd);
        conn->fd = (ccsocket_t)-1;
    }

    conn->closed = true;

    if (conn->close_cb) {
        ccev_close_cb cc = conn->close_cb;
        void *cu = conn->close_udata;
        conn->close_cb = NULL;
        cc(cu);
    }

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

int ccev_conn_sendfile(ccev_conn_t *conn, int fd, ccev_send_cb cb, void *udata) {
    if (!conn || conn->closed) return CCEV_ERR;

    if (cb) { conn->send_cb = cb; conn->send_udata = udata; }

    /* Use ccsocket_sendfile for zero-copy (kernel sendfile when supported) */
    ccsocket_sendf_state_t rc = ccsocket_sendfile(conn->fd, fd);
    if (rc == CC_SENDALL || rc == CC_SENDWAIT) {
        if (conn->send_cb) {
            ccev_send_cb scb = conn->send_cb;
            void *su = conn->send_udata;
            conn->send_cb = NULL;
            scb(su);
        }
        return CCEV_OK;
    }

    return CCEV_ERR;
}

int ccev_conn_fd(ccev_conn_t *conn) {
    return conn ? (int)(intptr_t)conn->fd : CCEV_ERR;
}

int ccev_conn_count(ccev_loop_t *loop) {
    return loop ? loop->conn_count : 0;
}
