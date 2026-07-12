/**
 * @file example_readline.c
 * @brief HTTP-like line protocol: read headers via readline,
 *        detect empty-line end, switch to readnum for body.
 *
 * Build: cc -I ../src -I ../deps/ccsocket/include \
 *            -I ../deps/ccalg/include -I ../deps/epoll/include \
 *            example_readline.c ../build/libccev.a -o example_readline
 *
 * Usage: ./example_readline
 *        (echo -e "X-Foo: bar\r\n\r\nABCD" | nc -N 127.0.0.1 12345)
 */

#include "ccev.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int  body_len;
static char body_buf[256];

/* ── Phase-2: readnum body callback ── */
static void on_body(void *udata, const char *data, size_t len, int status) {
    ccev_stream_t *st = (ccev_stream_t *)udata;
    if (status != CCEV_OK) { ccev_stream_close(st); return; }
    memcpy(body_buf, data, len);
    body_len = (int)len;
    printf("  body: %.*s\n", body_len, body_buf);
    ccev_stream_close(st);
}

/* ── Phase-1: readline header callback ── */
static void on_line(void *udata, const char *data, size_t len, int status) {
    ccev_stream_t *st = (ccev_stream_t *)udata;
    if (status != CCEV_OK) { ccev_stream_close(st); return; }

    /* Strip trailing \r\n or \n */
    size_t line_len = len;
    if (line_len >= 2 && data[line_len-2] == '\r') line_len -= 2;
    else if (line_len >= 1 && data[line_len-1] == '\n') line_len -= 1;

    if (line_len == 0) {
        /* Empty line → end of headers → read 4-byte body */
        printf("  headers complete, reading body...\n");
        ccev_stream_readnum(st, 4, 5000, on_body, st);
        return;
    }
    printf("  header: %.*s\n", (int)line_len, data);

    /* Read next header line (same delimiter, max 8192 bytes) */
    ccev_stream_readline(st, '\n', 8192, 0, on_line, st);
}

static void on_accept(void *udata, ccev_sock_t *client,
                       const char *ip, int port) {
    (void)udata;
    printf("connection from %s:%d\n", ip, port);
    ccev_stream_t *st = ccev_stream_open(client);
    if (!st) { ccev_sock_close(client); return; }
    ccev_stream_readline(st, '\n', 8192, 0, on_line, st);
}

int main(void) {
    ccev_loop_t *loop = ccev_loop_create(64);
    if (!loop) return 1;

    ccev_listen(loop, "127.0.0.1", 12345, 1, CCEV_REUSEADDR, on_accept, NULL);
    printf("listening on 127.0.0.1:12345 ...\n");
    ccev_loop_run(loop, CCEV_RUN_FOREVER);
    ccev_loop_destroy(loop);
    return 0;
}
