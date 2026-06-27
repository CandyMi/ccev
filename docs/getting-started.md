# Getting Started with ccev

ccev is a lightweight, cross-platform reactor event-driven library written in C99. It provides a two-level API for event-driven networking on Linux (epoll), macOS/BSD (kqueue), Windows (IOCP), and other POSIX systems (select).

## Building

### Prerequisites

- C compiler (GCC, Clang, MSVC, or MinGW) — C99 support required
- CMake ≥ 3.10
- git (for submodule initialization)

### Build commands

```bash
# First-time setup (after clone)
git submodule update --init --recursive

# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build everything (library + tests)
cmake --build build

# Run tests
ctest --test-dir build --output-on-failure
```

## Architecture

```
ccev_sock_t  — low-level: fd + event callbacks (rcb/wcb/close_cb)
    │
    ├── ccev_sock_create(loop, fd, udata)     external fd
    ├── ccev_listen(loop, addr, port, ...)     TCP/UDS listener
    └── ccev_connect(loop, host, port, ...)    async connect (+ DNS)
         │
         └── ccev_stream_open(sock)  ──→  ccev_stream_t
                                           buffered write + stream reader
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

## Quick Start: Echo Server (low-level sock API)

```c
#include "ccev.h"
#include <stdio.h>
#include <string.h>
#include "ccsocket.h"

static void on_sock_read(ccev_sock_t *sock, int events) {
    (void)events;
    char buf[4096];
    int n;
    ccsocket_stcode_t rc = ccsocket_recv(ccev_sock_get_fd(sock),
                                          buf, sizeof(buf), &n);
    if (rc == CC_OPCODE_OK && n > 0) {
        ccsocket_send(ccev_sock_get_fd(sock), buf, n, NULL);
    } else if (n == 0) {
        ccev_sock_close(sock);
    }
}

static void on_accept(void *udata, ccev_sock_t *client,
                       const char *ip, int port) {
    printf("accept: %s:%d\n", ip, port);
    ccev_sock_read_start(client, on_sock_read);
}

int main(void) {
    ccev_loop_t *loop = ccev_loop_create(1024);
    if (!loop) return 1;

    ccev_sock_t *l = ccev_listen(loop, "0.0.0.0", 8080, 128,
                                   CCEV_REUSEADDR, on_accept, NULL);
    if (!l) { ccev_loop_destroy(loop); return 1; }

    printf("echo server on 0.0.0.0:8080\n");
    ccev_loop_run(loop, CCEV_RUN_FOREVER);
    ccev_loop_destroy(loop);
    return 0;
}
```

## Quick Start: Echo Server (stream API)

```c
#include "ccev.h"
#include <stdio.h>
#include <string.h>

static void on_sent(void *udata) { (void)udata; }

static void on_readable(ccev_sock_t *sock, int events) {
    (void)events;
    char buf[4096];
    int n;
    ccsocket_stcode_t rc = ccsocket_recv(ccev_sock_get_fd(sock),
                                          buf, sizeof(buf), &n);
    if (rc == CC_OPCODE_OK && n > 0) {
        ccev_stream_t *st = (ccev_stream_t *)ccev_sock_get_udata(sock);
        ccev_stream_write(st, buf, (size_t)n, on_sent, NULL);
    } else {
        ccev_stream_close((ccev_stream_t *)ccev_sock_get_udata(sock));
    }
}

static void on_accept(void *udata, ccev_sock_t *client,
                       const char *ip, int port) {
    printf("accept: %s:%d\n", ip, port);
    ccev_stream_t *st = ccev_stream_open(client);
    ccev_sock_set_udata(client, st);
    ccev_sock_read_start(client, on_readable);
}

int main(void) {
    ccev_loop_t *loop = ccev_loop_create(1024);
    if (!loop) return 1;

    ccev_sock_t *l = ccev_listen(loop, "0.0.0.0", 8080, 128,
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
2. `ccev_listen()` creates a TCP listener (returns `ccev_sock_t*`).
3. On every incoming connection, `on_accept` is called with a new `ccev_sock_t*`.
4. The user may use the sock directly (`ccev_sock_read_start`) or upgrade to a stream (`ccev_stream_open`) for buffered I/O.
5. `ccev_loop_run(CCEV_RUN_FOREVER)` drives the event loop.
6. All I/O uses **EPOLLONESHOT** internally — after each callback the library re-arms automatically.

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

## Quick Start: Async Connect with DNS

```c
#include "ccev.h"
#include <stdio.h>

static void on_connect(void *udata, ccev_sock_t *sock, int status) {
    if (status == CCEV_OK) {
        printf("connected!\n");
        ccev_stream_t *st = ccev_stream_open(sock);
        ccev_stream_write(st, "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n",
                          43, NULL, NULL);
        ccev_stream_readline(st, '\n', 4096, 5000,
                             (ccev_stream_cb)(void(*)(void))printf, NULL);
    } else {
        printf("connection failed\n");
    }
    ccev_loop_stop((ccev_loop_t *)udata);
}

int main(void) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_connect(loop, "example.com", 80, 10000, CCEV_TCP_NODELAY,
                 on_connect, loop);
    ccev_loop_run(loop, CCEV_RUN_FOREVER);
    ccev_loop_destroy(loop);
    return 0;
}
```
