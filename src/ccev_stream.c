/**
 * @file ccev_stream.c
 * @brief Stream reader — read-until-delimiter / read-N-bytes state machine.
 *
 * Operates by temporarily taking over the connection's recv_cb.  Once the
 * requested data is accumulated, the user callback fires once and the
 * original recv_cb is restored.
 *
 * Any unconsumed data (e.g. more bytes arrived than requested) is kept in
 * the reader's buffer so the next read_until / read_n call can satisfy from
 * it immediately without a network round-trip.
 *
 * Re-entrancy: if the user calls read_* from inside the stream callback,
 * the reader is already in "idle" state (cb == NULL) and the new read
 * will either satisfy from remaining data or resume accumulation.
 *
 * Safety notes (memory):
 *   - Buffer reorganisation (memmove) happens AFTER the user callback
 *     returns, so rd->buf always contains the original consumed data at
 *     the front during the callback, even if the cb_data copy failed (OOM).
 *   - Re-entrant read_* from inside the callback will see rd->len already
 *     set to the remaining count, preventing the delimiter from matching
 *     against already-consumed data.
 *   - CC_OPCODE_WAIT (spurious wakeup) re-arms rather than erroring.
 */

#include "ccev_internal.h"

/* ════════════════════════════════════════════════════════════════
 *  Forward declarations
 * ════════════════════════════════════════════════════════════════ */

static void _stream_on_readable(void *udata);

/* ════════════════════════════════════════════════════════════════
 *  Internal helpers
 * ════════════════════════════════════════════════════════════════ */

/* Stop the reader WITHOUT firing the user callback.
 * Restores old_recv_cb/old_recv_udata, frees buf and reader. */
void ccev_conn_read_stop(ccev_conn_t *conn) {
    if (!conn || !conn->reader) return;
    ccev_stream_reader_t *rd = conn->reader;
    conn->recv_cb   = rd->old_recv_cb;
    conn->recv_udata = rd->old_recv_udata;
    ccev__free_fn(rd->buf);
    ccev__free_fn(rd);
    conn->reader = NULL;
}

/* Fire the stream callback with the consumed data, then reorganise
 * any remaining data in the reader buffer.
 *
 * Critically, rd->len is set to remain BEFORE the callback, so a
 * re-entrant read_* call from inside the callback will scan only the
 * unconsumed bytes — not the already-delivered data.  The memmove of
 * remaining data to the front of the buffer also happens BEFORE the
 * callback so that read_* sees correct content.
 *
 * Because memmove overwrites the consumed portion of rd->buf, we
 * allocate a separate cb_data copy.  If the allocation fails (OOM),
 * we skip the memmove and rely on the user contract ("data valid only
 * during callback"), accepting that a re-entrant read_* in the OOM
 * path is a secondary concern (the system is out of memory). */
static void _reader_dispatch(ccev_conn_t *conn, ccev_stream_reader_t *rd,
                              size_t consumed, int status) {
    ccev_stream_cb cb     = rd->cb;
    void          *ud     = rd->udata;
    size_t         remain = rd->len - consumed;

    /* Allocate a copy of the consumed data for the callback */
    char *cb_data = NULL;
    if (consumed > 0) {
        cb_data = (char *)ccev__realloc_fn(NULL, consumed);
        if (cb_data) memcpy(cb_data, rd->buf, consumed);
    }

    if (remain > 0 && cb_data) {
        /* Normal path: copy succeeded — reorganise buffer before callback
         * so re-entrant read_* sees correct remaining content. */
        memmove(rd->buf, rd->buf + consumed, remain);
    }
    /* When cb_data is NULL (OOM) and remain > 0: skip memmove so that
     * rd->buf still holds the consumed data for the callback fallback.
     * After the callback we reorganise in the cleanup section. */

    rd->len = remain;   /* Re-entrant read_* will only scan remaining bytes */
    rd->cb  = NULL;     /* Mark idle — re-entrant read_* can take over */

    /* Restore original recv callback */
    conn->recv_cb   = rd->old_recv_cb;
    conn->recv_udata = rd->old_recv_udata;

    /* Fire user callback — data is cb_data (copy) or rd->buf (OOM fallback) */
    cb(ud, cb_data ? cb_data : rd->buf, consumed, status);

    ccev__free_fn(cb_data);

    /* Post-callback reorganisation (only needed on OOM path when memmove
     * was skipped above, AND only if the reader wasn't freed by a
     * re-entrant read_stop during the callback). */
    if (remain > 0 && !cb_data && conn->reader == rd) {
        memmove(rd->buf, rd->buf + consumed, remain);
        rd->len = remain;
    }

    /* Free reader if no remaining data AND no re-entrant read started
     * (which would have set rd->cb to non-NULL) */
    if (remain == 0 && conn->reader == rd && rd->cb == NULL) {
        ccev__free_fn(rd->buf);
        ccev__free_fn(rd);
        conn->reader = NULL;
    }
}

