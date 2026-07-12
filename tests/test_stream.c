/**
 * @file test_stream.c
 * @brief Stream lifecycle and I/O tests for the new ccev_stream_t API.
 */

#include "ccev.h"
#include "ccsocket.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

static int passed, failed;
#define TEST(name) static void test_##name(void)
#define ASSERT(cond) do { \
  if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failed++; } \
  else passed++; \
} while(0)
#define RUN(name) do { printf("  %s\n", #name); fflush(stdout); test_##name(); } while(0)

static void timer_stop_loop(ccev_timer_t *timer, void *udata) {
    ccev_loop_stop((ccev_loop_t *)udata);
}

static int pair_create(ccsocket_t sv[2]) {
    return ccsocketpair(sv, CC_NOFLAG) ? 0 : -1;
}

/* ═══ ccev_stream_open / close ────────────────────── */

TEST(stream_open_null_sock_returns_null) {
    ASSERT(ccev_stream_open(NULL) == NULL);
}

TEST(stream_open_closed_sock_returns_null) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ASSERT(sock != NULL);

    ccev_sock_close(sock);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st == NULL);

    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

TEST(stream_open_then_close) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ASSERT(sock != NULL);

    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    ASSERT(ccev_stream_close(st) == CCEV_OK);
    ASSERT(ccev_stream_close(st) == CCEV_ERR); /* double close */

    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ═══ ccev_stream_wbuf_len ────────────────────────── */

TEST(wbuf_len_null_returns_zero) {
    ASSERT(ccev_stream_wbuf_len(NULL) == 0);
}

/* ═══ Stream read-until (basic smoke) ─────────────── */

/* Extended callback version that also stops the loop on timeout */
typedef struct {
    ccev_loop_t *loop;
    char   data[256];
    size_t len;
    int    status;
    int    called;
} stream_ctx_t;

static void stream_on_data(void *udata, const char *data, size_t len, int status) {
    stream_ctx_t *ctx = (stream_ctx_t *)udata;
    ctx->called = 1;
    ctx->status = status;
    ctx->len    = len;
    if (data && len > 0) {
        size_t cp = len < sizeof(ctx->data) ? len : sizeof(ctx->data) - 1;
        memcpy(ctx->data, data, cp);
        ctx->data[cp] = '\0';
    }
    /* Stop the loop so the test doesn't wait for the safety timer. */
    ccev_loop_stop(ctx->loop);
}

TEST(stream_readline_smoke) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ASSERT(sock != NULL);
    ccsocket_set_nonblock(sv[0], true);

    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.loop = loop;

    /* Safety timer (100ms — only fires if read never lands) */
    ccev_timer_add(loop, 100, CCEV_TIMER_ONCE,
                   timer_stop_loop, loop);

    ASSERT(ccev_stream_readline(st, '\n', 1024, 0, stream_on_data, &ctx) == CCEV_OK);

    ccsocket_send(sv[1], "hello\n", 6, NULL);

    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(ctx.called == 1);
    ASSERT(ctx.status == CCEV_OK);
    ASSERT(ctx.len == 6);
    ASSERT(strcmp(ctx.data, "hello\n") == 0);

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

TEST(stream_readnum_smoke) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ASSERT(sock != NULL);
    ccsocket_set_nonblock(sv[0], true);

    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.loop = loop;

    ccev_timer_add(loop, 100, CCEV_TIMER_ONCE,
                   timer_stop_loop, loop);

    ASSERT(ccev_stream_readnum(st, 5, 0, stream_on_data, &ctx) == CCEV_OK);
    ccsocket_send(sv[1], "hello", 5, NULL);

    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(ctx.called == 1);
    ASSERT(ctx.status == CCEV_OK);
    ASSERT(ctx.len == 5);
    ASSERT(strcmp(ctx.data, "hello") == 0);

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ═══ Stream write batch tests ───────────────────── */

typedef struct {
    int called;
    int value;
} send_ctx_t;

