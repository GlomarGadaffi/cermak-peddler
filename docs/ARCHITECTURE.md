# Pocket-Dial ESP32 Firmware: Deep Architectural Review

This document provides a highly technical, deep architectural analysis of the **pocket-dial** firmware. It reviews the post-refactor system topology, multi-core scheduling paradigm, component communication boundaries, select-based HTTP thread dispatching model, and concurrency optimizations.

---

## 1. System Topology & Component Overview

**pocket-dial** is an ultra-low-latency, dual-core SIP registrar and proxy server designed to run on resource-constrained ESP32 and ESP32-S3 microcontrollers. It supports multiple networking interfaces (Wi-Fi SoftAP/Station and SPI/RMII wired Ethernet) and drives smart displays (e.g., JC3248W535) using the LVGL graphics library.

The firmware architecture is divided into three core logical layers:
1. **Network Hardware & Driver Layer**: Controls physical media (Wi-Fi radio, W5500 SPI Ethernet MAC/PHY, LAN8720 RMII PHY) and registers low-level event handlers.
2. **Signaling & State Engine Layer (`RequestsHandler`)**: A lightweight RFC 3261-compliant SIP registrar and session controller managing client registration leases, active SIP sessions, and intercom broadcasting/paging features.
3. **User Interface & Query Layer**: Consists of a custom select-based, thread-dispatching `HttpServer` serving a retro CGA CRT web dashboard, an mDNS service responder, and a high-frequency LVGL-based GUI display task.

```mermaid
graph TD
    subgraph Core 1 [Core 1: Graphics & Real-Time Loop]
        A[lvgl_task 10ms] -->|Drives| B["AXS15231B LCD (QSPI)"]
        A -->|Reads| C["AXS15231B Touch (I2C)"]
        D[sip_server_task] -->|Runs| E[RequestsHandler::tick]
    end

    subgraph Core 0 [Core 0: Network & Web Control]
        F[http_server_task] -->|Listens| G[TCP Port 80]
        G -->|Select Activity| H["Detached Thread Dispatch"]
        H -->|Lock-Free Read| I["Registrar Snapshot (Clients/Sessions)"]
        J[system_status_task 500ms] -->|Updates| K[UI Status & Battery Volts]
        L[udp_receiver_task] -->|Reads| M[UDP Port 5060]
    end

    subgraph Hardware [Physical Hardware Layer]
        B
        C
        N["W5500 / LAN8720 Ethernet"] <-->|LwIP Stack| L
        N <-->|LwIP Stack| G
    end

    E -->|Pre-allocated Pool| O[SipClient / Session Memory]
    M -->|Dispatches Packet| E
    E -->|Write-Buffer| P[Local Outbox Vector]
    P -->|Send Outside Lock| N
```

---

## 2. Core Task Topology & Affinity Splits

To prevent render frame drops and network packet loss, **pocket-dial** enforces a strict core affinity split that isolates real-time communication tasks from CPU-intensive graphics rendering.

The system assigns FreeRTOS tasks to specific cores using `xTaskCreatePinnedToCore`:

### Core Affinity Allocation Table

| Task Name | Priority | Core Target (Display) | Core Target (Headless ETH) | Stack Size | Description |
| :--- | :---: | :---: | :---: | :---: | :--- |
| `lvgl_task` | 5 | **Core 1** | *N/A* | 8192 Bytes | Runs the LVGL render loop (`lv_timer_handler()`) every 10ms. Must have exclusive Core 1 access to avoid micro-stuttering. |
| `sip_server_task` | 5 | **Core 0** | **Core 1** | 8192 Bytes | Ticks the SIP state engine (`RequestsHandler::tick()`) and sweeps expired leases. |
| `udp_receiver_task` | 5 | **Core 0** | **Core 1** | 8192 Bytes | Listens on UDP port 5060, parses incoming packet headers, and dispatches them to the handler. |
| `http_server_task` | 4 | **Core 0** | **Core 0** | 8192 Bytes | Runs the select-based HTTP server accept loop. Spawns detached client worker threads. |
| `status_task` | 3 | **Core 0** | *N/A* | 4096 Bytes | Polls ADC battery voltage divider (GPIO 5) and updates on-screen status fields every 500ms. |

