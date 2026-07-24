/**
 * @file http1_bench.c
 * @brief HTTP/1.1 benchmark server using ccev_stream_t raw read + write.
 *
 * Reads chunks via ccev_stream_read(), parses complete lines from an
 * internal buffer, and sends a fixed response on empty-line (end of
 * headers).  Designed for wrk / ab / hey:
 *   wrk -t4 -c100 -d30s http://127.0.0.1:8080/
 *
 * Build: cc -I ../src -I ../deps/ccsocket/include \
 *            -I ../deps/ccalg/include -I ../deps/epoll/include \
 *            http1_bench.c ../build/libccev.a -o http1_bench
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

/* Per-connection state: partial line buffer between reads */
struct http_conn {
    ccev_stream_t *st;
    char           buf[8192];
    size_t         len;
};

static void on_data(void *udata, const char *data, size_t len, int status);

static void conn_start_read(struct http_conn *conn) {
    ccev_stream_read(conn->st, 4096, 0, on_data, conn);
}

static void on_data(void *udata, const char *data, size_t len, int status) {
    struct http_conn *conn = (struct http_conn *)udata;

    if (status != CCEV_OK || len == 0) {
        free(conn);
        return;
    }

    /* Append new data to buffer */
    size_t cap = sizeof(conn->buf) - conn->len;
    size_t to_copy = len < cap ? len : cap;
    memcpy(conn->buf + conn->len, data, to_copy);
    conn->len += to_copy;

    /* Scan for complete lines from the start of the buffer */
    char *p   = conn->buf;
    char *end = conn->buf + conn->len;

    while (p < end) {
        char *nl = (char *)memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;  /* incomplete line — wait for more data */

        size_t line_len = (size_t)(nl - p) + 1;

        /* Empty line = end of headers → send response */
        if ((line_len == 1 && p[0] == '\n') ||
            (line_len == 2 && p[0] == '\r' && p[1] == '\n')) {
            ccev_stream_write(conn->st, RESPONSE, strlen(RESPONSE),
                               NULL, NULL);
        }

        p += line_len;
    }

    /* Compact consumed bytes */
    if (p > conn->buf) {
        size_t remaining = (size_t)(end - p);
        memmove(conn->buf, p, remaining);
        conn->len = remaining;
    }

    /* Continue reading */
    conn_start_read(conn);
}

static void on_close(void *udata) {
    struct http_conn *conn = (struct http_conn *)udata;
    ccev_stream_close(conn->st);
    free(conn);
}

static void on_accept(void *udata, ccev_sock_t *client,
                       const char *ip, int port) {
    (void)udata; (void)ip; (void)port;

    ccev_stream_t *st = ccev_stream_open(client);
    if (!st) { ccev_sock_close(client); return; }

    struct http_conn *conn = (struct http_conn *)malloc(sizeof(*conn));
    if (!conn) { ccev_stream_close(st); return; }

    conn->st  = st;
    conn->len = 0;

    ccev_stream_set_close_cb(st, on_close, conn);
    conn_start_read(conn);
}

int main(int argc, char **argv) {
    const char *host = argc > 2 ? argv[2] : "0.0.0.0";
    uint16_t port   = (uint16_t)(argc > 1 ? atoi(argv[1]) : 8080);
    int backlog     = argc > 3 ? atoi(argv[3]) : 4096;

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
