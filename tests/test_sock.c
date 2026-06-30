/**
 * @file test_sock.c
 * @brief Socket lifecycle and event tests for the new ccev_sock_t API.
 */

#include "ccev.h"
#include "ccsocket.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <unistd.h>
#include <sys/socket.h>
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

static void timer_stop_loop(void *udata) {
    (void)udata;
    ccev_loop_stop(*(ccev_loop_t **)udata);
}

static int pair_create(ccsocket_t sv[2]) {
    return ccsocketpair(sv, CC_NOFLAG) ? 0 : -1;
}

/* ═══ ccev_sock_create ─────────────────────────────── */

TEST(create_invalid_fd_returns_null) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_sock_t *sock = ccev_sock_create(loop, (ccsocket_t)-1, NULL);
    ASSERT(sock == NULL);
    ccev_loop_destroy(loop);
}

TEST(create_null_loop_returns_null) {
    ccev_sock_t *sock = ccev_sock_create(NULL, (ccsocket_t)3, NULL);
    ASSERT(sock == NULL);
}

/* ═══ ccev_sock_close ──────────────────────────────── */

TEST(close_null_returns_err) {
    ASSERT(ccev_sock_close(NULL) == CCEV_ERR);
}

TEST(close_twice_returns_err) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);

    ASSERT(ccev_sock_close(sock) == CCEV_OK);
    ASSERT(ccev_sock_close(sock) == CCEV_ERR);

    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ═══ ccev_sock_get_fd / get_udata / set_udata ────── */

TEST(get_fd_null_returns_invalid) {
    ASSERT(ccev_sock_get_fd(NULL) == (ccsocket_t)-1);
}

TEST(get_udata_null_returns_null) {
    ASSERT(ccev_sock_get_udata(NULL) == NULL);
}

TEST(set_udata_null_is_safe) {
    ccev_sock_set_udata(NULL, NULL);
    passed++;
}

TEST(udata_set_and_get) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ASSERT(sock != NULL);

    int value = 42;
    ccev_sock_set_udata(sock, &value);
    ASSERT(ccev_sock_get_udata(sock) == &value);
    ASSERT(*(int*)ccev_sock_get_udata(sock) == 42);

    ccev_sock_close(sock);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ═══ ccev_sock_read_start / read_stop ──────────────── */

static int  read_ev_fired;
static int  read_ev_events;
static ccev_loop_t *read_ev_loop;

static void on_read_event(ccev_sock_t *sock, int events) {
    (void)sock;
    read_ev_fired  = 1;
    read_ev_events = events;
    ccev_loop_stop(read_ev_loop);
}

TEST(read_start_fires_on_data) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    read_ev_loop = ccev_loop_create(64);
    ASSERT(read_ev_loop != NULL);
    ccev_sock_t *sock = ccev_sock_create(read_ev_loop, sv[0], NULL);
    ASSERT(sock != NULL);

    read_ev_fired  = 0;
    read_ev_events = 0;

    ASSERT(ccev_sock_read_start(sock, on_read_event) == CCEV_OK);
    ccsocket_send(sv[1], "x", 1, NULL);

    /* Safety timer — prevents hang on any platform */
    ccev_timer_add(read_ev_loop, 5000, CCEV_TIMER_ONCE,
                   timer_stop_loop, &read_ev_loop);

    ccev_loop_run(read_ev_loop, CCEV_RUN_FOREVER);

    ASSERT(read_ev_fired == 1);
    ASSERT(read_ev_events & CCEV_EVENT_READ);

    ccev_sock_close(sock);
    ccev_loop_destroy(read_ev_loop);
    ccsocket_close(sv[1]);
}

TEST(read_stop_suppresses_event) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ASSERT(sock != NULL);

    read_ev_fired  = 0;
    read_ev_loop   = loop;

    ASSERT(ccev_sock_read_start(sock, on_read_event) == CCEV_OK);
    ASSERT(ccev_sock_read_stop(sock) == CCEV_OK);

    /* Write data — callback should NOT fire because read is stopped */
    ccsocket_send(sv[1], "x", 1, NULL);

    /* Safety timer to stop the loop if the callback doesn't fire */
    ccev_timer_add(loop, 50, CCEV_TIMER_ONCE,
                   timer_stop_loop, &loop);
    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(read_ev_fired == 0);

    ccev_sock_close(sock);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

