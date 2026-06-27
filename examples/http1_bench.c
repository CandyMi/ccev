/**
 * @file http1_bench.c
 * @brief HTTP/1.1 benchmark server using the high-level ccev_stream_t API.
 *
 * Designed for wrk / ab / hey:
 *   wrk -t4 -c100 -d30s http://127.0.0.1:8080/
 *
 * Reads the HTTP request line-by-line using ccev_stream_readline,
 * then sends a fixed "Hello, World!" response with Keep-Alive.
 *
 * Key design: the response write is fire-and-forget (no on_sent callback).
 * The next ccev_stream_readline is started immediately after writing,
 * so there is never a window where st->reader == NULL but the socket
 * is re-armed for EPOLLIN (which would cause a busy-loop on leftover
 * kernel data).
 */

#include "ccev.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define RESPONSE "HTTP/1.1 200 OK\r\n" \
                 "Content-Type: text/plain\r\n" \
                 "Content-Length: 13\r\n" \
                 "Connection: keep-alive\r\n" \
                 "Server: ccev\r\n" \
                 "\r\n" \
                 "Hello, World!"

/* ── Forward declarations ── */

static void on_request_line(void *udata, const char *data,
                             size_t len, int status);
static void on_header_line(void *udata, const char *data,
                            size_t len, int status);

/* ── Callbacks ── */

/** Called after reading one header line.  Empty line = end of headers. */
static void on_header_line(void *udata, const char *data,
                            size_t len, int status) {
    ccev_stream_t *st = (ccev_stream_t *)udata;
    if (status != CCEV_OK || len == 0) {
        ccev_stream_close(st);
        return;
    }

    /* Detect end of headers: "\r\n" (len=2) or "\n" (len=1). */
    if ((len == 1 && data[0] == '\n') ||
        (len == 2 && data[0] == '\r' && data[1] == '\n')) {
        /* Headers complete — send the fixed response (fire-and-forget)
         * and immediately start reading the next request.
         * The reader stays active, avoiding the busy-loop window. */
        ccev_stream_write(st, RESPONSE, strlen(RESPONSE), NULL, NULL);
        ccev_stream_readline(st, '\n', 8192, 0, on_request_line, st);
        return;
    }

    /* More headers — read the next line. */
    ccev_stream_readline(st, '\n', 8192, 0, on_header_line, st);
}

/** Called after reading the request line — read headers next. */
static void on_request_line(void *udata, const char *data,
                             size_t len, int status) {
    ccev_stream_t *st = (ccev_stream_t *)udata;
    if (status != CCEV_OK || len == 0) {
        ccev_stream_close(st);
        return;
    }

    /* Got the request line (e.g. "GET / HTTP/1.1\r\n").
     * No parsing needed — just read headers. */
    ccev_stream_readline(st, '\n', 8192, 0, on_header_line, st);
}

/** Called when the connection is closed by the peer or on error. */
static void on_close(void *udata) {
    ccev_stream_t *st = (ccev_stream_t *)udata;
    ccev_stream_close(st);  /* frees write buffers and reader */
}

/** Accept callback — upgrade the client socket to a stream. */
static void on_accept(void *udata, ccev_sock_t *client,
                       const char *ip, int port) {
    (void)udata; (void)ip; (void)port;

    ccev_stream_t *st = ccev_stream_open(client);
    if (!st) {
        ccev_sock_close(client);
        return;
    }

    ccev_stream_set_close_cb(st, on_close, st);
    ccev_stream_readline(st, '\n', 8192, 0, on_request_line, st);
}

/* ── Main ── */

int main(int argc, char **argv) {
    const char *host = argc > 2 ? argv[2] : "0.0.0.0";
    uint16_t port  = (uint16_t)(argc > 1 ? atoi(argv[1]) : 8080);
    int backlog    = argc > 3 ? atoi(argv[3]) : 4096;

    printf("ccev HTTP/1.1 stream benchmark: %s:%u (backlog=%d)\n",
           host, port, backlog);

    ccev_loop_t *loop = ccev_loop_create(1024);
    if (!loop) { fprintf(stderr, "loop_create failed\n"); return 1; }

    ccev_sock_t *l = ccev_listen(loop, host, port, backlog,
                                   CCEV_REUSEADDR, on_accept, NULL);
    if (!l) {
        fprintf(stderr, "listen failed on %s:%u\n", host, port);
        ccev_loop_destroy(loop);
        return 1;
    }

    ccev_loop_run(loop, CCEV_RUN_FOREVER);
    ccev_loop_destroy(loop);
    return 0;
}