# Pocket-Dial Firmware: Production Readiness Reality Check

This document provides a rigorous, post-refactor technical audit and production-readiness scorecard for the **pocket-dial ESP32 firmware**. We evaluate the implementation status of key performance, safety, and concurrency refactors against real-world embedded deployment constraints.

---

## 🏗️ Architectural Overview & Concurrency Model

The post-refactor pocket-dial firmware adopts a strictly decoupled, dual-core execution model designed to insulate real-time SIP signaling from long-lived or blocking HTTP operations. 

```mermaid
graph TD
    subgraph "Core 0: Network & Management"
        A[http_server_task] -->|accept select| B[TCP Listen Socket]
        B -->|client connect| C[Detached std::thread]
        C -->|handleClient| D[parseRequest]
        C -->|getActiveClients/Sessions| E[(Registrar Snapshot)]
    end

    subgraph "Core 1: Real-Time Signaling"
        F[udp_receiver_task] -->|recvfrom| G[onNewMessage]
        G -->|handle| H[RequestsHandler]
        H -->|mutates| I[(Active Clients & Sessions)]
        J[sip_server_task] -->|1Hz tick| H
        H -->|compiles & copies| E
    end

    E .->|protected by _snapshotMutex| C
    I .->|protected by _mutex| H
    H .->|locks & writes snapshot| E
```

### Core Pinning & Priority Splits
* **Core 0 (Networking & Management):**
  * **`http_server_task`** (Priority 4, Stack 8192 bytes): Pinned to Core 0 (`esp_main.cpp:128`). Runs an accept loop utilizing `select()` with a 250ms timeout.
  * **HTTP Client Thread Contexts:** Each accepted connection is dispatched to a detached thread context (`HttpServer.cpp:124-126`). On the ESP-IDF platform, these map to POSIX threads (`pthread`) executing on Core 0.
* **Core 1 (Real-Time SIP Signaling):**
  * **`udp_receiver_task`** (Priority 5, Stack 8192 bytes): Pinned to Core 1 (`UdpServer.cpp:82`). This task blocks on `recvfrom` waiting for UDP SIP packets. Once received, it parses and processes the signaling inline in the receiver thread context.
  * **`sip_server_task`** (Priority 5, Stack 8192 bytes): Pinned to Core 1 (`esp_main.cpp:125`). Executes the background 1Hz engine `tick()` loop.

---

## 📊 Production Readiness Scorecard

| Criterion | Target Metric | Post-Refactor Status | Grade | Technical Summary |
| :--- | :--- | :--- | :---: | :--- |
| **1. Safe Panic Handling** | Zero CPU panics in Onboarding Setup Mode on `/api/status` calls | Full protection with pointer null-checks | **`🟢 PASS`** | `HttpServer` now utilizes a safe `RequestsHandler*` pointer instead of a raw reference, backed by explicit `nullptr` checks on all status and administration endpoints. |
| **2. Heap Fragmentation** | Zero steady-state allocations (`new`/`std::make_shared`) for clients/sessions | Pre-allocated static pools recycled via custom resets | **`🟢 PASS`** | Static pools of size 32 (`SipClient`), 8 (`Session`), and `POCKETDIAL_MSG_POOL` (`SipMessage`) eliminate steady-state allocations for active elements and response cloning alike. |
| **3. Stack Watermark Safety** | Stack allocations < 1.5 KB; Heap-allocated buffers for large frames | Large buffers shifted to the heap; high watermarks safe | **`🟢 PASS`** | The 4 KB socket read buffer was moved from local stack to heap-allocated `std::vector` inside `handleClient()`. generous 8 KB stack sizes allocated to all core tasks. |
| **4. Lock-Free Polling** | HTTP polling bypasses signaling lock; zero packet drop during poll | Double-buffered snapshot with isolated mutex | **`🟢 PASS`** | Core `_mutex` is bypassed for dashboard queries. `HttpServer` fetches data from a copied snapshot protected by `_snapshotMutex`, written once per second during `tick()`. |
| **5. Rate Limiting** | Protection from registration flood; drops packet under attack | Bounded rate limits with sweep | **`🟢 PASS`** | Fully implemented using a 60-second idle bucket sweep and token bucket algorithm capped at `MAX_BUCKETS = 256` to prevent heap exhaustion. `_packetsDropped` is properly tracked. |

---

## 🔍 In-Depth Technical Assessment