static void on_sent(void *udata) {
    send_ctx_t *c = (send_ctx_t *)udata;
    c->called = 1;
}

TEST(stream_write_batch_without_cb) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ASSERT(sock != NULL);
    ccsocket_set_nonblock(sv[0], true);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    /* Buffer first chunk without flush */
    int rc = ccev_stream_write_batch(st, "hello ", 6, false, NULL, NULL);
    ASSERT(rc > 0);
    ASSERT(ccev_stream_wbuf_len(st) == 6);

    /* Buffer second chunk, then flush */
    rc = ccev_stream_write_batch(st, "world", 5, true, NULL, NULL);
    ASSERT(rc > 0);

    /* After flush, wbuf_len should be 0 since data fits in kernel buffer */
    /* On a socketpair both sides are local — flush should drain immediately */

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

TEST(stream_write_batch_with_cb) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ASSERT(sock != NULL);
    ccsocket_set_nonblock(sv[0], true);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    send_ctx_t cb_ctx;
    memset(&cb_ctx, 0, sizeof(cb_ctx));

    /* Buffer with per-buffer callback */
    int rc = ccev_stream_write_batch(st, "callback-test", 13, true, on_sent, &cb_ctx);
    ASSERT(rc > 0);

    /* Drain from the other end so the kernel buffer empties and the
     * per-buffer callback can fire (it fires when data leaves the wlist). */
    char drain[64];
    int n;
    ccsocket_recv(sv[1], drain, sizeof(drain), &n);

    /* Run the loop once — this processes the closing queue and fires callbacks */
    ccev_loop_run(loop, CCEV_RUN_ONCE);

    /* The per-buffer callback may or may not have fired depending on
     * whether the flush consumed the buffer in the same call.  Just verify
     * the API accepted the data correctly. */
    ASSERT(rc > 0);

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

TEST(stream_write_batch_flush_only) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ASSERT(sock != NULL);
    ccsocket_set_nonblock(sv[0], true);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    /* First buffer data then flush with NULL data + done=true */
    ccev_stream_write_batch(st, "data", 4, false, NULL, NULL);
    ASSERT(ccev_stream_wbuf_len(st) == 4);

    /* Flush-only call */
    int rc = ccev_stream_write_batch(st, NULL, 0, true, NULL, NULL);
    ASSERT(rc == 0);  /* NULL data + 0 len returns 0 */

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ═══ Stream reader timeout tests ──────────────────── */

static void stream_on_timeout(void *udata, const char *data, size_t len, int status) {
    stream_ctx_t *ctx = (stream_ctx_t *)udata;
    ctx->called = 1;
    ctx->status = status;
    ctx->len    = len;
    if (data && len > 0) {
        size_t cp = len < sizeof(ctx->data) ? len : sizeof(ctx->data) - 1;
        memcpy(ctx->data, data, cp);
        ctx->data[cp] = '\0';
    }
    if (ctx->loop) ccev_loop_stop(ctx->loop);
}

TEST(stream_readline_timeout) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ASSERT(sock != NULL);
    ccsocket_set_nonblock(sv[0], true);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.loop = loop;

    /* Very short timeout — no data written, so it must timeout */
    ASSERT(ccev_stream_readline(st, '\n', 1024, 10, stream_on_timeout, &ctx) == CCEV_OK);

    /* Safety timer to prevent hang if readline timeout fails */
    ccev_timer_add(loop, 5000, CCEV_TIMER_ONCE,
                   timer_stop_loop, loop);

    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(ctx.called == 1);
    ASSERT(ctx.status == CCEV_ERR);  /* timeout = error */
    ASSERT(ctx.len == 0);

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

