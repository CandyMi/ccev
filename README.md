# ccev

[![CI](https://img.shields.io/github/actions/workflow/status/CandyMi/ccev/ci.yml?branch=master&label=CI&logo=github&style=flat-square)](https://github.com/CandyMi/ccev/actions)
[![License](https://img.shields.io/github/license/CandyMi/ccev?label=License&style=flat-square)](LICENSE)
[![C](https://img.shields.io/badge/C-99-%23555555?logo=c&style=flat-square)](https://en.wikipedia.org/wiki/C99)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows-%2344cc11?style=flat-square)]()

A lightweight, cross-platform reactor event-driven library written in C99.

## Build

```bash
git clone --recurse-submodules https://github.com/CandyMi/ccev.git
cd ccev
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Reactor loop lifecycle

```c
ccev_loop_run(loop, mode)
 |
 +-- 1. Process expired timers (ccheap_pop --> callbacks)
 |       ONCE --> free, REPEAT --> re-insert
 |
 +-- 2. Check stop_flag (from ccev_loop_stop)
 |
 +-- 3. Compute epoll_wait timeout
 |       nearest timer --> remaining ms
 |       no timer      --> -1 (infinite)
 |
 +-- 4. epoll_wait(epfd, events, max_events, timeout)
 |
 +-- 5. Dispatch n fired events (each re-armed after callback):
 |       [wake pipe]    drain + re-arm EPOLLIN
 |       [EPOLLERR/HUP] schedule close
 |       [listener]     accept2 --> conn_create --> on_accept_cb
 |       [connecting]   getsockopt SO_ERROR --> on_connect_cb + re-arm
 |       [EPOLLIN]      recv_cb(udata) + re-arm
 |       [EPOLLOUT]     flush write buffer (re-arms internally)
 |
 +-- 6. Process closing queue (close_cb --> free conn)
 |
 +-- 7. stop_flag set? --> return
 |       mode ONCE  --> return
 |       FOREVER    --> goto 1
```

## Quick start

```c
#include "ccev.h"
#include <stdio.h>
#include <string.h>

static void on_recv(void *udata) {
    ccev_conn_t *conn = (ccev_conn_t *)udata;
    char buf[4096];
    int n;
    for (;;) {
        n = ccev_conn_recv(conn, buf, sizeof(buf), NULL, NULL);
        if (n > 0) ccev_conn_send(conn, buf, (size_t)n, NULL, NULL);
        else return;
    }
}
static void on_accept(void *udata, ccev_conn_t *conn,
                       const char *ip, int port) {
    ccev_conn_recv(conn, NULL, 0, on_recv, conn);
}

int main(void) {
    ccev_loop_t *loop = ccev_loop_create(1024);
    ccev_listen(loop, "0.0.0.0", 8080, 128, CCEV_REUSEADDR, on_accept, NULL);
    ccev_loop_run(loop, CCEV_RUN_FOREVER);
    ccev_loop_destroy(loop);
    return 0;
}
```

See [docs/](docs/) for full documentation.

## License

MIT
