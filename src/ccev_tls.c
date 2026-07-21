#define CCEV_HAVE_TLS 1
#include "ccev_tls_internal.h"
#include <errno.h>

/* ════════════════════════════════════════════════════════════════
 *  Forward declarations
 * ════════════════════════════════════════════════════════════════ */

/* _tls_handshake_pump return: CCEV_TLS_HS_AGAIN = WANT_READ */
#define CCEV_TLS_HS_AGAIN 1

static void _tls_on_readable(ccev_sock_t *sock, int events);
static void _tls_handshake_timeout_cb(ccev_timer_t *timer, void *udata);
static void _tls_read_timeout_cb(ccev_timer_t *timer, void *udata);
static int  _tls_handshake_pump(ccev_tls_t *tls);
static void _tls_flush_wbio(ccev_tls_t *tls);
static void _tls_complete_cleanup(ccev_tls_t *tls);
static void _tls_reader_accumulate(ccev_tls_t *tls, const char *data, size_t len);

/* ════════════════════════════════════════════════════════════════
 *  Flush pending ciphertext from wbio to stream write buffer
 * ════════════════════════════════════════════════════════════════ */

static void _tls_flush_wbio(ccev_tls_t *tls) {
    BIO *wbio = SSL_get_wbio(tls->ssl);
    if (!wbio) return;

    char *cipher;
    long cipher_len = BIO_get_mem_data(wbio, &cipher);
    if (cipher_len <= 0) return;

    /* Write ciphertext to the stream write buffer — stream handles
     * buffering, EPOLLOUT, and per-buffer write callbacks. */
    ccev_stream_write(&tls->st, cipher, (size_t)cipher_len, NULL, NULL);

    /* Consume wbio — data is now in the stream's write buffer. */
    BIO_read(wbio, cipher, cipher_len);
}

/* ════════════════════════════════════════════════════════════════
 *  Main read callback (replaces sock->rcb during TLS)
 * ════════════════════════════════════════════════════════════════ */

static void _tls_on_readable(ccev_sock_t *sock, int events) {
    (void)events;
    if (!sock || sock->closed) return;

    /* Union-based cast: sock is the first field of both ccev_stream_t
     * and ccev_tls_t, so this reinterpretation is safe via the
     * common-initial-sequence rule (C11 §6.5.2.3p6). */
    ccev_sock_any_t *any = (ccev_sock_any_t *)sock;
    ccev_tls_t *tls = &any->tls;

    /* ── Handshake in progress? ── */
    if (!tls->handshake_done) {
        char hs_buf[16384];
        int nread = 0;
        ccsocket_stcode_t rc_s = ccsocket_recv(sock->fd, hs_buf,
                                                 sizeof(hs_buf), &nread);
        if (rc_s == CC_OPCODE_OK && nread > 0)
            BIO_write(SSL_get_rbio(tls->ssl), hs_buf, nread);

        int rc = _tls_handshake_pump(tls);
        if (rc != CCEV_TLS_HS_AGAIN) {
            tls->handshake_done = true;
            if (tls->timer) {
                ccev_timer_del(sock->loop, tls->timer);
                tls->timer = NULL;
            }
            int status = (rc == CCEV_TLS_OK) ? CCEV_TLS_OK : CCEV_TLS_ERR_PROTO;
            if (tls->handshake_cb) {
                ccev_tls_handshake_cb cb = tls->handshake_cb;
                void *ud = tls->handshake_udata;
                tls->handshake_cb = NULL;
                cb(ud, tls, status);
            }
            if (status != CCEV_TLS_OK)
                ccev__sock_schedule_close(sock->loop, sock);
        }
        return;
    }

    /* ── Normal read: recv → BIO → SSL_read → reader dispatch ── */
    {
        char net_buf[65535];
        int nread = 0;
        ccsocket_stcode_t rc = ccsocket_recv(sock->fd, net_buf,
                                               sizeof(net_buf), &nread);

        if (rc == CC_OPCODE_WAIT || nread <= 0) {
            if (rc == CC_OPCODE_WAIT) {
                ccev__sock_rearm(sock->loop, sock);
            } else {
                if (tls->read_cb) {
                    ccev_stream_cb cb = tls->read_cb;
                    void *ud = tls->read_udata;
                    tls->read_cb = NULL;
                    cb(ud, NULL, 0, CCEV_ERR);
                }
                ccev__sock_schedule_close(sock->loop, sock);
            }
            return;
        }

        BIO_write(SSL_get_rbio(tls->ssl), net_buf, nread);

        while (1) {
            char plain[16384];
            int ret = SSL_read(tls->ssl, plain, (int)sizeof(plain));
            if (ret <= 0) {
                int err = SSL_get_error(tls->ssl, ret);
                if (err == SSL_ERROR_WANT_READ)
                    break;
                if (err == SSL_ERROR_ZERO_RETURN) {
                    if (tls->read_cb) {
                        ccev_stream_cb cb = tls->read_cb;
                        void *ud = tls->read_udata;
                        tls->read_cb = NULL;
                        cb(ud, NULL, 0, CCEV_ERR);
                    }
                    /* Peer sent close_notify — close immediately.
                     * TCP TIME_WAIT covers any delayed packets. */
                    _tls_complete_cleanup(tls);
                    ccev_stream_close(&tls->st);
                    return;
                }
                break;
            }
            _tls_reader_accumulate(tls, plain, (size_t)ret);
        }
    }

    /* Flush any renegotiation output. */
    _tls_flush_wbio(tls);
}