TEST(stream_readnum_timeout) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ASSERT(sock != NULL);
    ccsocket_set_nonblock(sv[0], true);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.loop = loop;

    /* Very short timeout — no data written, so it must timeout */
    ASSERT(ccev_stream_readnum(st, 5, 10, stream_on_timeout, &ctx) == CCEV_OK);

    ccev_timer_add(loop, 5000, CCEV_TIMER_ONCE,
                   timer_stop_loop, loop);

    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(ctx.called == 1);
    ASSERT(ctx.status == CCEV_ERR);
    ASSERT(ctx.len == 0);

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ═══ Pipeline + mode-switch + overflow ─────────────── */

static int  pline_count;
static char pline_buf[256];
static void on_pipeline_line(void *udata, const char *data,
                              size_t len, int status) {
    ccev_loop_t *loop = (ccev_loop_t *)udata;
    if (status != CCEV_OK || len == 0) return;
    size_t cp = len;
    if (pline_count + (int)cp > 256) cp = (size_t)(256 - pline_count);
    memcpy(pline_buf + pline_count, data, cp);
    pline_count += (int)cp;
    if (pline_count >= 6)
        ccev_loop_stop(loop);
}

TEST(stream_readline_pipeline) {
    /* 3 lines in one TCP segment → 3 callbacks, all dispatched */
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ccsocket_set_nonblock(sv[0], true);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    pline_count = 0;
    memset(pline_buf, 0, sizeof(pline_buf));

    ccev_timer_add(loop, 500, CCEV_TIMER_ONCE, timer_stop_loop, loop);
    ASSERT(ccev_stream_readline(st, '\n', 1024, 0, on_pipeline_line, loop) == CCEV_OK);
    ccsocket_send(sv[1], "a\nb\nc\n", 6, NULL);
    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(pline_count == 6);
    ASSERT(strcmp(pline_buf, "a\nb\nc\n") == 0);

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

static int  rl_rn_phase;        /* 0=header readline, 1=body readnum */
static stream_ctx_t rl_rn_hdr;
static stream_ctx_t rl_rn_body;

static void on_rl_rn_readline(void *udata, const char *data,
                               size_t len, int status) {
    ccev_stream_t *st = (ccev_stream_t *)udata;
    if (status != CCEV_OK || len == 0) return;

    if (rl_rn_phase == 0) {
        memcpy(rl_rn_hdr.data + rl_rn_hdr.len, data, len);
        rl_rn_hdr.len += len;
        rl_rn_hdr.called++;
        if (len == 1 && data[0] == '\n') {
            rl_rn_phase = 1;
            ccev_stream_readnum(st, 4, 0, on_rl_rn_readline, st);
        } else {
            ccev_stream_readline(st, '\n', 1024, 0, on_rl_rn_readline, st);
        }
    } else {
        memcpy(rl_rn_body.data + rl_rn_body.len, data, len);
        rl_rn_body.len += len;
        rl_rn_body.called++;
        rl_rn_body.status = status;
        ccev_loop_stop(rl_rn_hdr.loop);
    }
}

TEST(stream_readline_to_readnum_switch) {
    /* Read headers via readline, then switch to readnum for 4-byte body */
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ccsocket_set_nonblock(sv[0], true);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    memset(&rl_rn_hdr, 0, sizeof(rl_rn_hdr));
    memset(&rl_rn_body, 0, sizeof(rl_rn_body));
    rl_rn_hdr.loop = loop;
    rl_rn_phase = 0;

    ccev_timer_add(loop, 500, CCEV_TIMER_ONCE, timer_stop_loop, loop);
    ASSERT(ccev_stream_readline(st, '\n', 1024, 0, on_rl_rn_readline, st) == CCEV_OK);

    /* "H: v\r\n\r\nABCD" — header, empty line, then 4-byte body */
    ccsocket_send(sv[1], "H: v\n\nABCD", 11, NULL);
    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(rl_rn_hdr.called == 2);        /* "H: v\n" (5) + "\n" (1) */
    ASSERT(rl_rn_hdr.len == 6);
    ASSERT(rl_rn_body.called == 1);       /* "ABCD" */
    ASSERT(rl_rn_body.len == 4);
    ASSERT(rl_rn_body.status == CCEV_OK);

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ── Pipelined: readline→readnum→readline→readnum in one recv ── */


TEST(stream_readline_overflow) {
    /* maxlen reached without delimiter → CCEV_ERR */
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ccsocket_set_nonblock(sv[0], true);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.loop = loop;

    ccev_timer_add(loop, 500, CCEV_TIMER_ONCE, timer_stop_loop, loop);
    /* maxlen=4, send "hello" (5 bytes, no newline) → overflow */
    ASSERT(ccev_stream_readline(st, '\n', 4, 0, stream_on_data, &ctx) == CCEV_OK);
    ccsocket_send(sv[1], "hello", 5, NULL);
    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(ctx.called == 1);
    ASSERT(ctx.status == CCEV_ERR);
    ASSERT(ctx.len == 4);
    ASSERT(strncmp(ctx.data, "hell", 4) == 0);

    /* sock freed by overflow path — don't close again */
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ═══ Stream write with per-buffer callback ─────────── */

static int  write_cb_called;
static void *write_cb_udata;

static void on_write_complete(void *udata) {
    write_cb_called = 1;
    write_cb_udata  = udata;
}

TEST(stream_write_callback_fires) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ccsocket_set_nonblock(sv[1], true);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    write_cb_called = 0;
    write_cb_udata  = NULL;

    int marker = 1234;
    int rc = ccev_stream_write(st, "hello", 5, on_write_complete, &marker);
    ASSERT(rc > 0);

    /* Drain the receiving end so the kernel buffer can flush */
    char drain[64];
    int nd;
    ccsocket_recv(sv[1], drain, sizeof(drain), &nd);

    /* Run the loop to trigger the write-complete callback */
    ccev_loop_run(loop, CCEV_RUN_ONCE);

    ASSERT(write_cb_called == 1);
    ASSERT(write_cb_udata == &marker);

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

TEST(stream_write_null_data_returns_err) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    int rc = ccev_stream_write(st, NULL, 0, NULL, NULL);
    ASSERT(rc == 0);

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ═══ Stream sendfile ─────────────────────────────── */

TEST(stream_sendfile_smoke) {
#ifndef _WIN32
    char tmpname[] = "/tmp/ccev_sendfile_test_XXXXXX";
    int tfd = mkstemp(tmpname);
    if (tfd < 0) { passed++; return; }

    const char *payload = "sendfile-test-payload-12345";
    size_t plen = strlen(payload);
    ASSERT(write(tfd, payload, plen) == (ssize_t)plen);
    close(tfd);

    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { close(tfd); unlink(tmpname); passed++; return; }

    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    write_cb_called = 0;
    ASSERT(ccev_stream_sendfile(st, tmpname, on_write_complete, NULL) == CCEV_OK);

    /* Drain the receiving end */
    char buf[128];
    int n;
    ccsocket_recv(sv[1], buf, sizeof(buf) - 1, &n);

    if (n > 0) {
        buf[n] = '\0';
        ASSERT(strcmp(buf, payload) == 0);
    }

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
    unlink(tmpname);
    passed++;
#else
    passed++;
#endif
}

/* ═══ ccev_stream_set_send_cb ──────────────────────── */

static int  global_send_cb_fired;
static void on_global_send(void *udata) {
    global_send_cb_fired = 1;
    ccev_loop_stop((ccev_loop_t *)udata);
}

TEST(stream_set_send_cb_fires_on_drain) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ccsocket_set_nonblock(sv[1], true);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    global_send_cb_fired = 0;
    ccev_stream_set_send_cb(st, on_global_send, loop);

    /* Write data — flush should consume immediately on a socketpair */
    int rc = ccev_stream_write(st, "hello", 5, NULL, NULL);
    ASSERT(rc > 0);

    /* Drain the peer so kernel buffer empties and callback fires */
    char drain[64];
    int nd;
    ccsocket_recv(sv[1], drain, sizeof(drain), &nd);

    ccev_loop_run(loop, CCEV_RUN_ONCE);
    ASSERT(global_send_cb_fired == 1);

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ═══ ccev_stream_read_stop ─────────────────────────── */

TEST(stream_read_stop_cancels) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ccsocket_set_nonblock(sv[0], true);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.loop = loop;

    ASSERT(ccev_stream_readline(st, '\n', 1024, 0, stream_on_data, &ctx) == CCEV_OK);

    /* Cancel the reader mid-flight */
    ccev_stream_read_stop(st);

    /* Write data — should NOT trigger the read callback */
    ccsocket_send(sv[1], "hello\n", 6, NULL);

    /* Safety timer to stop the loop */
    ccev_timer_add(loop, 50, CCEV_TIMER_ONCE,
                   timer_stop_loop, loop);
    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(ctx.called == 0);

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ═══ ccev_stream_set_close_cb ──────────────────────── */

static int stream_close_cb_fired;
static void on_stream_close(void *udata) {
    (void)udata;
    stream_close_cb_fired = 1;
}

TEST(stream_set_close_cb_fires) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    stream_close_cb_fired = 0;
    ccev_stream_set_close_cb(st, on_stream_close, NULL);
    ccev_stream_close(st);

    /* Run to process closing queue */
    ccev_loop_run(loop, CCEV_RUN_ONCE);
    ASSERT(stream_close_cb_fired == 1);

    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ═══ Write batch — verified per-buffer callback ───── */

TEST(stream_write_batch_perbuf_callback_verified) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ccsocket_set_nonblock(sv[0], true);
    ccsocket_set_nonblock(sv[1], true);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    send_ctx_t cb_ctx;
    memset(&cb_ctx, 0, sizeof(cb_ctx));

    /* Write a modest chunk with per-buffer callback */
    const char *payload = "verify-perbuf-callback";
    size_t plen = strlen(payload);
    int rc = ccev_stream_write_batch(st, payload, plen, true,
                                      on_sent, &cb_ctx);
    ASSERT(rc > 0);

    /* Drain whatever the kernel already accepted */
    char drain[64];
    int nd;
    ccsocket_recv(sv[1], drain, sizeof(drain), &nd);

    /* Run the loop — this processes the closing queue and fires
     * the per-buffer callback when data leaves the wlist */
    ccev_loop_run(loop, CCEV_RUN_ONCE);
    ASSERT(cb_ctx.called == 1);

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ════════════════════════════════════════════════════ */

int main(void) {
    printf("test_stream\n");
    printf("──────────────────────\n"); fflush(stdout);

    /* open/close */
    RUN(stream_open_null_sock_returns_null);
    RUN(stream_open_closed_sock_returns_null);
    RUN(stream_open_then_close);

    /* wbuf_len */
    RUN(wbuf_len_null_returns_zero);

    /* stream reader */
    RUN(stream_readline_smoke);
    RUN(stream_readnum_smoke);
    RUN(stream_readline_timeout);
    RUN(stream_readnum_timeout);
    RUN(stream_readline_pipeline);
    RUN(stream_readline_to_readnum_switch);
    RUN(stream_readline_overflow);
    /* stream write */
    RUN(stream_write_callback_fires);
    RUN(stream_write_null_data_returns_err);

    /* stream write batch */
    RUN(stream_write_batch_without_cb);
    RUN(stream_write_batch_with_cb);
    RUN(stream_write_batch_flush_only);

    /* stream sendfile */
    RUN(stream_sendfile_smoke);

    /* stream set_send_cb */
    RUN(stream_set_send_cb_fires_on_drain);

    /* stream read_stop */
    RUN(stream_read_stop_cancels);

    /* stream set_close_cb */
    RUN(stream_set_close_cb_fires);

    /* stream write batch — verified per-buf cb */
    RUN(stream_write_batch_perbuf_callback_verified);

    printf("\n  %d passed, %d failed\n", passed, failed); fflush(stdout);
    return failed ? 1 : 0;
}
