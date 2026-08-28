# Stress / Load Test Findings — pocket-dial (display build, ESP32-S3)

Measured against a live JC3248W535 display board running the Phase-2 firmware,
joined to a LAN in **STATION mode** (`192.168.12.159`), from a host on the same
subnet using `tests/load/sip_stress.py`. ESP-IDF v5.3.5, default optimization.

## TL;DR
- **Single requests are fast and correct.** SIP `OPTIONS` → `200 OK` in **6–81 ms**
  (first packet slower, warm path ~6 ms). The SIP engine itself is healthy.
- **Bursts from a single source are dropped**, by design + by buffer limits.
- **The display build degrades in STATION mode**: `lvgl_task` starves the Core-1
  idle task (task-WDT warnings), and the HTTP server accepts a TCP connection
  then resets it without responding. These are **pre-existing display-firmware
  behaviors**, not caused by Phase-2 code (lock ordering verified clean; the SIP
  engine + host build are unaffected).

## What works
| Probe | Result |
|-------|--------|
| ICMP ping (host → device) | 0% loss, 2–6 ms |
| SIP `OPTIONS` (idle) | `200 OK`, 6–81 ms |
| TCP connect :80 | succeeds |
| SIP engine logic (host build, CI) | 29/29 smoke + unit tests pass |

## Findings

### 1. UDP receive mailbox = 6 (burst ceiling)
Boot log reports `udp mbox: 6` (`CONFIG_LWIP_UDP_RECVMBOX_SIZE`). A burst of ~30
simultaneous SIP packets from one host overruns this queue at the lwip layer, so
most are dropped before the SIP task sees them. 30 parallel REGISTERs → 0
responses; the same REGISTER sent singly succeeds.
- **Fix to absorb bursts:** raise `CONFIG_LWIP_UDP_RECVMBOX_SIZE` (e.g. 16–32) and
  `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE`. Costs a little DRAM (we have ~200 KB free).

### 2. Per-source-IP rate limiter (working as designed — Issue #38)
Token bucket ~40 burst / 20 pkt/s sustained **per source IP**. A single-host load
generator cannot exceed this no matter how many virtual UAs it runs — that is the
DoS protection doing its job. Real fleets register from many IPs. To load past it
for testing, use multiple source hosts or temporarily widen the bucket.

### 3. `lvgl_task` starves Core-1 idle task in STATION mode (task-WDT)
On a **quiet** STA-mode boot the task watchdog fires repeatedly:
`Task watchdog got triggered … IDLE1 (CPU 1) … CPU 1: lvgl_task`. The display
render task monopolizes Core 1 (where it is pinned; SIP+HTTP run on Core 0). The
first AP-mode boot of the session did **not** show this, so it correlates with
STA mode (more Wi-Fi/event logging → the on-screen log terminal re-renders
constantly → lvgl never yields enough for IDLE1).
- **Candidate fixes:** throttle/coalesce the on-screen log terminal so it doesn't
  invalidate the screen per log line; add a small `vTaskDelay` floor in the LVGL
  loop; or, if lvgl legitimately needs the core, set
  `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n` (masks the warning only).

### 4. HTTP server wedges in STATION mode
`/api/status` and `/` time out even when idle: the server **accepts** the TCP
connection (slow, ~1 s) then **RSTs it without an HTTP response**. SIP (also
Core 0) keeps answering `OPTIONS`, so the UDP path is alive — the TCP/HTTP accept
loop specifically is not servicing requests in STA mode. Not reproduced on the
host build (CI `/api/status` smoke test passes), so it is device/STA-specific.
- **Investigate:** the HTTP accept loop's interaction with STA-mode netif / the
  captive-portal branch / Core-0 scheduling while `lvgl_task` saturates Core 1.

## Recommended next steps (priority order)
1. Fix the on-screen log → LVGL render storm (root of the Core-1 WDT). P0 for the
   display build's stability under any real traffic.
2. Investigate the STA-mode HTTP accept-then-RST wedge. P0 for dashboard usability
   off the SoftAP.