/* ════════════════════════════════════════════════════════════════
 *  Timer callbacks
 * ════════════════════════════════════════════════════════════════ */

static void _tls_handshake_timeout_cb(ccev_timer_t *timer, void *udata) {
    ccev_tls_t *tls = (ccev_tls_t *)udata;
    if (!tls || tls->handshake_done) return;
    tls->timer = NULL;
    tls->handshake_done = true;
    if (tls->handshake_cb) {
        ccev_tls_handshake_cb cb = tls->handshake_cb;
        void *ud = tls->handshake_udata;
        tls->handshake_cb = NULL;
        cb(ud, tls, CCEV_TLS_ERR_IO);
    }
    ccev__sock_schedule_close(tls->st.sock.loop, &tls->st.sock);
}

static void _tls_read_timeout_cb(ccev_timer_t *timer, void *udata) {
    ccev_tls_t *tls = (ccev_tls_t *)udata;
    if (!tls || !tls->read_cb) return;
    tls->timer = NULL;
    ccev_stream_cb cb = tls->read_cb;
    void *ud = tls->read_udata;
    tls->read_cb = NULL;
    cb(ud, NULL, 0, CCEV_ERR);
}

/* ════════════════════════════════════════════════════════════════
 *  Public API — write
 * ════════════════════════════════════════════════════════════════ */

int ccev_tls_write(ccev_tls_t *tls, const void *data, size_t len,
                    ccev_send_cb cb, void *udata) {
    if (!tls || !tls->ssl || !tls->handshake_done) return CCEV_ERR;
    if (!data || !len) return 0;

    if (SSL_write(tls->ssl, data, (int)len) <= 0) {
        /* Flush wbio even on error — OpenSSL may have queued a fatal
         * alert (e.g. decrypt_error, bad_certificate).  Without this
         * the peer receives only a TCP RST with no diagnostic. */
        _tls_flush_wbio(tls);
        return CCEV_ERR;
    }

    /* Encrypted data is now in wbio — flush to the stream write buffer,
     * passing the user's write-completion callback through. */
    BIO *wbio = SSL_get_wbio(tls->ssl);
    char *cipher;
    long cipher_len = BIO_get_mem_data(wbio, &cipher);
    if (cipher_len <= 0)
        return (int)len; /* TLS 1.3 empty record */

    int ret = ccev_stream_write(&tls->st, cipher, (size_t)cipher_len, cb, udata);
    BIO_read(wbio, cipher, cipher_len);
    return (ret < 0) ? CCEV_ERR : (int)len;
}