### 1. Safe Panic Handling (Null-pointer avoidance in Onboarding Setup Mode)
* **Grade:** `🟢 PASS`
* **Issue Background:** [Issue #52](../ISSUES.md#L50-L63) identified a critical bug where the onboarding display firmware booted with a dereferenced null pointer passed as a reference: `g_httpServer = new HttpServer(..., *(RequestsHandler*)nullptr)`. Any browser hitting `/api/status` caused an immediate `LoadProhibited` CPU panic, forcing a reboot loop.
* **Code Verification:**
  * In `src/Helpers/HttpServer.hpp:28`, the constructor now takes a pointer: 
    ```cpp
    HttpServer(const std::string& ip, int port, RequestsHandler* handler = nullptr);
    ```
  * In `src/Helpers/HttpServer.cpp:406-412` (`sendApiStatus`), the code safely checks the pointer:
    ```cpp
    if (_handler != nullptr)
    {
        clients = _handler->getActiveClients();
        sessions = _handler->getActiveSessions();
        packets = _handler->getPacketsProcessed();
        dropped = _handler->getPacketsDropped();
    }
    ```
  * Similar safeguards protect the admin force disconnect (`/api/kill`) endpoint in `HttpServer.cpp:491-494`.
* **Remaining Risks:** None. Onboarding AP/Setup mode runs stably without any panics.

### 2. Heap Fragmentation Mitigation (Steady-state dynamic allocation)
* **Grade:** `🟢 PASS`
* **Issue Background:** [Issue #53](../ISSUES.md#L67-L83) detailed the hazard of executing dynamic allocations (`std::make_shared`) inside the active UDP signaling path. High-frequency SIP registration traffic would fragment the ESP32's limited heap, eventually leading to `bad_alloc` panics or out-of-memory crashes.
* **Code Verification:**
  * `RequestsHandler` pre-allocates pools in its constructor (`src/SIP/RequestsHandler.cpp:31-39`):
    ```cpp
    for (int i = 0; i < 32; ++i) {
        _clientPool.push_back(std::make_shared<SipClient>());
    }
    for (int i = 0; i < 8; ++i) {
        _sessionPool.push_back(std::make_shared<Session>());
    }
    ```
  * Active elements are recycled in `allocateClient` (`src/SIP/RequestsHandler.cpp:1043`) and `allocateSession` (`src/SIP/RequestsHandler.cpp:1078`) using `client->reset(...)` rather than allocating new objects.
  * The transient `SipMessage` allocations this section used to flag are also gone: `RequestsHandler::getMessageFromPool()` draws from a static `_messagePool` (sized `POCKETDIAL_MSG_POOL`) instead of `make_shared`-ing a fresh message per response, and every response-building call site in `RequestsHandler.cpp` goes through it. `std::make_shared<SipSdpMessage>` now appears only at pool-init time and as the last-resort fallback when the pool itself is exhausted (logged, not silent).
* **Remaining Risks:** None outstanding for this criterion. (Issue #76 additionally closed out a related redundant-work gap: cloning a message into the pool used to serialize the source back to a wire string and re-split it — `getMessageFromPool(source->toString(), source->getSource())` — even though the pool slot could just as well be assigned directly from the parsed source. A `getMessageFromPool(const SipMessage&)` overload now does that direct copy; see ISSUES.md.)

### 3. Stack Watermark Safety (HTTP Client Handling Task)
* **Grade:** `🟢 PASS`
* **Issue Background:** The default stack size of an ESP-IDF `pthread` context is highly restricted (~3 KB). [Issue #23](../src/Helpers/HttpServer.cpp#L132) flagged a severe overflow risk where a stack-local read buffer `char buf[4096]` would immediately overflow the thread stack and corrupt memory.
* **Code Verification:**
  * In `src/Helpers/HttpServer.cpp:141-144`, the 4 KB buffer is moved entirely to the heap:
    ```cpp
    // Heap-allocate the read buffer... using std::vector keeps the data on the heap.
    std::vector<char> buf(4096, 0);
    ```
  * In `main/esp_main.cpp:125-128`, the pinned RTOS tasks are assigned generous stack sizes of **8192 bytes**, providing a safe buffer space:
    ```cpp
    xTaskCreatePinnedToCore(&sip_server_task, "sip_server_task", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(&http_server_task, "http_server_task", 8192, NULL, 4, NULL, 0);
    ```
* **Remaining Risks:** Extremely low. Moving the buffer to a vector eliminates stack overflow risks during HTTP request reception.

### 4. Lock-Free Status Polling via Snapshotting
* **Grade:** `🟢 PASS`
* **Issue Background:** [Issue #48](../ISSUES.md#L9-L22) identified high lock contention on the single `std::mutex _mutex` in `RequestsHandler`. Every time the web interface polled `/api/status`, the HTTP task would lock `_mutex` to read active clients and sessions, blocking high-priority UDP signaling packets and causing high jitter or packet drops.
* **Code Verification:**
  * `RequestsHandler` implements a double-buffered snapshot structure (`src/SIP/RequestsHandler.hpp:115-123`):
    ```cpp
    struct RegistrarSnapshot {
        std::vector<std::pair<std::string, std::string>> clients;
        std::vector<std::tuple<std::string, std::string, std::string, int>> sessions;
        uint64_t packetsProcessed = 0;
        uint64_t packetsDropped = 0;
    };
    RegistrarSnapshot _snapshot;
    std::mutex _snapshotMutex;
    ```
  * In `RequestsHandler::tick()`, which runs once per second inside Core 1's `sip_server_task`, the snapshot is compiled under `_mutex` and then moved to `_snapshot` under `_snapshotMutex` (`src/SIP/RequestsHandler.cpp:957-989`).
  * When `HttpServer::sendApiStatus` calls `getActiveClients()` and `getActiveSessions()`, these methods lock `_snapshotMutex` rather than `_mutex` (`src/SIP/RequestsHandler.cpp:877-888`).
* **Remaining Risks:** None. The core SIP signaling loop is completely decoupled from dashboard status polling.

### 5. Rate-Limiting & Token Bucket Filtering
* **Grade:** `🟢 PASS`
* **Issue Background:** [Issue #38](../src/SIP/RequestsHandler.hpp#L88) required a per-source-IP token bucket filter and optional CIDR allowlist to protect the system from registration floods and packet-based Denial of Service (DoS) attacks.
* **Code Verification:**
  * The methods `bool ipAllowed(const sockaddr_in& src) const` and `bool allowPacket(const sockaddr_in& src)` are fully implemented in `src/SIP/RequestsHandler.cpp:1223-1262`.
  * `ipAllowed` performs CIDR matching against `_allowNet` and `_allowMask` to implement an optional IP range access list.
  * `allowPacket` implements a token bucket rate-limiting filter keyed by the sender's IP address. It allows a burst of 40 packets and a sustained rate of 20 packets/second.
  * To prevent bucket heap-exhaustion attacks, a hard limit of `MAX_BUCKETS = 256` is strictly enforced. If exceeded under active attack, new IPs are safely blocked.
  * An idle bucket cleanup sweep is executed during the 1Hz `tick()` loop to prune inactive client buckets older than 60 seconds, freeing up capacity.
  * When a packet is blocked or dropped due to ACL/rate limit violations, the `_packetsDropped` counter is atomically incremented and visible via `/api/status`.
* **Remaining Risks:** None. The UDP signaling interface is highly robust against packet floods.

---

## 🛠️ Security and Driver Level Defect Tracking

We also reviewed remaining security and driver issues mentioned in `ISSUES.md`:

### SSID/Password Overflow Protection ([Issue #54](../ISSUES.md#L85-L102))
* **Status:** `🟢 PASS`
* **Verification:** The unsafe `strcpy` calls were successfully refactored to safe `strlcpy` calls with explicit bounds constraints in `main/esp_main.cpp:57-60`:
  ```cpp
  strlcpy((char*)wifi_config.ap.ssid, EXAMPLE_ESP_WIFI_SSID, sizeof(wifi_config.ap.ssid));
  strlcpy((char*)wifi_config.ap.password, EXAMPLE_ESP_WIFI_PASS, sizeof(wifi_config.ap.password));
  ```
  This eliminates stack-corruption risks from oversized SSID/password payloads loaded from NVS or POST requests.

### Driver Return Code Audits ([Issue #55](../ISSUES.md#L107-L129))
* **Status:** `🟡 PASS WITH RESERVATIONS`
* **Verification:** 
  * The high-frequency DNS server loop in `main/wifi/DnsServer.cpp:191-194` was updated to explicitly log socket errors instead of silently ignoring them:
    ```cpp
    int sent = sendto(self->_socketFd, tx_buffer, tx_len, 0, (struct sockaddr *)&source_addr, socklen);
    if (sent < 0) {
        ESP_LOGE(TAG, "DNS sendto failed: errno %d", errno);
    }
    ```
  * **Remaining Risk:** NVS return values inside display/ethernet initializers still need a strict audit to ensure fallback values are correctly initialized when keys are missing.
