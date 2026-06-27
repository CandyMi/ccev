/**
 * @file test_stream.c
 * @brief Stream lifecycle and I/O tests for the new ccev_stream_t API.
 */

#include "ccev.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#endif

static int passed, failed;
#define TEST(name) static void test_##name(void)
#define ASSERT(cond) do { \
  if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failed++; } \
  else passed++; \
} while(0)
#define RUN(name) do { printf("  %s\n", #name); fflush(stdout); test_##name(); } while(0)

static int pair_create(int sv[2]) {
#ifdef _WIN32
    (void)sv; return -1;
#else
    return socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
#endif
}

/* ═══ ccev_stream_open / close ────────────────────── */

TEST(stream_open_null_sock_returns_null) {
    ASSERT(ccev_stream_open(NULL) == NULL);
}

TEST(stream_open_closed_sock_returns_null) {
#ifndef _WIN32
    int sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, (ccsocket_t)(intptr_t)sv[0], NULL);
    ASSERT(sock != NULL);

    ccev_sock_close(sock);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st == NULL);

    ccev_loop_destroy(loop);
    close(sv[1]);
    passed++;
#else
    passed++;
#endif
}

TEST(stream_open_then_close) {
#ifndef _WIN32
    int sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, (ccsocket_t)(intptr_t)sv[0], NULL);
    ASSERT(sock != NULL);

    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    ASSERT(ccev_stream_close(st) == CCEV_OK);
    ASSERT(ccev_stream_close(st) == CCEV_ERR); /* double close */

    ccev_loop_destroy(loop);
    close(sv[1]);
    passed++;
#else
    passed++;
#endif
}

/* ═══ ccev_stream_wbuf_len ────────────────────────── */

TEST(wbuf_len_null_returns_zero) {
    ASSERT(ccev_stream_wbuf_len(NULL) == 0);
}

/* ═══ Stream read-until (basic smoke) ─────────────── */

typedef struct {
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
}

TEST(stream_readline_smoke) {
#ifndef _WIN32
    int sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, (ccsocket_t)(intptr_t)sv[0], NULL);
    ASSERT(sock != NULL);
    ccsocket_set_nonblock((ccsocket_t)(intptr_t)sv[0], true);

    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* Safety timer */
    ccev_timer_add(loop, 5000, CCEV_TIMER_ONCE,
                   (ccev_timer_cb)(void(*)(void))ccev_loop_stop, loop);

    ASSERT(ccev_stream_readline(st, '\n', 1024, 0, stream_on_data, &ctx) == CCEV_OK);

    write(sv[1], "hello\n", 6);

    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(ctx.called == 1);
    ASSERT(ctx.status == CCEV_OK);
    ASSERT(ctx.len == 6);
    ASSERT(strcmp(ctx.data, "hello\n") == 0);

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    close(sv[1]);
    passed++;
#else
    passed++;
#endif
}

TEST(stream_readnum_smoke) {
#ifndef _WIN32
    int sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, (ccsocket_t)(intptr_t)sv[0], NULL);
    ASSERT(sock != NULL);
    ccsocket_set_nonblock((ccsocket_t)(intptr_t)sv[0], true);

    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ccev_timer_add(loop, 5000, CCEV_TIMER_ONCE,
                   (ccev_timer_cb)(void(*)(void))ccev_loop_stop, loop);

    ASSERT(ccev_stream_readnum(st, 5, 0, stream_on_data, &ctx) == CCEV_OK);
    write(sv[1], "hello", 5);

    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(ctx.called == 1);
    ASSERT(ctx.status == CCEV_OK);
    ASSERT(ctx.len == 5);
    ASSERT(strcmp(ctx.data, "hello") == 0);

    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    close(sv[1]);
    passed++;
#else
    passed++;
#endif
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

    printf("\n  %d passed, %d failed\n", passed, failed); fflush(stdout);
    return failed ? 1 : 0;
}