int ccev_tls_write_batch(ccev_tls_t *tls, const void *data, size_t len,
                          bool done, ccev_send_cb cb, void *udata) {
    if (!tls || !tls->ssl || !tls->handshake_done) return CCEV_ERR;

    if (data && len > 0) {
        if (SSL_write(tls->ssl, data, (int)len) <= 0) {
            /* Flush wbio even on error — OpenSSL may have queued a fatal
             * alert (e.g. decrypt_error, bad_certificate).  Without this
             * the peer receives only a TCP RST with no diagnostic. */
            _tls_flush_wbio(tls);
            return CCEV_ERR;
        }
        _tls_flush_wbio(tls);
    }

    if (done)
        ccev_stream_flush(&tls->st);

    if (cb) cb(udata);
    return (int)len;
}

int ccev_tls_flush(ccev_tls_t *tls) {
    if (!tls || !tls->ssl) return CCEV_ERR;
    return ccev_stream_flush(&tls->st);
}

/* ════════════════════════════════════════════════════════════════
 *  Reader — accumulation and dispatch
 * ════════════════════════════════════════════════════════════════ */

static void _tls_reader_dispatch(ccev_tls_t *tls, size_t consumed, int status) {
    ccev_sock_t   *sock = &tls->st.sock;
    ccev_stream_cb cb   = tls->read_cb;
    void          *ud   = tls->read_udata;
    size_t         remaining = tls->read_len - consumed;

    if (tls->timer) {
        ccev_timer_del(sock->loop, tls->timer);
        tls->timer = NULL;
    }

    size_t data_off = tls->read_pos;
    tls->read_pos += consumed;
    tls->read_len  = remaining;
    tls->read_cb   = NULL;

    /* Save buffer before callback — cb may replace read_buf
     * (re-entrant _tls_reader_start frees old + allocs new). */
    char   *old_buf = tls->read_buf;
    size_t  old_cap = tls->read_cap;

    cb(ud, old_buf + data_off, consumed, status);

    /* Compact if buffer wasn't replaced by callback. */
    if (tls->read_buf == old_buf && tls->read_pos > tls->read_cap / 2
        && tls->read_len > 0) {
        memmove(tls->read_buf, tls->read_buf + tls->read_pos, tls->read_len);
        tls->read_pos = 0;
    }

    if (remaining == 0 && tls->read_buf == old_buf) {
        ccev__free_fn(tls->read_buf);
        tls->read_buf = NULL;
        tls->read_cap = 0;
    }
}

static void _tls_reader_acc_dispatch(ccev_tls_t *tls) {
    if (!tls->read_cb) return;

    size_t consumed = 0;
    int    status   = CCEV_OK;

    if (tls->read_mode == CCEV_TLS_READ) {
        if (tls->read_len > 0) {
            consumed = tls->read_len;
            ccev_stream_cb cb = tls->read_cb;
            void *ud = tls->read_udata;
            if (tls->timer) {
                ccev_timer_del(tls->st.sock.loop, tls->timer);
                tls->timer = NULL;
            }
            size_t data_off = tls->read_pos;
            tls->read_pos += consumed;
            tls->read_len = 0;
            cb(ud, tls->read_buf + data_off, consumed, CCEV_OK);
            tls->read_pos = 0;
        }
    } else { /* CCEV_TLS_READNUM */
        if (tls->read_len >= tls->read_want)
            consumed = tls->read_want;
    }

    if (consumed > 0) {
        if (tls->read_mode == CCEV_TLS_READ) {
            /* Already dispatched above. Reader stays active. */
        } else {
            _tls_reader_dispatch(tls, consumed, status);
        }
    }
}

static void _tls_reader_accumulate(ccev_tls_t *tls, const char *data, size_t len) {
    size_t needed = tls->read_pos + tls->read_len + len;
    if (needed > tls->read_cap) {
        size_t new_cap = tls->read_cap ? tls->read_cap * 2 : 4096;
        while (new_cap < needed) new_cap *= 2;
        char *nb = (char *)ccev__realloc_fn(tls->read_buf, new_cap);
        if (!nb) {
            if (tls->read_cb) {
                ccev_stream_cb cb = tls->read_cb;
                void *ud = tls->read_udata;
                tls->read_cb = NULL;
                cb(ud, NULL, 0, CCEV_ERR);
            }
            /* OOM is unrecoverable — close the connection. */
            ccev__sock_schedule_close(tls->st.sock.loop, &tls->st.sock);
            return;
        }
        tls->read_buf = nb;
        tls->read_cap = new_cap;
    }

    memcpy(tls->read_buf + tls->read_pos + tls->read_len, data, len);
    tls->read_len += len;

    _tls_reader_acc_dispatch(tls);
}

