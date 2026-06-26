/**
 * @file echo_server.c
 * @brief A simple TCP echo server using ccev.
 *
 * Usage: ./echo_server [port] [address]
 *   Default: 0.0.0.0:8080
 *
 * Every received byte is echoed back. Run with telnet or nc:
 *   $ nc 127.0.0.1 8080
 *   Hello
 *   Hello
 */

#include "ccev.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void on_sent(void *udata) {
    (void)udata;  /* nothing to do — echo is fire-and-forget */
}

static void on_recv(void *udata) {
    ccev_conn_t *conn = (ccev_conn_t *)udata;
    char buf[4096];
    int n;

    /* Read all available data and echo back */
    for (;;) {
        n = ccev_conn_recv(conn, buf, sizeof(buf), NULL, NULL);
        if (n > 0) {
            ccev_conn_send(conn, buf, (size_t)n, on_sent, NULL);
        } else if (n == 0) {
            printf("  [echo] client closed connection\n");
            ccev_conn_close(conn);
            return;
        } else {
            /* EAGAIN or error (-1): return, epoll will re-arm */
            return;
        }
    }
}

static void on_close(void *udata) {
    ccev_conn_t *conn = (ccev_conn_t *)udata;
    printf("  [echo] connection closed\n");
    ccev_conn_close(conn);
}

static void on_accept(void *udata, ccev_conn_t *conn,
                       const char *ip, int port) {
    (void)udata;
    printf("  [echo] accept: %s:%d\n", ip, port);

    ccev_conn_set_close_cb(conn, on_close, conn);
    ccev_conn_recv(conn, NULL, 0, on_recv, conn);
}

int main(int argc, char **argv) {
    const char *host = (argc > 2) ? argv[2] : "0.0.0.0";
    uint16_t port = (uint16_t)((argc > 1) ? atoi(argv[1]) : 8080);

    printf("ccev echo server starting on %s:%u ...\n", host, port);

    ccev_loop_t *loop = ccev_loop_create(1024);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop\n");
        return 1;
    }

    ccev_conn_t *listener = ccev_listen(loop, host, port, 128,
                                         CCEV_REUSEADDR, on_accept, NULL);
    if (!listener) {
        fprintf(stderr, "Failed to listen on %s:%u\n", host, port);
        ccev_loop_destroy(loop);
        return 1;
    }

    printf("  listening... (stop with Ctrl+C)\n");
    ccev_loop_run(loop, CCEV_RUN_FOREVER);
    ccev_loop_destroy(loop);
    printf("bye.\n");
    return 0;
}