> [!IMPORTANT]
> **Task Isolation Design**:
> On the smart-display target (JC3248W535), **Core 1** is reserved exclusively for the `lvgl_task` to guarantee 60 FPS UI rendering. All network handling, SIP processing, and HTTP worker threads are pinned to **Core 0**. 
> For headless Ethernet/PoE builds, the high-priority `sip_server_task` and `udp_receiver_task` are shifted to **Core 1**, leaving **Core 0** to handle lower-priority HTTP/TCP traffic and background tasks.

---

## 3. Concurrency Architecture & Contention Mitigation

High-frequency HTTP polling of the CGA dashboard under production loads can introduce severe lock contention, resulting in UDP packet dropouts. The post-refactor codebase implements three critical architectural patterns to mitigate concurrency bottlenecks.

### A. Snapshotted Registrar Query Model (Issue #48)
Historically, the `HttpServer` queried `RequestsHandler` active client and session collections by directly acquiring the central `_mutex`. This blocked the real-time SIP signaling loop during active JSON generation.

The post-refactor architecture decouples the HTTP control plane from the UDP signaling plane using a **double-buffered snapshot model**:

1. **State Mutation (Real-Time Path)**: The main SIP signaling engine processes packets and ticks on a dedicated thread, acquiring `_mutex` only during rapid internal state changes.
2. **Snapshot Generation**: At the end of each periodic tick inside `RequestsHandler::tick()`, the engine constructs a lightweight `RegistrarSnapshot` (`_snapshot`) containing plain STL vectors of active registration strings and call metadata. This is committed to a secondary buffer under a dedicated, short-lived `_snapshotMutex`.
3. **Lock-Free Read (HTTP Path)**: When the `HttpServer` handles a GET request to `/api/status`, it queries `getActiveClients()` and `getActiveSessions()`. These methods acquire only the lightweight `_snapshotMutex` for a fraction of a microsecond to copy the snapshot, completely eliminating lock contention with the active signaling loop.

```
[UDP Receiver Thread]               [RequestsHandler State]              [HTTP Server Thread]
         │                                     │                                  │
         ├─────── Mutate State ───────────────>│                                  │
         │   (Acquires & releases _mutex)      │                                  │
         │                                     │                                  │
         │                                     ├── Update Snapshot ──┐            │
         │                                     │   (Once per second) │            │
         │                                     │<────────────────────┘            │
         │                                     │ (Acquires _snapshotMutex)        │
         │                                     │                                  │
         │                                     │<─────────── GET /api/status ─────┤
         │                                     │    (Acquires _snapshotMutex)     │
         │                                     │                                  │
```

### B. Out-of-Lock Socket Syscalls (Issue #51)
Executing slow blocking socket syscalls (like `sendto`) while holding the internal registrar lock created significant latency spikes. 

The refactored `RequestsHandler::handle` utilizes an **Outbox Pattern**:
* When processing an incoming SIP request, any generated responses or call-forwarding invites are temporarily accumulated inside a local `_outbox` vector (`std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>>`).
* The central `_mutex` is released immediately after the state machine logic completes.
* Once the critical section is exited, the thread iterates through the local outbox and executes the UDP `sendto` syscalls outside the lock, keeping registrar lock hold times at microsecond-scale.

### C. Static Memory Pools (Issue #53)
Dynamic heap allocations (`new`, `malloc`, `make_shared`) within the hot UDP signaling path are a major cause of memory fragmentation and non-deterministic jitter on embedded targets.

The post-refactor signaling engine implements **static memory pre-allocation**:
* During initialization, `RequestsHandler` pre-allocates contiguous arrays of `SipClient` and `Session` smart pointers inside the constructor (`_clientPool` of size 32, and `_sessionPool` of size 8).
* In steady-state operation, `allocateClient` and `allocateSession` search these pre-allocated pools to recycle unused objects, entirely bypassing the runtime heap.
* If the pool is exhausted under heavy load, the server automatically evicts the oldest expired client registration lease or returns `503 Service Unavailable`, protecting the core heap from out-of-memory (OOM) silent panics.

