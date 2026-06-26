# Getting Started with ccev

ccev is a lightweight, cross-platform reactor event-driven library written in C99. It provides a unified API for event-driven networking on Linux (epoll), macOS/BSD (kqueue), Windows (IOCP), and other POSIX systems (select).

## Building

### Prerequisites

- C compiler (GCC, Clang, MSVC, or MinGW) — C99 support required
- CMake ≥ 3.0
- git (for submodule initialization)

### Build commands

```bash
# First-time setup (after clone)
git submodule update --init --recursive
```

Then:

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build everything (library + tests + examples)
cmake --build build

# Run tests
ctest --test-dir build --output-on-failure

# Build and run a specific example
cmake --build build --target echo_server
./build/examples/echo_server 8080
```

## Quick Start: Signal Handler

```c
#include "ccev.h"
#include <signal.h>
#include <stdio.h>

static void on_signal(void *udata, int signum) {
    printf("signal %d received, shutting down\n", signum);
    ccev_loop_stop((ccev_loop_t *)udata);
}

int main(void) {
    ccev_loop_t *loop = ccev_default_loop();
    ccev_signal_handle(SIGINT,  on_signal, loop);
    ccev_signal_handle(SIGTERM, on_signal, loop);
    ccev_loop_run(loop, CCEV_RUN_FOREVER);
    ccev_loop_destroy(loop);
    return 0;
}
```

## Quick Start: Echo Server

```c
#include "ccev.h"
#include <stdio.h>
#include <string.h>

static void on_sent(void *udata) { (void)udata; }

static void on_recv(void *udata) {
    ccev_conn_t *conn = (ccev_conn_t *)udata;
    char buf[4096];
    int n;
    for (;;) {
        n = ccev_conn_recv(conn, buf, sizeof(buf), NULL, NULL);
        if (n > 0) {
            ccev_conn_send(conn, buf, (size_t)n, on_sent, NULL);
        } else if (n == 0) {
            ccev_conn_close(conn);
            return;
        } else {
            /* EAGAIN or error: wait for next epoll notification */
            return;
        }
    }
}

static void on_close(void *udata) {
    ccev_conn_t *conn = (ccev_conn_t *)udata;
    ccev_conn_close(conn);
}

static void on_accept(void *udata, ccev_conn_t *conn,
                       const char *ip, int port) {
    printf("accept: %s:%d\n", ip, port);
    ccev_conn_set_close_cb(conn, on_close, conn);
    ccev_conn_recv(conn, NULL, 0, on_recv, conn);
}

int main(void) {
    ccev_loop_t *loop = ccev_loop_create(1024);
    if (!loop) return 1;

    ccev_conn_t *l = ccev_listen(loop, "0.0.0.0", 8080, 128,
                                   CCEV_REUSEADDR, on_accept, NULL);
    if (!l) { ccev_loop_destroy(loop); return 1; }

    printf("echo server on 0.0.0.0:8080\n");
    ccev_loop_run(loop, CCEV_RUN_FOREVER);
    ccev_loop_destroy(loop);
    return 0;
}
```

### How it works

1. `ccev_loop_create()` creates the event loop.
2. `ccev_listen()` creates a TCP listener and registers it with the loop.
3. On every incoming connection, `on_accept` is called with a new `ccev_conn_t*`.
4. The user sets up callbacks and arms read events via `ccev_conn_recv()`.
5. `ccev_loop_run(CCEV_RUN_FOREVER)` drives the event loop.
6. All I/O uses **EPOLLONESHOT** internally — after each callback the library re-arms the connection for its next event.

## Quick Start: Timer Demo

```c
#include "ccev.h"
#include <stdio.h>

static void on_timer(void *udata) {
    printf("timer fired!\n");
    ccev_loop_stop((ccev_loop_t *)udata);
}

int main(void) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_timer_add(loop, 1000, CCEV_TIMER_ONCE, on_timer, loop);
    ccev_loop_run(loop, CCEV_RUN_FOREVER);
    ccev_loop_destroy(loop);
    return 0;
}
```