static int _tls_reader_start(ccev_tls_t *tls, size_t want,
                              bool is_raw, int timeout_ms,
                              ccev_stream_cb cb, void *udata) {
    if (!tls || !cb || !tls->handshake_done) return CCEV_ERR;
    if (!is_raw && want == 0) return CCEV_ERR;

    tls->read_cb = NULL;
    if (tls->timer) {
        ccev_timer_del(tls->st.sock.loop, tls->timer);
        tls->timer = NULL;
    }

    /* Free old buffer from a previous read mode. */
    if (tls->read_buf) {
        ccev__free_fn(tls->read_buf);
        tls->read_buf = NULL;
        tls->read_cap = 0;
    }

    if (is_raw) {
        tls->read_buf = (char *)ccev__realloc_fn(NULL, 16384);
        if (!tls->read_buf) return CCEV_ERR;
        tls->read_cap = 16384;
    } else {
        tls->read_buf = (char *)ccev__realloc_fn(NULL, want + 1);
        if (!tls->read_buf) return CCEV_ERR;
        tls->read_cap = want + 1;
    }
    tls->read_pos = 0;
    tls->read_len = 0;

    tls->read_want  = is_raw ? tls->read_cap : want;
    tls->read_mode  = is_raw ? CCEV_TLS_READ : CCEV_TLS_READNUM;
    tls->read_cb    = cb;
    tls->read_udata = udata;

    if (timeout_ms > 0 && !tls->timer) {
        tls->timer = ccev_timer_add(tls->st.sock.loop, (uint64_t)timeout_ms,
                                     CCEV_TIMER_ONCE, _tls_read_timeout_cb, tls);
    }

    tls->st.sock.rcb = _tls_on_readable;
    ccev__sock_rearm(tls->st.sock.loop, &tls->st.sock);

    return CCEV_OK;
}

int ccev_tls_read(ccev_tls_t *tls, size_t limit, int timeout_ms,
                   ccev_stream_cb cb, void *udata) {
    return _tls_reader_start(tls, limit, true, timeout_ms, cb, udata);
}
int ccev_tls_readnum(ccev_tls_t *tls, size_t n, int timeout_ms,
                      ccev_stream_cb cb, void *udata) {
    return _tls_reader_start(tls, n, false, timeout_ms, cb, udata);
}

/* ════════════════════════════════════════════════════════════════
 *  Handshake pump
 * ════════════════════════════════════════════════════════════════ */

static int _tls_handshake_pump(ccev_tls_t *tls) {
    int ret = tls->is_server
              ? SSL_accept(tls->ssl)
              : SSL_connect(tls->ssl);

    /* Always flush wbio — even on error there may be alerts to send. */
    _tls_flush_wbio(tls);

    if (ret == 1)
        return CCEV_TLS_OK;

    int err = SSL_get_error(tls->ssl, ret);

    /* With Memory BIOs, WANT_WRITE is rare (wbio is always writable)
     * but not guaranteed absent (TLS 1.3 HRR, OpenSSL internals).
     * Defensively treat both WANT_* as "try again". */
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        return CCEV_TLS_HS_AGAIN;

    /* Check certificate verification result before falling through
     * to protocol error — lets the caller distinguish "bad cert"
     * from "protocol mismatch".  SSL_get_verify_result is safe to
     * call regardless of handshake state (returns X509_V_OK if no
     * verification was performed or no cert was presented). */
    long verify_err = SSL_get_verify_result(tls->ssl);
    if (verify_err != X509_V_OK)
        return CCEV_TLS_ERR_CERT;

    return CCEV_TLS_ERR_PROTO;
}

/* ════════════════════════════════════════════════════════════════
 *  Public API — lifecycle
 * ════════════════════════════════════════════════════════════════ */