3. Raise `CONFIG_LWIP_UDP_RECVMBOX_SIZE` to absorb signaling bursts. P1.
4. Re-run this suite from **multiple source hosts** (to bypass the per-IP limiter)
   once 1–2 are fixed, to get true concurrent-registration / call-setup ceilings.

## Running the suite
```
python tests/load/sip_stress.py --host <device-ip> --clients 30 --echo-calls 8
python tests/load/sip_stress.py --host <device-ip> --register-only --pace 0.1
```
`--pace` (seconds between launches) keeps you under the rate limiter / mailbox.
The tool samples `GET /api/status` for server-side packet & pool counters.

---

## Update — findings #3 & #4 FIXED (verified on hardware)

Both STA-mode issues are resolved (commit `b04ecac`):
- **Core-1 lvgl watchdog** → fixed by coalescing/rate-limiting the on-screen log
  in `lvgl_task` (≤8 lines / 150 ms). Zero `task_wdt` lines on boot; servers up at
  ~6 s (was ~11 s).
- **HTTP accept-then-RST** → root cause was the accept loop's **3 KB pthread stack**
  (`std::thread`); `sendApiStatus` overflowed it on-device. Fixed with
  `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=8192` + bind `INADDR_ANY`.

### Real numbers (paced ~10/s, single source, post-fix)
| Operation | Success | p50 | p95 | max |
|-----------|---------|-----|-----|-----|
| REGISTER  | 20/20 (100%) | 8.6 ms | 25.8 ms | 121 ms |
| Echo call (777) | 5/5 (100%) | 13.3 ms | 19.9 ms | 21 ms |

