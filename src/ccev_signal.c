/**
 * @file ccev_signal.c
 * @brief Signal handling via self-pipe trick — POSIX sigaction /
 *        Windows console handler, integrated with the default loop.
 *
 * The signal handler writes the signal number as a single byte to a
 * self-pipe.  The pipe's read end is registered in the default loop's
 * epoll set.  When ccev_loop_run() drains the pipe, it decodes the
 * byte, looks up the callback in loop->signals[signum], and fires it.
 *
 * Only one handler per signum — last registration wins.  Only the
 * default loop (ccev_default_loop()) may register signal handlers.
 */

#include "ccev_internal.h"

#include <signal.h>

/* ── Signal-number translation ──────────────────────────────── */

static int ccev__to_os_signum(int ccev_sig) {
    switch (ccev_sig) {
#ifdef SIGHUP
    case CCEV_SIGHUP:  return SIGHUP;
#endif
#ifdef SIGINT
    case CCEV_SIGINT:  return SIGINT;
#endif
#ifdef SIGQUIT
    case CCEV_SIGQUIT: return SIGQUIT;
#endif
#ifdef SIGTERM
    case CCEV_SIGTERM: return SIGTERM;
#endif
#ifdef SIGUSR1
    case CCEV_SIGUSR1: return SIGUSR1;
#endif
#ifdef SIGUSR2
    case CCEV_SIGUSR2: return SIGUSR2;
#endif
#ifdef SIGPIPE
    case CCEV_SIGPIPE: return SIGPIPE;
#endif
#ifdef SIGALRM
    case CCEV_SIGALRM: return SIGALRM;
#endif
    default:           return -1;
    }
}

/* ── Signal handler (async-signal-safe) ─────────────────────── */

static void ccev__sig_handler(int signum) {
    unsigned char byte = (unsigned char)signum;
    /* write() is async-signal-safe per POSIX */
    int r = (int)write(ccev__g_default_loop->signal_pipe[1], &byte, 1);
    (void)r;  /* best-effort */
}

/* ── Dispatch callback (fired from loop on pipe readable) ───── */

static void ccev__signal_dispatch(void *udata) {
    ccev_loop_t *loop = ccev__g_default_loop;
    if (!loop) return;

    unsigned char byte;
    while (ccsocket_recv(loop->signal_pipe[0], (char*)&byte, 1, NULL)
           == CC_OPCODE_OK) {
        int signum = (int)byte;
        if (signum >= 0 && signum < 64 && loop->signals[signum].cb)
            loop->signals[signum].cb(loop->signals[signum].udata, signum);
    }
    /* Re-arm the pipe for the next signal */
    if (loop->signal_conn && !loop->signal_conn->closed)
        ccev__conn_mod_internal(loop, loop->signal_conn, EPOLLIN);
}

/* ── Public API ─────────────────────────────────────────────── */

int ccev_signal_handle(int signum, ccev_signal_cb cb, void *udata) {
    if (!ccev__g_default_loop) return CCEV_ERR;
    if (signum < 1 || signum > 63) return CCEV_ERR;
    if (!cb) return CCEV_ERR;

    int os_sig = ccev__to_os_signum(signum);
    if (os_sig < 0) return CCEV_ERR;  /* unsupported on this platform */

    /* Store callback */
    ccev__g_default_loop->signals[signum].cb    = cb;
    ccev__g_default_loop->signals[signum].udata = udata;

    /* Wire up the signal dispatch callback on first use */
    if (ccev__g_default_loop->signal_conn)
        ccev__g_default_loop->signal_conn->recv_cb = ccev__signal_dispatch;

    /* Ensure signal dispatch conn is registered */
    if (ccev__g_default_loop->signal_conn && !ccev__g_default_loop->signal_conn->closed)
        ccev__conn_mod_internal(ccev__g_default_loop, ccev__g_default_loop->signal_conn, EPOLLIN);

    /* Install OS signal handler */
#ifdef _WIN32
    /* Windows: only SIGINT and SIGTERM via signal() */
    signal(os_sig, ccev__sig_handler);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = ccev__sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NODEFER;
    sigaction(os_sig, &sa, NULL);
#endif

    return CCEV_OK;
}

int ccev_signal_ignore(int signum) {
    if (!ccev__g_default_loop) return CCEV_ERR;
    if (signum < 1 || signum > 63) return CCEV_ERR;

    int os_sig = ccev__to_os_signum(signum);
    if (os_sig < 0) return CCEV_ERR;

    /* Clear callback */
    ccev__g_default_loop->signals[signum].cb    = NULL;
    ccev__g_default_loop->signals[signum].udata = NULL;

    /* Restore default disposition */
    signal(os_sig, SIG_DFL);
    return CCEV_OK;
}