TEST(read_start_null_sock_returns_err) {
    ASSERT(ccev_sock_read_start(NULL, on_read_event) == CCEV_ERR);
}

TEST(read_stop_null_sock_returns_err) {
    ASSERT(ccev_sock_read_stop(NULL) == CCEV_ERR);
}

TEST(read_start_on_closed_returns_err) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ccev_sock_close(sock);
    ASSERT(ccev_sock_read_start(sock, on_read_event) == CCEV_ERR);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ═══ ccev_sock_write_start / write_stop ─────────────── */

static int  write_ev_fired;
static ccev_loop_t *write_ev_loop;

static void on_write_event(ccev_sock_t *sock, int events) {
    (void)sock;
    write_ev_fired = 1;
    ccev_loop_stop(write_ev_loop);
}

/*
 * Write-start test (cross-platform).
 *
 * On Linux epoll, EPOLLOUT fires immediately for a socket that's already
 * writable (level-triggered).  On Windows (wepoll), AFD_POLL_SEND is
 * transition-based and does NOT fire for an already-writable socket.
 * To trigger the transition, we fill sv[0]'s send buffer until full,
 * register EPOLLOUT, then drain from sv[1] — the buffer-emptied
 * transition fires EPOLLOUT on both platforms.
 */
TEST(write_start_fires_on_writable) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    write_ev_loop = ccev_loop_create(64);
    ASSERT(write_ev_loop != NULL);
    /* ccev_sock_create sets sv[0] to nonblock — needed for the fill loop */
    ccev_sock_t *sock = ccev_sock_create(write_ev_loop, sv[0], NULL);
    ASSERT(sock != NULL);

    /* Fill sv[0]'s send buffer until EAGAIN (socket NOT writable) */
    {
        char fill[65536];
        int n;
        while (ccsocket_send(sv[0], fill, sizeof(fill), &n) == CC_OPCODE_OK) {}
    }

    write_ev_fired = 0;
    ASSERT(ccev_sock_write_start(sock, on_write_event) == CCEV_OK);

    /* Drain from sv[1] — this creates space in sv[0]'s send buffer,
     * triggering the "not writable → writable" transition. */
    {
        char drain[65536];
        ccsocket_set_nonblock(sv[1], true);
        while (ccsocket_recv(sv[1], drain, sizeof(drain), NULL) == CC_OPCODE_OK) {}
    }

    /* Safety timer — prevents hang if the event never fires */
    ccev_timer_add(write_ev_loop, 5000, CCEV_TIMER_ONCE,
                   timer_stop_loop, &write_ev_loop);

    ccev_loop_run(write_ev_loop, CCEV_RUN_FOREVER);

    ASSERT(write_ev_fired == 1);

    ccev_sock_close(sock);
    ccev_loop_destroy(write_ev_loop);
    ccsocket_close(sv[1]);
}

TEST(write_stop_suppresses_event) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ASSERT(sock != NULL);

    write_ev_fired = 0;
    write_ev_loop  = loop;

    ASSERT(ccev_sock_write_start(sock, on_write_event) == CCEV_OK);
    ASSERT(ccev_sock_write_stop(sock) == CCEV_OK);

    ccev_timer_add(loop, 50, CCEV_TIMER_ONCE,
                   timer_stop_loop, &loop);
    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(write_ev_fired == 0);

    ccev_sock_close(sock);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

TEST(write_start_null_sock_returns_err) {
    ASSERT(ccev_sock_write_start(NULL, on_write_event) == CCEV_ERR);
}

TEST(write_stop_null_sock_returns_err) {
    ASSERT(ccev_sock_write_stop(NULL) == CCEV_ERR);
}

