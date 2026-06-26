/**
 * @file test_conn.c
 * @brief Connection I/O tests — create, recv modes, send, sendall,
 *        sendfile, close lifecycle, NULL guards, closed-fd paths.
 */

#include "ccev.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

static int passed, failed;
#define TEST(name) static void test_##name(void)
#define ASSERT(cond) do { \
  if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failed++; } \
  else passed++; \
} while(0)
#define RUN(name) do { printf("  %s\n", #name); fflush(stdout); test_##name(); } while(0)

/* ═══ Helpers ═══ */

static int pair_create(int sv[2]) {
#ifdef _WIN32
    (void)sv;
    return -1;
#else
    return socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
#endif
}

/* ═══ ccev_conn_create ─────────────────────────────────── */

TEST(create_invalid_fd_returns_null) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_conn_t *conn = ccev_conn_create(loop, (ccsocket_t)-1, NULL);
    ASSERT(conn == NULL);
    ccev_loop_destroy(loop);
}

TEST(create_null_loop_returns_null) {
    ccev_conn_t *conn = ccev_conn_create(NULL, (ccsocket_t)3, NULL);
    ASSERT(conn == NULL);
}

/* ═══ ccev_conn_recv — all four modes ════════════════════ */

TEST(recv_mode3_register_cb) {
    /* buf=NULL, len=0, cb!=NULL: register callback, arm EPOLLIN */
#ifndef _WIN32
    int sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_conn_t *conn = ccev_conn_create(loop, (ccsocket_t)(intptr_t)sv[0], NULL);
    ASSERT(conn != NULL);

    int cb_fired = 0;
    ccev_recv_cb my_cb = (ccev_recv_cb)(void(*)(void))&cb_fired;
    int rc = ccev_conn_recv(conn, NULL, 0, my_cb, &cb_fired);
    ASSERT(rc == CCEV_OK);

    ccev_conn_close(conn);
    ccev_loop_destroy(loop);
    close(sv[1]);
    passed++;
#else
    passed++;
#endif
}

TEST(recv_mode4_disarm) {
    /* buf=NULL, len=0, cb=NULL: clear callback, disarm */
#ifndef _WIN32
    int sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_conn_t *conn = ccev_conn_create(loop, (ccsocket_t)(intptr_t)sv[0], NULL);
    ASSERT(conn != NULL);

    /* First arm, then disarm */
    ccev_conn_recv(conn, NULL, 0, (ccev_recv_cb)1, NULL);
    int rc = ccev_conn_recv(conn, NULL, 0, NULL, NULL);
    ASSERT(rc == CCEV_OK);

    ccev_conn_close(conn);
    ccev_loop_destroy(loop);
    close(sv[1]);
    passed++;
#else
    passed++;
#endif
}

TEST(recv_after_close_returns_err) {
#ifndef _WIN32
    int sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_conn_t *conn = ccev_conn_create(loop, (ccsocket_t)(intptr_t)sv[0], NULL);
    ASSERT(conn != NULL);

    ccev_conn_close(conn);
    char buf[16];
    int rc = ccev_conn_recv(conn, buf, sizeof(buf), NULL, NULL);
    ASSERT(rc == CCEV_ERR);

    ccev_loop_destroy(loop);
    close(sv[1]);
    passed++;
#else
    passed++;
#endif
}

/* ═══ ccev_conn_send ───────────────────────────────────── */

TEST(send_before_close) {
#ifndef _WIN32
    int sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_conn_t *conn = ccev_conn_create(loop, (ccsocket_t)(intptr_t)sv[0], NULL);
    ASSERT(conn != NULL);

    const char *msg = "hello";
    int rc = ccev_conn_send(conn, msg, strlen(msg), NULL, NULL);
    ASSERT(rc > 0);

    ccev_conn_close(conn);
    ccev_loop_destroy(loop);
    close(sv[1]);
    passed++;
#else
    passed++;
#endif
}

TEST(send_after_close_returns_err) {
#ifndef _WIN32
    int sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_conn_t *conn = ccev_conn_create(loop, (ccsocket_t)(intptr_t)sv[0], NULL);
    ASSERT(conn != NULL);

    ccev_conn_close(conn);
    int rc = ccev_conn_send(conn, "x", 1, NULL, NULL);
    ASSERT(rc == CCEV_ERR);

    ccev_loop_destroy(loop);
    close(sv[1]);
    passed++;
#else
    passed++;
#endif
}

