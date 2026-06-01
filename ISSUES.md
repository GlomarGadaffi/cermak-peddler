# Issue Tracking & Architectural Roadmap

This document serves as the active issue tracker and architectural roadmap for **pocket-dial**. It tracks high-impact concurrency, performance, and hardware-specific issues identified during production deployments, along with their resolution status.

---

## Active Issues

### 🔴 Issue #48: `RequestsHandler` Mutex Lock Contention under Status Polling
* **Status**: ⏳ Open / Planned
* **Labels**: `performance`, `concurrency`
* **Severity**: Medium

#### Description
In `RequestsHandler`, a single `std::mutex _mutex` is used to protect registered clients (`_clients`) and active calls (`_sessions`). The web dashboard query APIs (`getActiveClients()`, `getActiveSessions()`, `getClientCount()`, `getSessionCount()`) acquire this mutex synchronously.

Whenever the CGA CRT web dashboard polls the `/api/status` endpoint, the `HttpServer` invokes these methods. Under high polling rates or heavy load, dashboard status queries directly compete for `_mutex` with high-priority incoming SIP UDP packets (processed in `RequestsHandler::handle`) and periodic engine ticks (`RequestsHandler::tick`), introducing signaling latency or potential packet loss on resource-constrained targets like the ESP32.

#### Proposed Solution
Implement **state snapshotting** or double-buffering. Decouple dashboard queries by having a separate, throttled task or loop write lightweight, atomic, or copied states to a secondary container. This allows the HTTP server to serve client queries lock-free or with minimal read-only contention, completely bypassing the core signaling critical path.

---

### 🔴 Issue #50: Synchronous Client Handling blocking `HttpServer` Accept Loop
* **Status**: ⏳ Open / Planned
* **Labels**: `bug`, `network`
* **Severity**: High

#### Description
In `src/Helpers/HttpServer.cpp`, the accept thread handles incoming TCP connections synchronously in a single-threaded loop:
```cpp
void HttpServer::acceptLoop()
{
    while (_running)
    {
        ...
        int clientSock = static_cast<int>(accept(_listenSock, ...));
        ...
        handleClient(clientSock);
    }
}
```
If an HTTP client has a poor network connection or is slow to complete standard socket operations (e.g., waiting on `recv` timeouts up to the 5-second `SO_RCVTIMEO` threshold), the main HTTP accept thread stalls, completely blocking any other incoming dashboard or API connections.

#### Proposed Solution
Transition `HttpServer` to a non-blocking select/poll architecture, utilize a thread-pool/worker queue, or on ESP32 migrate to the native ESP-IDF `esp_http_server` component which handles connections asynchronously.

---

## Resolved Issues

### 🟢 Issue #49: Core Task Pinning Imbalance (SIP Signaling & HTTP sharing Core 0)
* **Status**: ✅ Resolved (v1.2.0 / `c7eb41d`)
* **Labels**: `architecture`, `esp32`

#### Description
In `main/esp_main.cpp`, `sip_server_task` was pinned to Core 1 and `http_server_task` was pinned to Core 0. However, `UdpServer::startReceive()` spawned `udp_receiver_task` pinned to Core 0.

Because the `udp_receiver_task` executes incoming packet parsing and signaling state logic (`RequestsHandler::handle`) synchronous-inline in its thread context, the entire SIP signaling control plane was actually running on Core 0! This forced both the high-priority SIP signaling plane and the long-lived, potentially blocking HTTP server thread to share CPU cycles on Core 0, while Core 1 sat largely idle running a low-frequency 1Hz ticking task.

#### Resolution
Introduced the `POCKETDIAL_UDP_RX_CORE` compile-time define, which defaults to Core 1 for standard SoftAP and W5500 Ethernet builds. This co-locates the high-priority UDP socket receiver on Core 1 alongside the main SIP engine task, achieving a clean dual-core split. On the display target (which reserves Core 1 exclusively for high-frequency LVGL screen redraws), it is overridden to Core 0, keeping signaling and rendering perfectly isolated.

---

### 🟢 Issue #51: Move Socket Syscalls outside `RequestsHandler` Critical Sections
* **Status**: ✅ Resolved (v1.1.0 / `eb125ab`)
* **Labels**: `performance`, `refactoring`

#### Description
To minimize lock hold times on high-concurrency systems, standard network-bound actions (like `sendto` socket syscalls), address translation resolutions (like calling `getPrimaryLocalIP()`), or large-string allocations should be kept outside the primary mutex locked blocks. Under heavy load, executing UDP socket syscalls inline inside locked blocks introduced microsecond-scale stalls that escalated lock contention.

#### Resolution
Refactored `RequestsHandler::handle(std::shared_ptr<SipMessage> request)` to accumulate outbound events inside a local `_outbox` vector. The `_mutex` is now held strictly during state-machine mutations. Once the locked block exits, the handler loops through `_outbox` and fires off the `sendto` socket syscalls outside the critical path, keeping lock-hold durations to microseconds.