ccev_tls_t *ccev_tls_open(ccev_sock_t *sock,
                            ccev_tls_ctx_t *ctx) {
    if (!sock || sock->closed || sock->in_closing) return NULL;
    if (sock->mode != CCEV_SOCK_INIT) return NULL;
    if (!ctx || !ctx->ssl_ctx) return NULL;

    ccev__tls_init();

    /* Union-based cast: sock was allocated via ccev_sock_any_t, and
     * ccev_tls_t's first field is ccev_stream_t which has ccev_sock_t
     * as its first field — same pointer, safe reinterpretation. */
    ccev_sock_any_t *any = (ccev_sock_any_t *)sock;
    ccev_tls_t *tls = &any->tls;

    /* Zero TLS and stream fields beyond the embedded sock.
     * The sock's original fields (loop, fd, rcb, wcb, events, etc.)
     * are preserved since they occupy the common initial sequence. */
    memset((char *)&tls->st + sizeof(ccev_sock_t), 0,
           sizeof(ccev_tls_t) - sizeof(ccev_sock_t));

    /* Initialise stream write buffer. */
    cclink_init(&tls->st.wlist);
    tls->st.sendfile_fd = -1;

    /* Create SSL object from the context. */
    SSL *ssl = SSL_new(ctx->ssl_ctx);
    if (!ssl) return NULL;

    if (ctx->is_server)
        SSL_set_accept_state(ssl);
    else
        SSL_set_connect_state(ssl);

    BIO *rbio = BIO_new(BIO_s_mem());
    BIO *wbio = BIO_new(BIO_s_mem());
    if (!rbio || !wbio) {
        if (rbio) BIO_free(rbio);
        if (wbio) BIO_free(wbio);
        SSL_free(ssl);
        return NULL;
    }
    SSL_set_bio(ssl, rbio, wbio);

    tls->ssl       = ssl;
    tls->is_server = ctx->is_server;

    /* Take over sock's read callback.  Stream's write callback handles
     * EPOLLOUT automatically via the stream flush path. */
    sock->rcb = _tls_on_readable;
    ccev__sock_rearm(sock->loop, sock);

    return tls;
}

int ccev_tls_handshake(ccev_tls_t *tls,
                        int timeout_ms,
                        ccev_tls_handshake_cb cb,
                        void *udata) {
    if (!tls || !tls->ssl || !cb) return CCEV_ERR;

    /* Idempotent: if handshake already completed, fire callback
     * immediately so callers always get a notification. */
    if (tls->handshake_done) {
        cb(udata, tls, CCEV_TLS_OK);
        return CCEV_TLS_OK;
    }

    tls->handshake_cb   = cb;
    tls->handshake_udata = udata;

    if (timeout_ms > 0) {
        tls->timer = ccev_timer_add(tls->st.sock.loop, (uint64_t)timeout_ms,
                                     CCEV_TIMER_ONCE,
                                     _tls_handshake_timeout_cb, tls);
    }

    tls->st.sock.rcb = _tls_on_readable;
    ccev__sock_rearm(tls->st.sock.loop, &tls->st.sock);

    int rc = _tls_handshake_pump(tls);
    if (rc != CCEV_TLS_HS_AGAIN) {
        tls->handshake_done = true;
        if (tls->timer) {
            ccev_timer_del(tls->st.sock.loop, tls->timer);
            tls->timer = NULL;
        }
        if (rc == CCEV_TLS_OK) {
            cb(udata, tls, CCEV_TLS_OK);
            return CCEV_TLS_OK;
        }
        cb(udata, tls, rc);
        ccev__sock_schedule_close(tls->st.sock.loop, &tls->st.sock);
        return rc;
    }

    return CCEV_TLS_HS_AGAIN;
}

int ccev_tls_set_servername(ccev_tls_t *tls, const char *hostname) {
    if (!tls || !tls->ssl) return CCEV_TLS_ERR_SYS;
    if (tls->handshake_done) return CCEV_ERR;
    if (!hostname) return CCEV_TLS_OK;

    /* Set SNI extension + enable hostname verification.
     * Without SSL_set1_host, the peer certificate's CN/SAN is never
     * checked against the target hostname — a valid cert for any
     * domain would be accepted (MITM). */
    if (!SSL_set1_host(tls->ssl, hostname))
        return CCEV_TLS_ERR_SYS;

    return SSL_set_tlsext_host_name(tls->ssl, hostname) ? CCEV_TLS_OK : CCEV_TLS_ERR_SYS;
}