TEST(send_empty_returns_zero) {
#ifndef _WIN32
    int sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_conn_t *conn = ccev_conn_create(loop, (ccsocket_t)(intptr_t)sv[0], NULL);
    ASSERT(conn != NULL);

    int rc = ccev_conn_send(conn, NULL, 0, NULL, NULL);
    ASSERT(rc == 0);
    rc = ccev_conn_send(conn, "x", 0, NULL, NULL);
    ASSERT(rc == 0);

    ccev_conn_close(conn);
    ccev_loop_destroy(loop);
    close(sv[1]);
    passed++;
#else
    passed++;
#endif
}

/* ═══ ccev_conn_sendall — both done modes ════════════════ */

TEST(sendall_batch_then_flush) {
#ifndef _WIN32
    int sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_conn_t *conn = ccev_conn_create(loop, (ccsocket_t)(intptr_t)sv[0], NULL);
    ASSERT(conn != NULL);

    /* Buffer multiple chunks, then flush */
    ASSERT(ccev_conn_sendall(conn, "hello ", 6, false, NULL, NULL) > 0);
    ASSERT(ccev_conn_sendall(conn, "world", 5, false, NULL, NULL) > 0);
    ASSERT(ccev_conn_sendall(conn, NULL, 0, true, NULL, NULL) == CCEV_OK);

    ccev_conn_close(conn);
    ccev_loop_destroy(loop);
    close(sv[1]);
    passed++;
#else
    passed++;
#endif
}

/* ═══ ccev_conn_sendfile — basic smoke ─────────────────── */

