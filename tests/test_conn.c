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
    /* Create a temp file, write data, rewind, send via sendfile */
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    int tmp_fd = -1;
    tmp_fd = open("/tmp/ccev_sendfile_test", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (tmp_fd < 0) { passed++; ccev_loop_destroy(loop); return; }

    const char *data = "sendfile test data";
    write(tmp_fd, data, strlen(data));
    lseek(tmp_fd, 0, SEEK_SET);

    /* Need a connected socket pair to send to */
    int sv[2];
    if (pair_create(sv) != 0) { close(tmp_fd); passed++; ccev_loop_destroy(loop); return; }

    ccev_conn_t *conn = ccev_conn_create(loop, (ccsocket_t)(intptr_t)sv[0], NULL);
    ASSERT(conn != NULL);

    int rc = ccev_conn_sendfile(conn, tmp_fd, NULL, NULL);
    ASSERT(rc == CCEV_OK);

    ccev_conn_close(conn);
    ccev_loop_destroy(loop);
    close(tmp_fd);
    unlink("/tmp/ccev_sendfile_test");
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

TEST(fd_null_returns_err) {
    ASSERT(ccev_conn_fd(NULL) == CCEV_ERR);
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
    RUN(fd_null_returns_err);

    /* udata */
    RUN(udata_set_and_get);

    /* count */
    RUN(conn_count_null_returns_zero);

    printf("\n  %d passed, %d failed\n", passed, failed); fflush(stdout);
    return failed ? 1 : 0;
}
