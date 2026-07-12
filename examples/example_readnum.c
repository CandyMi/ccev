/**
 * @file example_readnum.c
 * @brief Binary protocol: fixed-length header + variable-length body.
 *
 *        Frame: [u32:type][u32:body_len][body...]
 *        Read 8-byte header via readnum, parse body_len,
 *        switch to readnum for body.
 *
 * Build: cc -I ../src -I ../deps/ccsocket/include \
 *            -I ../deps/ccalg/include -I ../deps/epoll/include \
 *            example_readnum.c ../build/libccev.a -o example_readnum
 */

#include "ccev.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Phase-2: body callback ── */
static void on_body(void *udata, const char *data, size_t len, int status) {
    ccev_stream_t *st = (ccev_stream_t *)udata;
    if (status != CCEV_OK) { ccev_stream_close(st); return; }
    printf("  body (%zu bytes): %.*s\n", len, (int)len, data);
    ccev_stream_close(st);
}

/* ── Phase-1: 8-byte fixed header ── */
static void on_header(void *udata, const char *data, size_t len, int status) {
    ccev_stream_t *st = (ccev_stream_t *)udata;
    if (status != CCEV_OK || len < 8) { ccev_stream_close(st); return; }

    /* Parse: 4 bytes big-endian type, 4 bytes big-endian body_len */
    unsigned char *p = (unsigned char *)data;
    uint32_t type     = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                      | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
    uint32_t body_len = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16)
                      | ((uint32_t)p[6] << 8)  |  (uint32_t)p[7];
    printf("  header: type=%u body_len=%u\n", type, body_len);

    if (body_len == 0) { ccev_stream_close(st); return; }
    ccev_stream_readnum(st, body_len, 0, on_body, st);
}

static void on_accept(void *udata, ccev_sock_t *client,
                       const char *ip, int port) {
    (void)udata;
    printf("connection from %s:%d\n", ip, port);
    ccev_stream_t *st = ccev_stream_open(client);
    if (!st) { ccev_sock_close(client); return; }
    ccev_stream_readnum(st, 8, 0, on_header, st);
}

int main(void) {
    ccev_loop_t *loop = ccev_loop_create(64);
    if (!loop) return 1;

    ccev_listen(loop, "127.0.0.1", 12346, 1, CCEV_REUSEADDR, on_accept, NULL);
    printf("listening on 127.0.0.1:12346 ...\n");
    ccev_loop_run(loop, CCEV_RUN_FOREVER);
    ccev_loop_destroy(loop);
    return 0;
}
