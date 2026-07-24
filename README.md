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
  Upgrade a sock via `ccev_stream_open()` (same address, backed by `ccev_sock_any_t`).  
  Provides `ccev_stream_write()`, `ccev_stream_read()`, `ccev_stream_sendfile()`.

```mermaid
flowchart LR
    subgraph Public["Public API"]
        L["ccev_listen() → ccev_sock_t*"]
        C["ccev_connect() → ccev_sock_t*"]
        S["ccev_sock_create(loop, fd, udata)\n→ ccev_sock_t*"]
    end
    subgraph Low["Low-level (ccev_sock_t)"]
        RS["sock_read_start / _stop"]
        WS["sock_write_start / _stop"]
        SC["sock_set_close_cb"]
        CL["sock_close"]
    end
    subgraph High["High-level (ccev_stream_t)"]
        O["ccev_stream_open(sock)\n→ stream"]
        W["stream_write / write_batch / flush"]
        SF["stream_sendfile"]
        RL["stream_read\n/ read_stop"]
    end
    Public --> Low
    Public --> High
    O -. "cast via ccev_sock_any_t\n(same address)" .-> Low
```

## Reactor loop lifecycle

```mermaid
flowchart TD
    subgraph Run["ccev_loop_run(loop, mode) — one iteration"]
        direction TB
        ENTER["entry"] --> Q0{"stop_flag?"}
        Q0 -->|"Yes"| EXIT0["return 0"]
        Q0 -->|"No"| WIN["Win32: ccev__signal_dispatch"]
        
        WIN --> T1["ccev__timer_process(now)\n→ next_ms (-1 if none)"]
        T1 --> Q1{"stop_flag?"}
        Q1 -->|"Yes"| EXIT
        Q1 -->|"No"| TO["Compute epoll timeout"]
        
        TO --> TOC{{"mode?"}}
        TOC -->|"RUN_ONCE"| TO0["timeout = 0"]
        TOC -->|"RUN_FOREVER\n+ no timers"| TOM1["timeout = -1\n(block indefinitely)"]
        TOC -->|"RUN_FOREVER\n+ next timer"| TOM2["timeout = next_ms"]
        
        TO0 --> EW["epoll_wait(EINTR retry)\n→ n events"]
        TOM1 --> EW
        TOM2 --> EW
        
        EW --> DISPATCH["For each of n events..."]
        
        DISPATCH --> EVT{"ev dispatch"}
        EVT -->|"null / closed"| SKIP["continue"]
        EVT -->|"wake_sock"| WAKE["drain pipe\ncontinue"]
        EVT -->|"EPOLLERR|HUP"| HUP["mode dispatch\nCONNECT→is_connected+cb\nINIT→rcb(HUP)+close\nLISTEN→close"]
        EVT -->|"LISTEN + IN"| LISTEN["batch accept (≤128)\nfor each: ccev_sock_create\n→ listen_cb\nre-arm EPOLLIN\ncontinue"]
        EVT -->|"CONNECT + OUT"| CONN["ccsocket_is_connected"]
        CONN -->|"CC_CONNECTED"| CONNOK["mode = INIT\ndel connect timer\nconnect_cb(OK)\ncontinue"]
        CONN -->|"CC_CONNERROR"| CONNERR["connect_cb(ERR)\nschedule_close\ncontinue"]
        EVT -->|"normal IN"| READ["fire rcb(sock, events)"]
        EVT -->|"normal OUT"| WRITE["fire wcb(sock, events)"]
        
        READ --> REARM["ccev__sock_rearm\n(if rcb/wcb present)"]
        WRITE --> REARM
        
        EW --> POST["Post-dispatch"]
        POST --> RWAKE["Re-arm wake_sock\nepoll_ctl MOD\n(EPOLLIN|ONESHOT)"]
        RWAKE --> CLOSING["ccev__process_closing()"]
        CLOSING --> Q2{"stop_flag?"}
        Q2 -->|"No"| Q3{"mode ==\nFOREVER?"}
        Q3 -->|"Yes"| WIN
    end
    
    Q2 -->|"Yes"| EXIT["Clear stop_flag\nreturn (ONCE?n:0)"]
    Q3 -->|"No"| EXIT
```

```mermaid
flowchart LR
    subgraph states["Socket state machine"]
        INIT["CCEV_SOCK_INIT\n(active data)"] -->|"ccev_sock_close"| CLOSED["sock->closed = true"]
        INIT -->|"ccev_listen"| LISTEN["CCEV_SOCK_LISTEN"]
        INIT -.->|"ccev_stream_open"| STREAM["(ccev_stream_t)"]
        LISTEN -->|"accept → client"| INIT
    end
    subgraph close["Deferred close flow"]
        CLOSED -->|"ccev__sock_schedule_close"| CLIST["move to closing list\nepoll_ctl DEL"]
        CLIST -->|"ccev__process_closing\n(next loop iteration)"| FIRE["fire close_cb"]
        FIRE --> FREE["ccev__sock_free\nclose(fd) + free"]
    end
```

### Lifecycle phases

1. **Phase 0** (Win32): Poll `sig_pending` flag.
2. **Phase 1 — Timers**: `ccev__timer_process()` pops expired timers from the 4-ary heap and fires callbacks before I/O dispatch.
3. **Phase 2 — Timeout**: 0 for RUN_ONCE, -1 (block) when no timers, or `next_ms` to meet the earliest timer.
4. **Phase 3 — Poll**: `epoll_wait()` with EINTR retry.
5. **Phase 4 — Dispatch**: Route each event by socket mode:
   - `wake_sock`: drain pipe, skip re-arm.
   - `HUP/ERR`: Mode-based dispatch — CONNECT→`ccsocket_is_connected`+`connect_cb`, INIT→`rcb(HUP)`+close, LISTEN→close.
   - `LISTEN`: batch accept (≤128), fire `listen_cb` per client, re-arm.
   - `CONNECT`: `ccsocket_is_connected()` — `CC_CONNECTED`→OK, `CC_CONNERROR`→ERR+close.
   - Normal I/O: fire `rcb`/`wcb`, then `ccev__sock_rearm()`.
6. **Phase 5**: Unconditionally re-arm `wake_sock` (`epoll_ctl MOD`, EPOLLIN|ONESHOT).
7. **Phase 6**: `ccev__process_closing()` — fire `close_cb` and free.
8. Check `stop_flag`: break if set, loop if `CCEV_RUN_FOREVER`.

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