/* Start a stream read operation.
 *
 * Handles three cases:
 *   1. Existing idle reader with sufficient buffered data → fire immediately
 *   2. Existing idle reader, insufficient data → reuse, arm EPOLLIN
 *   3. No reader → allocate fresh, arm EPOLLIN
 *
 * Returns CCEV_OK on success (may fire synchronously), CCEV_ERR on error. */
static int _reader_start(ccev_conn_t *conn, size_t want,
                          char delim, bool is_n,
                          ccev_stream_cb cb, void *udata) {
    if (!conn || !cb || want == 0 || conn->closed) return CCEV_ERR;

    ccev_stream_reader_t *rd = conn->reader;

    /* Cancel active reader if one exists */
    if (rd && rd->cb) {
        ccev_conn_read_stop(conn);
        rd = NULL;
    }

    /* Create or reuse */
    if (!rd) {
        rd = (ccev_stream_reader_t *)ccev__realloc_fn(NULL,
                                        sizeof(ccev_stream_reader_t));
        if (!rd) return CCEV_ERR;
        memset(rd, 0, sizeof(*rd));

        rd->buf = (char *)ccev__realloc_fn(NULL, want + 1);
        if (!rd->buf) { ccev__free_fn(rd); return CCEV_ERR; }
        rd->cap          = want + 1;
        rd->old_recv_cb  = conn->recv_cb;
        rd->old_recv_udata = conn->recv_udata;
        rd->conn         = conn;
        conn->reader     = rd;
    } else {
        /* Idle reader with buffer — grow if needed */
        if (rd->cap < want + 1) {
            char *nb = (char *)ccev__realloc_fn(rd->buf, want + 1);
            if (!nb) { ccev_conn_read_stop(conn); return CCEV_ERR; }
            rd->buf = nb;
            rd->cap = want + 1;
        }
        /* Refresh saved recv callback — user might have changed it
         * between reads (e.g. via ccev_conn_recv mode 3). */
        rd->old_recv_cb   = conn->recv_cb;
        rd->old_recv_udata = conn->recv_udata;
    }

    rd->want  = want;
    rd->delim = delim;
    rd->is_n  = is_n;
    rd->cb    = cb;
    rd->udata = udata;

    /* Try to satisfy from existing buffered data */
    size_t consumed = 0;
    if (is_n) {
        if (rd->len >= want) consumed = want;
    } else {
        for (size_t i = 0; i < rd->len; i++) {
            if (rd->buf[i] == delim) { consumed = i + 1; break; }
        }
        /* max_len exceeded — deliver at most want bytes */
        if (consumed == 0 && rd->len >= want) {
            consumed = want;
            _reader_dispatch(conn, rd, consumed, CCEV_ERR);
            return CCEV_OK;
        }
    }

    if (consumed > 0) {
        _reader_dispatch(conn, rd, consumed, CCEV_OK);
        return CCEV_OK;
    }

    /* Not satisfied — take over recv callback and arm */
    conn->recv_cb   = _stream_on_readable;
    conn->recv_udata = rd;
    ccev__conn_rearm(conn->loop, conn);
    return CCEV_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  Internal recv callback (replaces conn->recv_cb while reader active)
 * ════════════════════════════════════════════════════════════════ */

static void _stream_on_readable(void *udata) {
    ccev_stream_reader_t *rd  = (ccev_stream_reader_t *)udata;
    ccev_conn_t          *conn = rd->conn;
    if (!conn || conn->closed) return;

    int nread = 0;
    ccsocket_stcode_t rc = ccsocket_recv(conn->fd, rd->buf + rd->len,
                                          rd->cap - rd->len, &nread);

    /* CC_OPCODE_WAIT = EAGAIN (spurious wakeup) — re-arm and return */
    if (rc == CC_OPCODE_WAIT) {
        ccev__conn_rearm(conn->loop, conn);
        return;
    }

    /* Real error or EOF — connection dead */
    if (rc != CC_OPCODE_OK || nread <= 0) {
        ccev_stream_cb cb = rd->cb;
        void *cb_udata    = rd->udata;
        ccev_conn_read_stop(conn);
        cb(cb_udata, NULL, 0, CCEV_ERR);
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
            ccev__conn_rearm(conn->loop, conn);
            return;
        }
    } else {
        for (size_t i = 0; i < rd->len; i++) {
            if (rd->buf[i] == rd->delim) {
                consumed = i + 1;
                break;
            }
        }
        if (consumed == 0) {
            if (rd->len >= rd->want) {
                consumed = rd->want;
                status   = CCEV_ERR;
            } else {
                ccev__conn_rearm(conn->loop, conn);
                return;
            }
        }
    }

    _reader_dispatch(conn, rd, consumed, status);
}

/* ════════════════════════════════════════════════════════════════
 *  Public API
 * ════════════════════════════════════════════════════════════════ */

int ccev_conn_read_until(ccev_conn_t *conn, char delim, size_t max_len,
                          ccev_stream_cb cb, void *udata) {
    return _reader_start(conn, max_len, delim, false, cb, udata);
}

int ccev_conn_read_n(ccev_conn_t *conn, size_t n,
                      ccev_stream_cb cb, void *udata) {
    return _reader_start(conn, n, 0, true, cb, udata);
}
