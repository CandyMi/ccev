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

static void on_read(ccev_sock_t *sock, int events) {
    (void)events;
    ccsocket_t fd = ccev_sock_get_fd(sock);
    char buf[4096];

    for (;;) {
        int n;
        ccsocket_stcode_t rc = ccsocket_recv(fd, buf, sizeof(buf), &n);
        if (rc == CC_OPCODE_OK && n > 13) {
            /* Got enough data — send response immediately.
             * Keep-alive: return here and the ONESHOT re-arm will
             * fire on_read again for the next request. */
            int w;
            ccsocket_send(fd, RESPONSE, strlen(RESPONSE), &w);
            return;
        } else if (rc == CC_OPCODE_OK && n > 0) {
            /* Partial data — keep reading */
            continue;
        } else if (rc == CC_OPCODE_ERROR && n == 0) {
            ccev_sock_close(sock);
            return;
        } else {
            /* EAGAIN (CC_OPCODE_WAIT) or error — return.
             * ONESHOT re-arm happens automatically. */
            return;
        }
    }
}

static void on_close(void *udata) {
    ccev_sock_t *sock = (ccev_sock_t *)udata;
    (void)sock;  /* already in closing — no further action */
}

static void on_accept(void *udata, ccev_sock_t *client,
                       const char *ip, int port) {
    (void)udata; (void)ip; (void)port;
    ccev_sock_set_close_cb(client, on_close, client);
    ccev_sock_read_start(client, on_read);
}

int main(int argc, char **argv) {
    const char *host = argc > 2 ? argv[2] : "0.0.0.0";
    uint16_t port = (uint16_t)(argc > 1 ? atoi(argv[1]) : 8080);
    int backlog = argc > 3 ? atoi(argv[3]) : 4096;

    printf("ccev benchmark HTTP server: %s:%u (backlog=%d)\n",
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