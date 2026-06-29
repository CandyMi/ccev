/**
 * @file echo_server.c
 * @brief A simple TCP echo server using ccev (sock-level API).
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
#include "ccsocket.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void on_read(ccev_sock_t *sock, int events) {
    (void)events;
    ccsocket_t fd = ccev_sock_get_fd(sock);
    char buf[4096];
    int n;

    for (;;) {
        ccsocket_stcode_t rc = ccsocket_recv(fd, buf, sizeof(buf), &n);
        if (rc == CC_OPCODE_OK && n > 0) {
            int w;
            ccsocket_send(fd, buf, (size_t)n, &w);
        } else if (rc == CC_OPCODE_ERROR && n == 0) {
            printf("  [echo] client closed connection\n");
            ccev_sock_close(sock);
            return;
        } else {
            /* EAGAIN (CC_OPCODE_WAIT) or error — return.
             * ONESHOT re-arm happens automatically in the dispatch loop. */
            return;
        }
    }
}

static void on_close(void *udata) {
    ccev_sock_t *sock = (ccev_sock_t *)udata;
    printf("  [echo] connection closed\n");
    (void)sock;  /* sock is already in closing — no further action needed */
}

static void on_accept(void *udata, ccev_sock_t *client,
                       const char *ip, int port) {
    (void)udata;
    printf("  [echo] accept: %s:%d\n", ip, port);

    ccev_sock_set_close_cb(client, on_close, client);
    ccev_sock_read_start(client, on_read);
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

    ccev_sock_t *listener = ccev_listen(loop, host, port, 128,
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