`/api/status`: 150 ms; `/api/cdr`: 17 ms; full dashboard `/`: ~6 s (84 KB, one-time).
Server counters: 21 clients registered, packetsDropped ~2. Still recommend testing
from multiple IPs to find the true concurrency ceiling (Issue #79).

### Update — finding #1 FIXED (Issue #78)

`CONFIG_LWIP_UDP_RECVMBOX_SIZE` raised 6 → 32 in `sdkconfig.defaults`
(`CONFIG_LWIP_TCPIP_RECVMBOX_SIZE` also set to 32). Not yet re-verified against
a live burst re-run on hardware — that re-run (`sip_stress.py --clients 30`
unpaced, confirming a materially higher success rate) is still open.

---

## Update — Issue #79: multi-source-IP ceiling, measured (host build, one box)

`sip_stress.py` gained `--source-ips` / `--source-ip-base` / `--source-ip-count`
(bind each virtual UA's own socket to a distinct local address instead of
sharing the OS-picked default) and `--hold-ms` (keep an echo call open between
ACK and BYE so concurrent calls actually overlap in the Session pool instead of
each completing before the next INVITE lands). This is a real bypass of the
Issue #38 limiter, not a workaround for a weaker check: `RequestsHandler::
allowPacket`'s `_rateBuckets` is `unordered_map<uint32_t, RateBucket>` keyed on
`sin_addr.s_addr` alone (`RequestsHandler.cpp`) — no port in the key — so any
genuinely distinct source IP draws its own 40-burst/20-pkt/s bucket.

### What was actually run

**Host build** (`SipServer.exe`, Windows, defaults: `POCKETDIAL_MAX_CLIENTS=32`
/ `POCKETDIAL_MAX_SESSIONS=8`), **one physical machine**, source IPs drawn from
`127.0.0.2`–`127.0.0.41` — Windows treats the whole `127.0.0.0/8` block as
loopback with zero configuration (unlike Linux, which by default brings up only
`127.0.0.1`), so this needed no `ip addr add` / netns setup. This is **not** a
real multi-host run: one NIC, one kernel network stack, one process under test.
It answers the "does the registrar's own pool logic degrade cleanly past the
per-IP limiter" question; it does **not** answer anything ESP32-specific
(lwIP mailbox sizing, FreeRTOS task scheduling, the SoftAP association cap) or
anything about real network latency/loss. See "What a real run still needs"
below.

```
python tests/load/sip_stress.py --host 127.0.0.1 --port <sip-port> \
  --clients 40 --register-only --source-ip-base 127.0.0. --source-ip-count 40
```
**Result: client-pool ceiling confirmed exactly at 32.**
```
REGISTER storm: 40 attempts, 32 ok (80%)
status codes: 200:32, 503:8
```
`GET /api/status` afterward showed exactly 32 entries in `clients`, all with
distinct `127.0.0.x` addresses. The server stayed up and the dashboard stayed
responsive through and after the run — no crash, no watchdog-equivalent hang,
which is the "clean 503 path" half of #79's acceptance criterion, confirmed for
ordinary REGISTER/INVITE-to-a-real-extension traffic.

```
python tests/load/sip_stress.py --host 127.0.0.1 --port <sip-port> \
  --clients 32 --echo-calls 16 --source-ip-base 127.0.0. --source-ip-count 32 \
  --hold-ms 1000
```
**Result: session ceiling of 8 is real on the server side, but the `777`
echo-test path does not surface it to the caller as a 503.** All 16 concurrent,
held-open echo calls came back `200 OK` from the client's point of view. The
server's own log told a different story: exactly 8 `Session Created between
<ext> and 777` lines appeared (matching `POCKETDIAL_MAX_SESSIONS`), and
**zero** log output at all for the other 8 attempted calls — `allocateSession()`
returned `nullptr` for them and `RequestsHandler::onInvite`'s `777` branch
silently skips the session bookkeeping in that case, having already enqueued
the `180`/`200` responses *before* checking. Filed as **#115** (found while
executing #79, not fixed here — see that issue for the root cause,
`RequestsHandler.cpp` ~line 755, and why the fix isn't a one-liner: it needs a
trace of what a BYE against an untracked Call-ID currently does).

So the honest measured statement is: **peak tracked concurrent sessions = 8
(matches `POCKETDIAL_MAX_SESSIONS`); the ordinary call-setup path 503s cleanly
at that ceiling (verified by code inspection, `RequestsHandler.cpp` ~line
1008); the `777` diagnostic/echo path does not enforce it and needs #115
before it can be used to validate the session ceiling from the caller's side.**

### Other things this run surfaced
- **`_rateBuckets.size() >= 256` fail-safe** (`RequestsHandler.cpp`): past 256
  distinct source IPs concurrently tracked, *new* IPs get dropped outright
  regardless of their own token count. Caps how far the loopback-aliasing
  technique above scales on one box — not hit at 40 IPs, worth knowing before
  trying hundreds.
- **OPTIONS-keepalive pruning interacts with quick successive runs.** Each
  `sip_stress.py` invocation closes its UA sockets on exit; the server then
  logs `Pruning client due to missed OPTIONS keepalive pings` for that whole
  batch a little later, so polling `/api/status` shortly after starting a new
  run against a server that still has a prior run's now-dead clients live can
  read a confusingly low or transiently-zero client count. Not a bug — just a
  timing trap when scripting back-to-back runs against one long-lived server.
- **`--source-ips`/`--source-ip-count` vs. `--pace`:** `--pace` exists to stay
  *under* the per-IP token bucket on a single-source run; the source-IP flags
  exist to bypass that bucket entirely by giving every UA its own bucket. Using
  both together works but is usually pointless — pick one goal per run.

### What a real multi-host run still needs
This environment had exactly one machine available. A production capacity
claim (not just "the registrar's own pool bookkeeping is sound") still needs:
- **Actual separate physical or VM hosts** (or, short of that, Linux network
  namespaces / real secondary interface addresses via `ip addr add`) sending
  from real, independent network stacks — the loopback-aliasing trick above
  shares one kernel's UDP stack and one CPU with the process under test, so it
  cannot surface anything about real NIC/driver behavior under load.
- **The actual ESP32 firmware**, not the host build — the host build shares the
  same `RequestsHandler`/`Registrar` C++ logic, so the pool-ceiling numbers
  above are a legitimate stand-in for that logic specifically, but say nothing
  about `CONFIG_LWIP_UDP_RECVMBOX_SIZE`, FreeRTOS task-stack pressure, or the
  SoftAP association cap (`docs/SCALING.md` §5) — those need the real board.
- **Real network conditions** (latency, jitter, loss) that a same-host loopback
  run cannot produce.
