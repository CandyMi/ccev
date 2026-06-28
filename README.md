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

## Two-level API

ccev provides two levels of socket abstraction:

- **`ccev_sock_t`** — raw fd + event callbacks (reactor primitive).  
  Create via `ccev_sock_create()`, `ccev_listen()`, or `ccev_connect()`.
  Register interest with `ccev_sock_read_start()` / `ccev_sock_write_start()`.

- **`ccev_stream_t`** — buffered I/O + stream reader (protocol primitive).  
  Upgrade a sock via `ccev_stream_open()`.  
  Provides `ccev_stream_write()`, `ccev_stream_readline()`, `ccev_stream_readnum()`, `ccev_stream_sendfile()`.

## Reactor loop lifecycle

```
ccev_loop_run(loop, mode)
 |
 +-- 1. now = ccev__now_ms()
 |
 +-- 2. ccev__timer_process(loop, now):
 |       Phase 1: extract all expired from heap into cclink
 |       Phase 2: fire callbacks (ONCE free, REPEAT re-insert)
 |       Phase 3: return ms until next future timer (-1 if none)
 |
 +-- 3. Check stop_flag (from ccev_loop_stop)
 |
 +-- 4. Compute epoll_wait timeout
 |       next_ms < 0  --> -1 (infinite, wait for I/O)
 |       next_ms >= 0 --> next_ms (block until next timer)
 |
 +-- 5. epoll_wait(epfd, events, max_events, timeout)
 |
 +-- 6. Dispatch n fired events:
 |       [wake pipe]    drain only (re-arm at step 7)
 |       [HUP/ERR]      ccev__sock_schedule_close
 |       [listener]     accept2 --> sock_create --> listen_cb + re-arm
 |       [connecting]   getsockopt SO_ERROR --> connect_cb + re-arm
 |       [EPOLLIN]      rcb(sock, events) + re-arm
 |       [EPOLLOUT]     wcb(sock, events) + re-arm
 |
 +-- 7. Re-arm wake_sock
 |
 +-- 8. Process closing queue (close_cb --> sock_free)
 |
 +-- 9. stop_flag set? --> return
 |       mode ONCE  --> return
 |       FOREVER    --> goto 1
```

## Quick start — echo server

```c
#include "ccev.h"
#include <stdio.h>
#include <string.h>

/* Forward declarations */
static void on_readable(ccev_sock_t *sock, int events);

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
    } else if (rc == CC_OPCODE_ERROR && n == 0) {
        ccev_sock_close(sock);
    }
}

static void on_accept(void *udata, ccev_sock_t *client,
                       const char *ip, int port) {
    printf("accept: %s:%d\n", ip, port);
    /* Upgrade to stream for buffered write */
    ccev_stream_t *st = ccev_stream_open(client);
    ccev_sock_set_udata(client, st);
    ccev_sock_read_start(client, on_readable);
}

int main(void) {
    ccev_loop_t *loop = ccev_loop_create(1024);
    if (!loop) return 1;

    ccev_sock_t *listener = ccev_listen(loop, "0.0.0.0", 8080, 128,
                                         CCEV_REUSEADDR, on_accept, NULL);
    if (!listener) { ccev_loop_destroy(loop); return 1; }

    printf("echo server on 0.0.0.0:8080\n");
    ccev_loop_run(loop, CCEV_RUN_FOREVER);
    ccev_loop_destroy(loop);
    return 0;
}
```

See [docs/](docs/) for full documentation.

## License

MIT
