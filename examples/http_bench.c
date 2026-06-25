/**
 * @file http_bench.c
 * @brief Minimal HTTP/1.1 server for benchmarking ccev throughput.
 *
 * Designed for use with wrk / ab / hey:
 *   wrk -t4 -c100 -d30s http://127.0.0.1:8080/
 *
 * Serves a fixed "Hello, World!" response with Keep-Alive support.
 * No parsing — any valid HTTP GET is answered identically.
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

static void on_sent(void *udata) {
    /* Write complete — conn is ready for next request */
}

static void on_recv(void *udata) {
    ccev_conn_t *conn = (ccev_conn_t *)udata;
    char buf[4096];
    int n;

    for (;;) {
        n = ccev_conn_recv(conn, buf, sizeof(buf), NULL, NULL);
        if (n > 13) {
            /* Got enough data — send response immediately */
            ccev_conn_send(conn, RESPONSE, strlen(RESPONSE), on_sent, NULL);
            return;
        } else if (n == 0) {
            ccev_conn_close(conn);
            return;
        } else if (n < 0) {
            ccev_conn_close(conn);
            return;
        } else {
            /* EAGAIN — wait for more data */
            return;
        }
    }
}

static void on_close(void *udata) {
    ccev_conn_t *conn = (ccev_conn_t *)udata;
    ccev_conn_close(conn);
}

static void on_accept(void *udata, ccev_conn_t *conn,
                       const char *ip, int port) {
    (void)udata;
    ccev_conn_set_close_cb(conn, on_close, conn);
    ccev_conn_recv(conn, NULL, 0, on_recv, conn);
}

int main(int argc, char **argv) {
    const char *host = argc > 2 ? argv[2] : "0.0.0.0";
    const char *port = argc > 1 ? argv[1] : "8080";
    int backlog = argc > 3 ? atoi(argv[3]) : 4096;

    printf("ccev benchmark HTTP server: %s:%s (backlog=%d)\n",
           host, port, backlog);

    ccev_loop_t *loop = ccev_loop_create(1024);
    if (!loop) { fprintf(stderr, "loop_create failed\n"); return 1; }

    ccev_conn_t *l = ccev_listen(loop, host, port, backlog,
                                   CCEV_REUSEADDR, on_accept, NULL);
    if (!l) {
        fprintf(stderr, "listen failed on %s:%s\n", host, port);
        ccev_loop_destroy(loop);
        return 1;
    }

    ccev_loop_run(loop, CCEV_RUN_FOREVER);
    ccev_loop_destroy(loop);
    return 0;
}
