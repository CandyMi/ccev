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
            /* EAGAIN (1) or error (-1): wait for next epoll notification */
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

int main(int argc, char **argv) {
    const char *host = argc > 2 ? argv[2] : "0.0.0.0";
    const char *port = argc > 1 ? argv[1] : "8080";

    ccev_loop_t *loop = ccev_loop_create(1024);
    if (!loop) return 1;

    ccev_conn_t *l = ccev_listen(loop, "0.0.0.0", 8080, 128,
                                   CCEV_REUSEADDR, on_accept, NULL);
    if (!l) { ccev_loop_destroy(loop); return 1; }

    printf("echo server on %s:%s\n", host, port);
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

## Project structure

```
ccev/
├── CMakeLists.txt          # Root build system
├── src/
│   ├── ccev.h              # Public API header (with Doxygen comments)
│   ├── ccev_internal.h     # Internal data structures
│   ├── ccev.c              # Reactor core (loop, listen, connect)
│   ├── ccev_timer.c        # Timer subsystem (ccheap)
│   ├── ccev_conn.c         # Connection I/O, buffering, sendfile
│   ├── ccev_dns.c          # Asynchronous DNS resolver
│   └── ccev_icmp.c         # ICMP echo (ping)
├── deps/                   # Git submodules
│   ├── epoll/              # github.com/CandyMi/epoll
│   ├── ccalg/              # github.com/CandyMi/ccalg
│   └── ccsocket/           # github.com/CandyMi/ccsocket
├── scripts/
│   ├── init-deps.sh          # Submodule re-init (POSIX shell)
│   ├── init-deps.cmd         # Submodule re-init (Windows CMD)
│   └── init-deps.ps1         # Submodule re-init (PowerShell)
├── tests/
│   ├── CMakeLists.txt
│   ├── test_timer.c
│   ├── test_conn.c
│   └── test_dns.c
├── examples/
│   ├── echo_server.c
│   ├── timer_demo.c
│   └── http_bench.c         # HTTP benchmark server (wrk/ab)
└── docs/
    ├── api-reference.md
    └── getting-started.md
```

## Key Design Decisions

- **Single-threaded reactor**: All I/O, timers, and DNS resolution share one event loop. No locks in the hot path.
- **EPOLLONESHOT by default**: Every event fires at most once per registration. Unfired events are automatically re-armed by the library — the user never needs to call epoll_ctl().
- **Asynchronous send / synchronous recv**: `ccev_conn_send()` buffers data and manages EPOLLOUT internally. `ccev_conn_recv()` is a synchronous read (call it from within a recv callback).
- **Speculative reads**: `ccev_conn_recv()` with a callback performs a non-blocking read first. If data is available immediately, the callback fires synchronously — avoiding a redundant epoll registration.
- **D-ary heap timers**: 4-ary min-heap (ccheap) with O(log n) insert/pop and O(log n) update via embedded index.
- **Race-mode DNS**: Queries are sent to all configured servers simultaneously via unconnected UDP sockets. The first valid response wins.
- **Deferred close**: Connections are moved to a closing list during dispatch; actual teardown happens after all callbacks return.
- **sendfile**: `ccev_conn_sendfile()` delegates to kernel sendfile (zero-copy on Linux, macOS, FreeBSD).
- **ICMP echo**: `ccev_icmp_echo()` integrates ccicmp with the reactor for privilege-free ping on modern kernels.
