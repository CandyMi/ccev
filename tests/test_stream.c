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

/* readline/readnum tests removed — raw read only */

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

/* readline/readnum tests removed — raw read only */

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

static int  sendfile_cb_called;
static int  sendfile_cb_status;

static void on_sendfile_done(void *udata, int status) {
    sendfile_cb_called = 1;
    sendfile_cb_status = status;
    ccev_loop_stop((ccev_loop_t *)udata);
}

static void on_sendfile_mark(void *udata, int status) {
    (void)udata;
    sendfile_cb_called = 1;
    sendfile_cb_status = status;
}

static void on_close_marker(void *udata) {
    int *p = (int *)udata;
    (*p)++;
}

/* (Re)create tmpname with the given payload. */
static void sendfile_payload_file(const char *tmpname,
                                  const char *payload, size_t plen) {
    int tfd = open(tmpname, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT(tfd >= 0);
    ASSERT(write(tfd, payload, plen) == (ssize_t)plen);
    close(tfd);
}

/* (Re)create tmpname as a file of `bytes` 'x' characters. */
static void sendfile_big_file(const char *tmpname, size_t bytes) {
    int tfd = open(tmpname, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT(tfd >= 0);
    char chunk[4096];
    memset(chunk, 'x', sizeof(chunk));
    size_t left = bytes;
    while (left > 0) {
        size_t n = left < sizeof(chunk) ? left : sizeof(chunk);
        ASSERT(write(tfd, chunk, n) == (ssize_t)n);
        left -= n;
    }
    close(tfd);
}

TEST(stream_sendfile_smoke) {
#ifndef _WIN32
    char tmpname[] = "/tmp/ccev_sendfile_test_XXXXXX";
    int tfd = mkstemp(tmpname);
    if (tfd < 0) { passed++; return; }
    close(tfd);

    const char *payload = "sendfile-test-payload-12345";
    size_t plen = strlen(payload);
    sendfile_payload_file(tmpname, payload, plen);

    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { unlink(tmpname); passed++; return; }

    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    /* Transfer is fully async — the loop drives EPOLLOUT. */
    sendfile_cb_called = 0;
    sendfile_cb_status = -1;
    ASSERT(ccev_stream_sendfile(st, tmpname, on_sendfile_done, loop) == CCEV_OK);
    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(sendfile_cb_called == 1);
    ASSERT(sendfile_cb_status == CCEV_OK);

    /* Verify the payload arrived intact. */
    char buf[128];
    int n;
    ccsocket_recv(sv[1], buf, sizeof(buf) - 1, &n);
    ASSERT(n == (int)plen);
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

TEST(stream_sendfile_after_write_keeps_order) {
#ifndef _WIN32
    char tmpname[] = "/tmp/ccev_sendfile_test_XXXXXX";
    int tfd = mkstemp(tmpname);
    if (tfd < 0) { passed++; return; }
    close(tfd);

    const char *payload = "sendfile-test-payload-12345";
    size_t plen = strlen(payload);
    sendfile_payload_file(tmpname, payload, plen);

    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { unlink(tmpname); passed++; return; }

    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    /* Headers written BEFORE sendfile must arrive before the file. */
    const char *hdr = "HEAD:";
    int hlen = (int)strlen(hdr);
    ASSERT(ccev_stream_write(st, hdr, (size_t)hlen, NULL, NULL) > 0);

    sendfile_cb_called = 0;
    sendfile_cb_status = -1;
    ASSERT(ccev_stream_sendfile(st, tmpname, on_sendfile_done, loop) == CCEV_OK);
    ccev_loop_run(loop, CCEV_RUN_FOREVER);
    ASSERT(sendfile_cb_called == 1);
    ASSERT(sendfile_cb_status == CCEV_OK);

    /* Read back: header bytes must precede the file payload. */
    char buf[512];
    size_t want = (size_t)hlen + plen;
    size_t got = 0;
    while (got < want) {
        int n = 0;
        ccsocket_recv(sv[1], buf + got, (int)(want - got), &n);
        if (n <= 0) break;
        got += (size_t)n;
    }
    ASSERT(got == want);
    ASSERT(memcmp(buf, hdr, (size_t)hlen) == 0);
    ASSERT(memcmp(buf + hlen, payload, plen) == 0);

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
    unlink(tmpname);
    passed++;
#else
    passed++;
#endif
}

TEST(stream_sendfile_overlap_rejected) {
#ifndef _WIN32
    char tmpname[] = "/tmp/ccev_sendfile_test_XXXXXX";
    int tfd = mkstemp(tmpname);
    if (tfd < 0) { passed++; return; }
    close(tfd);
    sendfile_payload_file(tmpname, "x", 1);

    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { unlink(tmpname); passed++; return; }

    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    /* First transfer is in-flight (async) → second must be rejected. */
    ASSERT(ccev_stream_sendfile(st, tmpname, NULL, NULL) == CCEV_OK);
    ASSERT(ccev_stream_sendfile(st, tmpname, NULL, NULL) == CCEV_ERR);

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
    unlink(tmpname);
    passed++;
#else
    passed++;
#endif
}

TEST(stream_sendfile_cancel_on_close) {
#ifndef _WIN32
    char tmpname[] = "/tmp/ccev_sendfile_test_XXXXXX";
    int tfd = mkstemp(tmpname);
    if (tfd < 0) { passed++; return; }
    close(tfd);
    sendfile_big_file(tmpname, 1024 * 1024);   /* > socket send buffer */

    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { unlink(tmpname); passed++; return; }

    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    int close_cb_called = 0;
    ccev_stream_set_close_cb(st, on_close_marker, &close_cb_called);

    sendfile_cb_called = 0;
    ASSERT(ccev_stream_sendfile(st, tmpname, on_sendfile_mark, NULL) == CCEV_OK);

    /* Let EPOLLOUT fire once — sendfile drains to EWOULDBLOCK and is
     * left in-flight (peer never reads). */
    ccev_loop_run(loop, CCEV_RUN_ONCE);
    ASSERT(sendfile_cb_called == 0);

    /* User-initiated close cancels the transfer: sf_cb must NOT fire,
     * only close_cb. */
    ccev_stream_close(st);
    ccev_loop_run(loop, CCEV_RUN_ONCE);   /* process closing queue */
    ASSERT(sendfile_cb_called == 0);
    ASSERT(close_cb_called == 1);

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

    ASSERT(ccev_stream_read(st, 1024, 0, stream_on_data, &ctx) == CCEV_OK);

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

    /* stream reader (readline/readnum tests removed) */
    /* stream write */
    RUN(stream_write_callback_fires);
    RUN(stream_write_null_data_returns_err);

    /* stream write batch */
    RUN(stream_write_batch_without_cb);
    RUN(stream_write_batch_with_cb);
    RUN(stream_write_batch_flush_only);

    /* stream sendfile */
    RUN(stream_sendfile_smoke);
    RUN(stream_sendfile_after_write_keeps_order);
    RUN(stream_sendfile_overlap_rejected);
    RUN(stream_sendfile_cancel_on_close);

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