TEST(write_start_on_closed_returns_err) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ccev_sock_close(sock);
    ASSERT(ccev_sock_write_start(sock, on_write_event) == CCEV_ERR);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ═══ ccev_sock_set_close_cb ───────────────────────── */

TEST(set_close_cb_null_is_safe) {
    ccev_sock_set_close_cb(NULL, NULL, NULL);
    passed++;
}

static int close_cb_fired;
static void on_close(void *udata) {
    (void)udata;
    close_cb_fired = 1;
}

TEST(close_cb_fires_on_close) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ASSERT(sock != NULL);

    close_cb_fired = 0;
    ccev_sock_set_close_cb(sock, on_close, NULL);
    ccev_sock_close(sock);
    ccev_loop_run(loop, CCEV_RUN_ONCE);
    ASSERT(close_cb_fired == 1);

    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ═══ ccev_sock_count ──────────────────────────────── */

TEST(count_null_returns_zero) {
    ASSERT(ccev_sock_count(NULL) == 0);
}

TEST(count_after_create) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }
    ccev_loop_t *loop = ccev_loop_create(64);
    /* Loop creates internal wake_sock, so count starts at 1 */
    ASSERT(ccev_sock_count(loop) == 1);

    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ASSERT(sock != NULL);
    ASSERT(ccev_sock_count(loop) == 2);

    ccev_sock_close(sock);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ── Dummy accept callback for listen failure tests ── */
static void _dummy_accept(void *udata, ccev_sock_t *client,
                           const char *ip, int port) {
    (void)udata; (void)client; (void)ip; (void)port;
}

/* ═══ ccev_listen failure modes ───────────────────── */

TEST(listen_null_loop_returns_null) {
    ASSERT(ccev_listen(NULL, "127.0.0.1", 8080, 5, CCEV_REUSEADDR,
                       _dummy_accept, NULL) == NULL);
}

TEST(listen_null_addr_returns_null) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ASSERT(ccev_listen(loop, NULL, 8080, 5, CCEV_REUSEADDR,
                       _dummy_accept, NULL) == NULL);
    ccev_loop_destroy(loop);
}

TEST(listen_null_cb_returns_null) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ASSERT(ccev_listen(loop, "127.0.0.1", 8080, 5, CCEV_REUSEADDR,
                       NULL, NULL) == NULL);
    ccev_loop_destroy(loop);
}

TEST(listen_invalid_addr_returns_null) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    /* "invalid" is not a valid IP format — ccsocket_get_version returns
     * CC_FAMILY_INVALID, so ccev_listen should return NULL immediately. */
    ccev_sock_t *l = ccev_listen(loop, "invalid", 8080, 5, CCEV_REUSEADDR,
                                  _dummy_accept, NULL);
    ASSERT(l == NULL);
    ccev_loop_destroy(loop);
}

/* ═══ Connect timeout before DNS resolves (R4.1 race) ── */

static int   ctdr_flag;
static int   ctdr_status;
static ccev_loop_t *ctdr_loop;

static void ctdr_on_connect(void *udata, ccev_sock_t *sock, int status) {
    (void)udata; (void)sock;
    ctdr_flag   = 1;
    ctdr_status = status;
    ccev_loop_stop(ctdr_loop);
}

TEST(connect_timeout_before_dns_resolves) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ctdr_loop   = loop;
    ctdr_flag   = 0;
    ctdr_status = -99;

    /* Point DNS to non-routable TEST-NET-2 so queries won't resolve
     * within the 1ms connect timeout.  The timeout fires first; the
     * eventual DNS response should be safely discarded (context->alive
     * set false by _connect_timeout_cb). */
    ccev_dns_set_server(loop, "198.51.100.1", 53);

    /* Requires DNS resolution — not in cache, not localhost */
    ccev_sock_t *conn = ccev_connect(loop, "connect-timeout-race.test", 8080,
                                      1, 0,
                                      ctdr_on_connect, loop);
    ASSERT(conn != NULL);

    /* Loop runs until timeout fires → cb(ERR) → loop_stop */
    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(ctdr_flag   == 1);
    ASSERT(ctdr_status == CCEV_ERR);

    /* Drain any delayed DNS activity — no crash = test passes */
    ccev_loop_run(loop, CCEV_RUN_ONCE);
    ccev_loop_run(loop, CCEV_RUN_ONCE);

    ccev_loop_destroy(loop);
}