---

## 4. Select-Based HTTP Server Thread Dispatch Model

To prevent slow-client TCP connections from stalling the main HTTP accept thread, the `HttpServer` employs a select-monitored, multi-threaded worker dispatch model.

```
       [Main HTTP Server Thread]
                   │
         [select() on listen_sock]
                   │
           (Activity Detected)
                   │
         [accept() client socket]
                   │
       ┌───────────┴───────────┐
       ▼                       ▼
 [Spawn std::thread]     [Resume select()]
       │
 [Set SO_RCVTIMEO (5s)]
       │
 [Read body (Max 16KB)]
       │
 [Validate Same-Origin]
       │
 [Send HTTP Response]
       │
  [Close Socket]
```

### Accept Loop Implementation
1. The main HTTP accept loop uses `select()` on the listening socket with a `250ms` timeout to periodically yield execution and verify if the server is still running.
2. Upon activity, `accept()` is called to retrieve the client socket.
3. The server immediately dispatches client processing to a detached thread context (`std::thread([this, clientSock]() { handleClient(clientSock); }).detach()`), instantly freeing the accept thread to monitor subsequent connections.

### Worker Protection & Robustness (Issue #23)
* **Slowloris Protection**: The worker thread sets a strict 5-second socket receive timeout (`SO_RCVTIMEO`) using `setsockopt` to terminate slow-sending or dead TCP connections.
* **Heap Stack-Safety**: Rather than allocating a raw stack-local character buffer (which would overflow the tiny standard `pthread` thread stack on ESP32), the worker utilizes a heap-allocated `std::vector<char>` read buffer.
* **Buffer Overflow Cap**: The worker parses the `Content-Length` header and enforces a maximum payload limit of **16 KB** (16,384 bytes). If a client attempts to upload a larger body (e.g., in a malicious POST flood to `/api/wifi/connect`), the worker immediately responds with `413 Payload Too Large` and aborts the connection, securing the target's RAM.

---

## 5. Security & Network Protections

### Same-Origin Policy & CSRF Prevention (Issue #28 / #38)
Because the device serves as a local captive portal or open access point, malicious scripts running in background browser tabs on connected clients could attempt to trigger administrative side-effects (such as disconnecting users or reconfiguring Wi-Fi).

To prevent Cross-Site Request Forgery (CSRF), the server implements a robust **Same-Origin Check** inside `HttpServer::isSameOrigin`:
* If an incoming state-mutating POST request (`/api/kill`, `/api/wifi/connect`, `/api/wifi/mode_ap`) contains an `Origin` header (sent by modern browsers during cross-site requests), the server extracts the host domain.
* The server compares the `Origin` host string directly with the `Host` header sent by the client.
* If they do not match, the request is immediately rejected with `403 Forbidden` (`{"error":"cross-origin request rejected"}`).
* No wildcard `Access-Control-Allow-Origin: *` headers are ever returned on API routes, preventing cross-origin browser reads of active registration profiles.

### Per-Source-IP Token Bucket Rate Limiting (Issue #38)
To protect the registrar from UDP flood denial-of-service (DoS) attacks, the `RequestsHandler` integrates a thread-safe token bucket rate limiter:
* **Sustained/Burst Thresholds**: UDP packets are evaluated using a per-source IP token bucket with a default burst depth of **40 packets** and a sustained replenishment rate of **20 packets per second**.
* **Zero CPU Parse Overheads**: Rate check verification is executed before any SIP header parsing, dynamic routing, or database work. If an IP exceeds its burst threshold, the packet is instantly discarded, and the atomic `_packetsDropped` counter is incremented.
* **Eviction Cycle**: To prevent memory leak accumulation from transient spoofed IPs, inactive buckets are periodically evicted during the central registrar sweep.
* **Subnet CIDR Filtering**: If compiled with `-DPOCKETDIAL_ALLOW_CIDR="192.168.1.0/24"`, the registrar translates incoming IP addresses and blocks any traffic originating from outside the designated local network segment.