int ccev_tls_set_alpn(ccev_tls_t *tls, const char *protos) {
    if (!tls || !tls->ssl) return CCEV_TLS_ERR_SYS;
    if (!protos) return CCEV_TLS_OK;
    size_t protos_len = strlen(protos);
    unsigned char *wire = (unsigned char *)ccev__realloc_fn(NULL, protos_len + 2);
    if (!wire) return CCEV_TLS_ERR_SYS;
    size_t out = 0;
    const char *p = protos;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t plen = comma ? (size_t)(comma - p) : strlen(p);
        if (plen > 255) { ccev__free_fn(wire); return CCEV_TLS_ERR_SYS; }
        wire[out++] = (unsigned char)plen;
        memcpy(wire + out, p, plen);
        out += plen;
        p = comma ? comma + 1 : p + plen;
    }
    int ret = SSL_set_alpn_protos(tls->ssl, wire, (unsigned int)out);
    ccev__free_fn(wire);
    return (ret == 0) ? CCEV_TLS_OK : CCEV_TLS_ERR_SYS;
}


ccev_tls_t *ccev_tls_wrap_stream(ccev_sock_t *sock,
                                   ccev_tls_ctx_t *ctx,
                                   const char *servername,
                                   int timeout_ms,
                                   ccev_tls_handshake_cb cb,
                                   void *udata) {
    ccev_tls_t *tls = ccev_tls_open(sock, ctx);
    if (!tls) return NULL;
    if (servername)
        ccev_tls_set_servername(tls, servername);
    ccev_tls_handshake(tls, timeout_ms, cb, udata);
    return tls;
}

/* ════════════════════════════════════════════════════════════════
 *  Close path
 * ════════════════════════════════════════════════════════════════ */

static void _tls_complete_cleanup(ccev_tls_t *tls) {
    if (!tls) return;

    /* Fire pending handshake callback before releasing SSL.
     * If close() is called mid-handshake the user's state machine
     * would otherwise wait forever for a callback that never comes. */
    if (!tls->handshake_done && tls->handshake_cb) {
        ccev_tls_handshake_cb cb = tls->handshake_cb;
        void *ud = tls->handshake_udata;
        tls->handshake_cb = NULL;
        cb(ud, tls, CCEV_TLS_ERR_IO);
    }

    if (tls->timer) {
        ccev_timer_del(tls->st.sock.loop, tls->timer);
        tls->timer = NULL;
    }
    if (tls->ssl) {
        SSL_free(tls->ssl);
        tls->ssl = NULL;
    }
    if (tls->read_buf) {
        ccev__free_fn(tls->read_buf);
        tls->read_buf = NULL;
    }
    /* Stream cleanup + schedule_close handled by ccev_stream_close. */
}

int ccev_tls_close(ccev_tls_t *tls) {
    if (!tls || !tls->st.sock.loop) return CCEV_ERR;
    if (tls->st.sock.closed) {
        _tls_complete_cleanup(tls);
        return CCEV_OK;
    }

    /* Flush any pending write data via the stream. */
    ccev_stream_flush(&tls->st);

    if (tls->ssl) {
        /* Best-effort: try SSL_shutdown once to send close_notify.
         * Do NOT wait for peer's reply — TCP TIME_WAIT covers
         * any delayed packets on the wire. */
        SSL_shutdown(tls->ssl);
        _tls_flush_wbio(tls);
    }

    _tls_complete_cleanup(tls);
    ccev_stream_close(&tls->st);
    return CCEV_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  Accessor APIs — delegate to stream
 * ════════════════════════════════════════════════════════════════ */

void ccev_tls_set_send_cb(ccev_tls_t *tls, ccev_send_cb cb, void *udata) {
    if (!tls) return;
    ccev_stream_set_send_cb(&tls->st, cb, udata);
}

void ccev_tls_set_close_cb(ccev_tls_t *tls, ccev_close_cb cb, void *udata) {
    if (!tls) return;
    ccev_stream_set_close_cb(&tls->st, cb, udata);
}

size_t ccev_tls_wbuf_len(const ccev_tls_t *tls) {
    return tls ? ccev_stream_wbuf_len(&tls->st) : 0;
}
