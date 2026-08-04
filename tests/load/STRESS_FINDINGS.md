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
