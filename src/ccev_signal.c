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

/* ── Signal / console handler ─────────────────────────────────
 *
 * POSIX:  async-signal-safe signal handler — write() to the
 *         self-pipe is listed in POSIX.1-2016 as safe.
 *
 * Win32:  console control handler via SetConsoleCtrlHandler.
 *         Runs in an OS-provided thread — send() on the
 *         TCP-loopback pipe is safe (not a signal context).
 *                                                              ── */

#if !defined(_WIN32)
typedef void (*ccev_signal_system_cb)(int);
static void cev__sig_handler(int signum) {
    ccev_loop_t *loop = ccev_default_loop();
    if (!loop) return;

    char byte = (char)signum;
    if (loop->signal_pipe[1] > 0)
        (void)write(loop->signal_pipe[1], &byte, 1);
}
#else
#include <windows.h>
typedef BOOL (WINAPI *ccev_signal_system_cb)(DWORD);
static BOOL WINAPI cev__sig_handler(DWORD dwCtrlType) {
    ccev_loop_t *loop = ccev_default_loop();
    if (!loop) return FALSE;

    int signum;
    switch (dwCtrlType) {
        case CTRL_C_EVENT:        signum = SIGINT;   break;
        case CTRL_BREAK_EVENT:    signum = SIGBREAK;  break;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT: signum = SIGTERM;   break;
        default: return FALSE;
    }

    char byte = (char)signum;
    if (loop->signal_pipe[1] > 0)
        (void)send(loop->signal_pipe[1], &byte, 1, 0);
    return TRUE;
}
#endif

/* ── Dispatch callback (fired from loop on pipe readable) ─────
 *
 * Reads signal bytes from the self-pipe and pushes each signum onto
 * loop->signal_queue.  Actual user callback dispatch is deferred to
 * ccev__signal_process_queue() at the end of the loop iteration,
 * providing a clean point for re-entrancy-safe delivery.
 *
 * On POSIX the bytes are written by cev__sig_handler (async-signal-
 * safe write).  On Windows they are written by the console control
 * handler (SetConsoleCtrlHandler) which runs in a separate thread
 * and can safely call send() on the TCP-loopback socket pair. ── */
void ccev__signal_dispatch(ccev_sock_t *sock, int events) {
    (void)sock; (void)events;
    ccev_loop_t *loop = ccev_default_loop();
    if (!loop) return;

    unsigned char byte;
    while (ccsocket_recv(loop->signal_pipe[0], (char*)&byte, 1, NULL)
           == CC_OPCODE_OK) {
        int signum = (int)byte;
        ccev_signal_event_t *ev = (ccev_signal_event_t *)
            ccev__realloc_fn(NULL, sizeof(ccev_signal_event_t));
        if (ev) { ev->signum = signum; cclink_push_back(&loop->signal_queue, &ev->node); }
    }
    /* Re-arm is handled by the event loop after recv_cb returns */
}

/* ── Drain pending signal queue (called from ccev_loop_run) ──
 *
 * Pops every ccev_signal_event_t from loop->signal_queue and fires
 * the corresponding registered callback.  The queue is filled by
 * ccev__signal_dispatch (pipe rcb in _dispatch_events) — the same
 * path on both platforms.                                ── */

void ccev__signal_process_queue(ccev_loop_t *loop) {
    if (!loop) return;
    while (!cclink_empty(&loop->signal_queue)) {
        cclink_node_t *n = cclink_pop_front(&loop->signal_queue);
        ccev_signal_event_t *ev = CCLINK_CONTAINER(n, ccev_signal_event_t, node);
        int signum = ev->signum;
        if (signum >= 1 && signum <= 63 && loop->signals[signum].cb)
            loop->signals[signum].cb(loop->signals[signum].udata, signum);
        ccev__free_fn(ev);
    }
}

/* ── Platform-safe signal handler install ──
 *
 * POSIX: sigaction with async-signal-safe write(pipe).
 * Windows: SetConsoleCtrlHandler for console events (SIGINT/SIGBREAK);
 * other signals are not supported via the pipe mechanism.     ── */

static int ccev__sig_install(int signum, ccev_signal_system_cb handler) {
#if defined(_WIN32)
    if (signum == SIGINT || signum == SIGBREAK) {
        if (handler == SIG_DFL) {
            /* Don't unregister the handler — other console signals
             * may still need it.  Harmless: events without a
             * registered callback are freed by process_queue. */
            return CCEV_OK;
        }
        return SetConsoleCtrlHandler(cev__sig_handler, TRUE)
               ? CCEV_OK : CCEV_ERR;
    }
    (void)handler;
    return CCEV_ERR;  /* Only SIGINT and SIGBREAK via pipe on Windows */
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

    /* Wire up the signal dispatch callback on first use */
    if (loop->signal_sock && !loop->signal_sock->closed)
        ccev_sock_read_start(loop->signal_sock, ccev__signal_dispatch);

    /* Install OS signal handler (must succeed before storing callback) */
    int ret = ccev__sig_install(signum, cev__sig_handler);
    if (ret != CCEV_OK) return ret;

    loop->signals[signum].cb    = cb;
    loop->signals[signum].udata = udata;
    return CCEV_OK;
}

int ccev_signal_ignore(int signum) {
    if (signum < 1 || signum > 63) return CCEV_ERR;
    ccev_loop_t *loop = ccev_default_loop();
    if (!loop) return CCEV_ERR;

    loop->signals[signum].cb    = NULL;
    loop->signals[signum].udata = NULL;
    return ccev__sig_install(signum, SIG_DFL);
}
