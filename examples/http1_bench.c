/**
 * @file http1_bench.c
 * @brief HTTP/1.1 benchmark server using ccev_stream_t readline + write.
 *
 * Designed for wrk / ab / hey:
 *   wrk -t4 -c100 -d30s http://127.0.0.1:8080/
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

static void on_body_line(void *udata, const char *data,
                          size_t len, int status);
static void on_request_line(void *udata, const char *data,
                             size_t len, int status);

static void on_body_line(void *udata, const char *data,
                          size_t len, int status) {
    ccev_stream_t *st = (ccev_stream_t *)udata;
    if (status != CCEV_OK || len == 0) { ccev_stream_close(st); return; }

    /* End of headers (empty line "\r\n" or "\n")? */
    if ((len == 1 && data[0] == '\n') ||
        (len == 2 && data[0] == '\r' && data[1] == '\n')) {
        ccev_stream_write(st, RESPONSE, strlen(RESPONSE), NULL, NULL);
        ccev_stream_readline(st, '\n', 8192, 0, on_request_line, st);
        return;
    }
    ccev_stream_readline(st, '\n', 8192, 0, on_body_line, st);
}

static void on_request_line(void *udata, const char *data,
                             size_t len, int status) {
    ccev_stream_t *st = (ccev_stream_t *)udata;
    if (status != CCEV_OK || len == 0) { ccev_stream_close(st); return; }
    ccev_stream_readline(st, '\n', 8192, 0, on_body_line, st);
}

static void on_close(void *udata) {
    ccev_stream_t *st = (ccev_stream_t *)udata;
    ccev_stream_close(st);
}

static void on_accept(void *udata, ccev_sock_t *client,
                       const char *ip, int port) {
    (void)udata; (void)ip; (void)port;
    ccev_stream_t *st = ccev_stream_open(client);
    if (!st) { ccev_sock_close(client); return; }
    ccev_stream_set_close_cb(st, on_close, st);
    ccev_stream_readline(st, '\n', 8192, 0, on_request_line, st);
}

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
