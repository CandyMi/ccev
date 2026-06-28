/**
 * @file ccev_signal.c
 * @brief Signal handling via self-pipe trick.
 *
 * @author CandyMi
 * @license MIT
 *
 * Integrates with the default loop (ccev_default_loop()).  The signal
 * handler writes the signal number as a single byte to a self-pipe.
 * The dispatch callback reads the byte and fires the registered handler.
 *
 * Only one handler per signum — last registration wins.  Only the
 * default loop may register signal handlers.
 */

#include "ccev_internal.h"
#include <signal.h>

/* ── Signal handler (async-signal-safe) ─────────────────────── */

static void ccev__sig_handler(int signum) {
    ccev_loop_t *loop = ccev_default_loop();
    if (!loop) return;
    unsigned char byte = (unsigned char)signum;
#if defined(_WIN32)
    if (loop->signal_pipe[1] != (ccsocket_t)-1)
        (void)ccsocket_send(loop->signal_pipe[1], (char*)&byte, 1, NULL);
#else
    /* Pipe is non-blocking; EAGAIN means the kernel buffer is full
     * (~64 KiB / 65536 pending signals) — the signal byte is lost
     * but no recovery is possible from a signal handler. */
    if (loop->signal_pipe[1] > 0)
        (void)write(loop->signal_pipe[1], (char*)&byte, 1);
#endif
}

/* ── Dispatch callback (fired from loop on pipe readable) ───── */

void ccev__signal_dispatch(ccev_sock_t *sock, int events) {
    (void)sock; (void)events;
    ccev_loop_t *loop = ccev_default_loop();
    if (!loop) return;

    unsigned char byte;
    while (ccsocket_recv(loop->signal_pipe[0], (char*)&byte, 1, NULL)
           == CC_OPCODE_OK) {
        int signum = (int)byte;
        if (signum >= 1 && signum <= 63 && loop->signals[signum].cb)
            loop->signals[signum].cb(loop->signals[signum].udata, signum);
    }
    /* Re-arm is handled by the event loop after recv_cb returns */
}

/* ── Public API ─────────────────────────────────────────────── */

/* ── Platform-safe signal handler install (sigaction preferred) ── */

static int ccev__sig_install(int signum, void (*handler)(int)) {
#if defined(_WIN32)
    /* Windows only provides signal() */
    signal(signum, handler);
    return CCEV_OK;
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    return (sigaction(signum, &sa, NULL) == 0) ? CCEV_OK : CCEV_ERR;
#endif
}

int ccev_signal_handle(int signum, ccev_signal_cb cb, void *udata) {
    if (signum < 1 || signum > 63 || !cb) return CCEV_ERR;

    ccev_loop_t *loop = ccev_default_loop();
    if (!loop) return CCEV_ERR;

    /* Store callback */
    loop->signals[signum].cb    = cb;
    loop->signals[signum].udata = udata;

    /* Wire up the signal dispatch callback on first use */
    /* Wire up signal dispatch via sock_read_start */
    if (loop->signal_sock && !loop->signal_sock->closed)
        ccev_sock_read_start(loop->signal_sock, ccev__signal_dispatch);

    /* Install OS signal handler */
    return ccev__sig_install(signum, ccev__sig_handler);
}

int ccev_signal_ignore(int signum) {
    if (signum < 1 || signum > 63) return CCEV_ERR;
    ccev_loop_t *loop = ccev_default_loop();
    if (!loop) return CCEV_ERR;

    loop->signals[signum].cb    = NULL;
    loop->signals[signum].udata = NULL;
    return ccev__sig_install(signum, SIG_DFL);
}
