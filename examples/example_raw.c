/**
 * @file example_raw.c
 * @brief Raw read with idle timeout — echo server.
 *
 *        Dispatches data as it arrives.  5-second idle timeout:
 *        if no data arrives for 5s, the connection is closed.
 *
 * Build: cc -I ../src -I ../deps/ccsocket/include \
 *            -I ../deps/ccalg/include -I ../deps/epoll/include \
 *            example_raw.c ../build/libccev.a -o example_raw
 *
 *        To tune stack buffer size at compile time:
 *            cc -DCCEV_READ_STACK_SIZE=4096 ...
 */

#include "ccev.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define IDLE_TIMEOUT_MS 5000

static void on_data(void *udata, const char *data, size_t len, int status) {
    ccev_stream_t *st = (ccev_stream_t *)udata;
    if (status != CCEV_OK) {
        printf("  timeout or error — closing\n");
        ccev_stream_close(st);
        return;
    }
    /* Echo back (fire-and-forget, zero copy via direct-send fast path) */
    ccev_stream_write(st, data, len, NULL, NULL);
}

static void on_close(void *udata) {
    ccev_stream_close((ccev_stream_t *)udata);
}

static void on_accept(void *udata, ccev_sock_t *client,
                       const char *ip, int port) {
    (void)udata;
    printf("connection from %s:%d\n", ip, port);
    ccev_stream_t *st = ccev_stream_open(client);
    if (!st) { ccev_sock_close(client); return; }
    ccev_stream_set_close_cb(st, on_close, st);
    /* limit=0 (unlimited), timeout=5s idle */
    ccev_stream_read(st, 0, IDLE_TIMEOUT_MS, on_data, st);
}

int main(void) {
    ccev_loop_t *loop = ccev_loop_create(64);
    if (!loop) return 1;

    ccev_listen(loop, "127.0.0.1", 12347, 1, CCEV_REUSEADDR, on_accept, NULL);
    printf("echo server on 127.0.0.1:12347 (raw mode, 5s idle timeout)\n");
    ccev_loop_run(loop, CCEV_RUN_FOREVER);
    ccev_loop_destroy(loop);
    return 0;
}