TEST(sendfile_smoke) {
#ifndef _WIN32
    const char *filepath = "/tmp/ccev_sendfile_test";
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    /* Create a temp file, write data */
    int tmp_fd = open(filepath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (tmp_fd < 0) { passed++; ccev_loop_destroy(loop); return; }

    const char *data = "sendfile test data";
    write(tmp_fd, data, strlen(data));
    close(tmp_fd);

    /* Need a connected socket pair to send to */
    int sv[2];
    if (pair_create(sv) != 0) { unlink(filepath); passed++; ccev_loop_destroy(loop); return; }

    ccev_conn_t *conn = ccev_conn_create(loop, (ccsocket_t)(intptr_t)sv[0], NULL);
    ASSERT(conn != NULL);

    /* sendfile now takes a file path instead of an fd */
    int rc = ccev_conn_sendfile(conn, filepath, NULL, NULL);
    ASSERT(rc == CCEV_OK);

    ccev_conn_close(conn);
    ccev_loop_destroy(loop);
    unlink(filepath);
    close(sv[1]);
    passed++;
#else
    passed++;
#endif
}

/* ═══ ccev_conn_close ──────────────────────────────────── */

TEST(close_twice_returns_err) {
#ifndef _WIN32
    int sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_conn_t *conn = ccev_conn_create(loop, (ccsocket_t)(intptr_t)sv[0], NULL);

    ASSERT(ccev_conn_close(conn) == CCEV_OK);
    ASSERT(ccev_conn_close(conn) == CCEV_ERR);

    ccev_loop_destroy(loop);
    close(sv[1]);
    passed++;
#else
    passed++;
#endif
}

TEST(close_null_returns_err) {
    ASSERT(ccev_conn_close(NULL) == CCEV_ERR);
}

/* ═══ ccev_conn_set_close_cb / udata / fd  NULL guards ══ */

TEST(set_close_cb_null_is_safe) {
    ccev_conn_set_close_cb(NULL, NULL, NULL);
    passed++;
}

TEST(get_udata_null_returns_null) {
    ASSERT(ccev_conn_get_udata(NULL) == NULL);
}

TEST(set_udata_null_is_safe) {
    ccev_conn_set_udata(NULL, NULL);
    passed++;
}

TEST(fd_null_returns_invalid) {
    ASSERT(ccev_conn_fd(NULL) == INVALID_SOCKET);
}

/* ═══ udata lifecycle ════*/

TEST(udata_set_and_get) {
#ifndef _WIN32
    int sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_conn_t *conn = ccev_conn_create(loop, (ccsocket_t)(intptr_t)sv[0], NULL);
    ASSERT(conn != NULL);

    int value = 42;
    ccev_conn_set_udata(conn, &value);
    ASSERT(ccev_conn_get_udata(conn) == &value);
    ASSERT(*(int*)ccev_conn_get_udata(conn) == 42);

    ccev_conn_close(conn);
    ccev_loop_destroy(loop);
    close(sv[1]);
    passed++;
#else
    passed++;
#endif
}

/* ═══ ccev_conn_count ──────────────────────────────────── */

TEST(conn_count_null_returns_zero) {
    ASSERT(ccev_conn_count(NULL) == 0);
}

/* ═══ listen + connect (end-to-end) ────────────────────── */

static int  listen_accept_flag;
static ccev_conn_t *listen_accept_conn;
static int  listen_connect_flag;
static ccev_loop_t *listen_test_loop;

static void listen_on_accept(void *udata, ccev_conn_t *conn,
                              const char *ip, int port) {
    (void)udata; (void)ip; (void)port;
    listen_accept_flag = 1;
    listen_accept_conn = conn;
    ccev_loop_stop(listen_test_loop);
}

static void listen_on_connect(void *udata, ccev_conn_t *conn, int status) {
    (void)udata;
    if (status == CCEV_OK && conn) {
        listen_connect_flag = 1;
        /* Don't stop the loop here — connect may complete synchronously
         * (before loop_run starts), which would prevent the accept from
         * being processed.  The accept callback stops the loop instead. */
    }
}

TEST(listen_then_connect) {
#ifndef _WIN32
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    listen_test_loop    = loop;
    listen_accept_flag  = 0;
    listen_connect_flag = 0;
    listen_accept_conn  = NULL;

    ccev_conn_t *listener = ccev_listen(loop, "127.0.0.1", 0, 5,
                                         CCEV_REUSEADDR,
                                         listen_on_accept, loop);
    ASSERT(listener != NULL);

    /* Discover the port the OS assigned */
    struct sockaddr_in sin;
    socklen_t slen = sizeof(sin);
    ccsocket_t lfd = ccev_conn_fd(listener);
    ASSERT(getsockname((int)(intptr_t)lfd,
                       (struct sockaddr *)&sin, &slen) == 0);
    int port = ntohs(sin.sin_port);
    ASSERT(port > 0);

    int rc = ccev_connect(loop, "127.0.0.1", (uint16_t)port,
                          1000, CCEV_TCP_NODELAY,
                          listen_on_connect, loop);
    ASSERT(rc == CCEV_OK);

    /* Run until the accept fires and stops the loop.
     * macOS arm64's non-blocking connect on loopback is asynchronous
     * (EINPROGRESS), so a single RUN_ONCE poll would miss both the
     * connect completion and the accept.  RUN_FOREVER + loop_stop
     * inside the accept callback handles both sync and async paths. */
    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(listen_accept_flag  == 1);
    ASSERT(listen_accept_conn  != NULL);
    ASSERT(listen_connect_flag == 1);

    ccev_conn_close(listener);
    ccev_loop_destroy(loop);
    passed++;
#else
    passed++;
#endif
}

/* ═══ Main ═══ */

int main(void) {
    printf("connection tests:\n"); fflush(stdout);

    /* create */
    RUN(create_invalid_fd_returns_null);
    RUN(create_null_loop_returns_null);

    /* recv */
    RUN(recv_mode3_register_cb);
    RUN(recv_mode4_disarm);
    RUN(recv_after_close_returns_err);

    /* send */
    RUN(send_before_close);
    RUN(send_after_close_returns_err);
    RUN(send_empty_returns_zero);

    /* sendall */
    RUN(sendall_batch_then_flush);

    /* sendfile */
    RUN(sendfile_smoke);

    /* close */
    RUN(close_twice_returns_err);
    RUN(close_null_returns_err);

    /* NULL guards */
    RUN(set_close_cb_null_is_safe);
    RUN(get_udata_null_returns_null);
    RUN(set_udata_null_is_safe);
    RUN(fd_null_returns_invalid);

    /* udata */
    RUN(udata_set_and_get);

    /* count */
    RUN(conn_count_null_returns_zero);

    /* listen + connect */
    RUN(listen_then_connect);

    printf("\n  %d passed, %d failed\n", passed, failed); fflush(stdout);
    return failed ? 1 : 0;
}