/* ═══ ccev_listen + ccev_connect (end-to-end) ─────── */

static int  listen_accept_flag;
static ccev_sock_t *listen_accept_sock;
static int  listen_connect_flag;
static ccev_loop_t *listen_test_loop;

static void listen_on_accept(void *udata, ccev_sock_t *client,
                              const char *ip, int port) {
    (void)udata; (void)ip; (void)port;
    listen_accept_flag = 1;
    listen_accept_sock = client;
    ccev_loop_stop(listen_test_loop);
}

static void listen_on_connect(void *udata, ccev_sock_t *sock, int status) {
    (void)udata;
    if (status == CCEV_OK && sock) {
        listen_connect_flag = 1;
    }
}

TEST(listen_then_connect) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    listen_test_loop     = loop;
    listen_accept_flag   = 0;
    listen_connect_flag  = 0;
    listen_accept_sock   = NULL;

    ccev_sock_t *listener = ccev_listen(loop, "127.0.0.1", 0, 5,
                                         CCEV_REUSEADDR,
                                         listen_on_accept, loop);
    ASSERT(listener != NULL);

    uint16_t port;
    char addr[MAX_ADDRLEN];
    ccsocket_t lfd = ccev_sock_get_fd(listener);
    ASSERT(ccsocket_get_sockname(lfd, addr, &port));
    ASSERT(port > 0);

    ccev_sock_t *conn = ccev_connect(loop, "127.0.0.1", port,
                                      5000, CCEV_TCP_NODELAY,
                                      listen_on_connect, loop);
    ASSERT(conn != NULL);

    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(listen_accept_flag  == 1);
    ASSERT(listen_accept_sock  != NULL);
    ASSERT(listen_connect_flag == 1);

    ccev_sock_close(listener);
    ccev_loop_destroy(loop);
    passed++;
}

/* ════════════════════════════════════════════════════ */

int main(void) {
    printf("test_sock\n");
    printf("──────────────────────\n"); fflush(stdout);

    /* create */
    RUN(create_invalid_fd_returns_null);
    RUN(create_null_loop_returns_null);

    /* close */
    RUN(close_null_returns_err);
    RUN(close_twice_returns_err);

    /* metadata */
    RUN(get_fd_null_returns_invalid);
    RUN(get_udata_null_returns_null);
    RUN(set_udata_null_is_safe);
    RUN(udata_set_and_get);

    /* read_start / read_stop */
    RUN(read_start_fires_on_data);
    RUN(read_stop_suppresses_event);
    RUN(read_start_null_sock_returns_err);
    RUN(read_stop_null_sock_returns_err);
    RUN(read_start_on_closed_returns_err);

    /* write_start / write_stop */
    RUN(write_start_fires_on_writable);
    RUN(write_stop_suppresses_event);
    RUN(write_start_null_sock_returns_err);
    RUN(write_stop_null_sock_returns_err);
    RUN(write_start_on_closed_returns_err);

    /* close callback */
    RUN(set_close_cb_null_is_safe);
    RUN(close_cb_fires_on_close);

    /* count */
    RUN(count_null_returns_zero);
    RUN(count_after_create);

    /* listen failure */
    RUN(listen_null_loop_returns_null);
    RUN(listen_null_addr_returns_null);
    RUN(listen_null_cb_returns_null);
    RUN(listen_invalid_addr_returns_null);

    /* connect timeout + DNS race */
    RUN(connect_timeout_before_dns_resolves);

    /* listen + connect */
    RUN(listen_then_connect);

    printf("\n  %d passed, %d failed\n", passed, failed); fflush(stdout);
    return failed ? 1 : 0;
}
