/**
 * @file ccev_signal.c
 * @brief Signal handling via self-pipe trick.
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
    ccsocket_send(loop->signal_pipe[1], &byte, 1, NULL);
}

/* ── Dispatch callback (fired from loop on pipe readable) ───── */

static void ccev__signal_dispatch(void *udata) {
    (void)udata;
    ccev_loop_t *loop = ccev_default_loop();
    if (!loop) return;

    unsigned char byte;
    while (ccsocket_recv(loop->signal_pipe[0], (char*)&byte, 1, NULL)
           == CC_OPCODE_OK) {
        int signum = (int)byte;
        if (signum >= 1 && signum <= 63 && loop->signals[signum].cb)
            loop->signals[signum].cb(loop->signals[signum].udata, signum);
    }
    if (loop->signal_conn && !loop->signal_conn->closed)
        ccev__conn_mod_internal(loop, loop->signal_conn, EPOLLIN);
}

/* ── Public API ─────────────────────────────────────────────── */

int ccev_signal_handle(int signum, ccev_signal_cb cb, void *udata) {
    if (signum < 1 || signum > 63 || !cb) return CCEV_ERR;

    ccev_loop_t *loop = ccev_default_loop();
    if (!loop) return CCEV_ERR;

    /* Store callback */
    loop->signals[signum].cb    = cb;
    loop->signals[signum].udata = udata;

    /* Wire up the signal dispatch callback on first use */
    if (loop->signal_conn)
        loop->signal_conn->recv_cb = ccev__signal_dispatch;

    /* Ensure signal dispatch conn is registered */
    if (loop->signal_conn && !loop->signal_conn->closed)
        ccev__conn_mod_internal(loop, loop->signal_conn, EPOLLIN);

    /* Install OS signal handler */
    signal(signum, ccev__sig_handler);

    return CCEV_OK;
}

int ccev_signal_ignore(int signum) {
    if (signum < 1 || signum > 63) return CCEV_ERR;
    ccev_loop_t *loop = ccev_default_loop();
    if (!loop) return CCEV_ERR;

    loop->signals[signum].cb    = NULL;
    loop->signals[signum].udata = NULL;
    signal(signum, SIG_DFL);
    return CCEV_OK;
}
