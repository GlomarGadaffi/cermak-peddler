# Pocket-Dial Firmware: Performance Benchmarks & Methodology

This document outlines the performance benchmark plan, theoretical resource models, and live-board physical testing methodologies for the post-refactor **pocket-dial ESP32 firmware**. 

Since pocket-dial is deployed across multiple hardware form factors (headless SoftAP modules, W5500 Ethernet boards, and smart-display units running high-frequency graphics), this document establishes rigorous resource budgets and measurement guidelines rather than assuming a single physical board setup.

---

## 💾 1. Theoretical Resource Models (ESP32 Targets)

Here, we model the memory and latency footprints on the three primary production target boards:
1. **Target A: Headless ESP32-WROOM-32E** (Dual-core, 240MHz Xtensa D0WDQ6, 520 KB internal SRAM, no PSRAM).
2. **Target B: ESP32-S3 Smart Display (JC3248W535)** (Dual-core, 240MHz Xtensa LX7, 512 KB SRAM, 2 MB external PSRAM, running LVGL graphics).
3. **Target C: ESP32-W5500 Ethernet Gateway** (Headless gateway utilizing wired SPI Ethernet, dual-core, 520 KB internal SRAM).

---

## 🧵 2. Task Stack Allocations & Watermark Estimates

In FreeRTOS, the "stack high-water mark" is the minimum amount of free stack space (in bytes) that has remained unused since the task was created. If this value approaches zero, a stack overflow is imminent, which triggers an immediate CPU panic.

The post-refactor tasks are allocated generous stacks, resulting in highly secure margins.

### Stack Allocations vs. Projected Watermark Estimates

| Task Name | Core | Allocated Stack (Bytes) | Projected Peak Stack Usage (Bytes) | Projected High-Water Mark (Free Bytes) | Technical Risk Analysis & Design Details |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **`sip_server_task`** | Core 1 | 8,192 | 3,840 | 4,352 | Handles the 1Hz ticking engine, keeps alive, and sweeps expired clients. Low risk of recursion or heavy frames. |
| **`udp_receiver_task`**| Core 1 | 8,192 | 4,200 | 3,992 | **High-activity path.** Processes and parses incoming SIP string structures inline. Generous 8 KB allocation protects against complex headers. |
| **`http_server_task`** | Core 0 | 8,192 | 2,800 | 5,392 | Executes the non-blocking accept loop using `select()`. Light and secure as handling is delegated. |
| **HTTP client thread** | Core 0 | ~3,072 *Default* | 1,450 | 1,622 | Each active HTTP socket runs in a detached `pthread`. **PASS due to heap shift of 4 KB read buffer.** |

### 💡 Why HTTP Client Threads Do Not Overflow pthread Defaults
In the original design, allocating a stack-local buffer like `char buf[4096]` inside the HTTP connection handler would immediately exceed the ~3 KB default pthread stack limit allocated by the ESP-IDF RTOS layer, causing a silent stack overflow or memory corruption.

In the post-refactor design (`HttpServer.cpp:141-144`), we shift the buffer to the heap:
```cpp
std::vector<char> buf(4096, 0);
```
By allocating the vector, the 4,096 bytes are allocated from the **system heap** rather than the POSIX thread's stack. The thread stack itself only holds the vector's control structure (24 bytes on Xtensa), keeping the stack footprint to less than 1.5 KB and comfortably below the default pthread limits!

---

## 📦 3. Heap Memory Footprint Estimates

The ESP32 possesses a unified SRAM map, but internal memory is divided into Instruction RAM (IRAM) and Data RAM (DRAM). Free heap memory is DRAM available to the user.

We estimate heap consumption across three key operating states:

### Heap Consumption Models per Target State

```
  [State 1: Idle Baseline] ──> Pre-allocates static pools (~10 KB DRAM overhead)
            │
            ├──> [State 2: Active HTTP Polling] ──> Transient heap allocation (~4-6 KB DRAM, fast release)
            │
            └──> [State 3: Active SIP Call] ────> Steady-state memory footprint unchanged (Static pool reuse!)
```

| Target Board | State 1: Idle Baseline (No Clients Registered) | State 2: HTTP status Polling (CGA Dashboard Active) | State 3: Active SIP Call (2 Registered, 1 Call) | Heap Fragmentation Hazard Level |
| :--- | :---: | :---: | :---: | :---: |
| **Headless ESP32-WROOM** | 280 KB Free | 274 KB Free *(Transient)* | 279 KB Free | **Low** (Steady-state dynamic allocations eliminated via pooling) |
| **ESP32-S3 Smart Display**| 120 KB Free *(Graphics RAM consumed)*| 114 KB Free *(Transient)* | 119 KB Free | **Medium** (LVGL UI rendering and SIP engine share internal DRAM) |
| **ESP32-W5500 Ethernet** | 260 KB Free *(Ethernet buffers occupied)*| 254 KB Free *(Transient)* | 259 KB Free | **Low** (Stable socket handling outside signaling loop) |

### 🔍 Breakdown of State Calculations
1. **State 1 (Idle Baseline):**
   * Static pools are pre-allocated in `RequestsHandler::RequestsHandler`:
     * `_clientPool`: $32 \times \text{std::shared\_ptr<SipClient>} \approx 4.1\text{ KB}$
     * `_sessionPool`: $8 \times \text{std::shared\_ptr<Session>} \approx 2.2\text{ KB}$
     * Total pool footprint: $\approx 6.3\text{ KB}$ (plus basic vector overhead and static strings).
   * Total static overhead is a flat $\approx 10\text{ KB}$, providing known bounds.
2. **State 2 (Active HTTP Status Polling):**
   * Handled inside `HttpServer::sendApiStatus`.
   * Allocates a JSON response stream, temporary string formatting, and a heap vector for client readings:
     * 4 KB client read buffer (`buf` vector)
     * $\approx 1-2\text{ KB}$ string buffer for JSON payload generation.
   * This results in a transient $\approx 6\text{ KB}$ dip in free heap. **Crucially, this is immediately deallocated** when `handleClient` terminates and closes the socket, causing zero permanent fragmentation.
3. **State 3 (Active SIP Call):**
   * Reuses already allocated pool items from `_clientPool` and `_sessionPool`.
   * **Zero additional dynamic memory is allocated** for clients or sessions.
   * Minimal transient allocations ($< 1\text{ KB}$) occur in lwIP buffers and `SipMessage` parsing, returning free memory immediately to State 1 levels. This prevents the severe fragmentation failures identified in [Issue #53](../ISSUES.md#L67).

---

## ⚡ 4. HTTP API Request Latencies

We model request processing latencies based on the network and storage activities required for each HTTP endpoint.

| HTTP Endpoint | HTTP Method | Expected Latency (Normal Load) | Expected Latency (10 Clients Poll) | Processing Bottleneck & Hardware Activity |
| :--- | :---: | :---: | :---: | :--- |
| **`GET /`** | GET | 10–25 ms | 15–40 ms | Reading static index HTML from flash or embedded header (`CGA_INDEX_HTML`). |
| **`GET /api/status`** | GET | 3–8 ms | 10–25 ms | Low latency due to reading pre-built snapshot under `_snapshotMutex`. Completely bypasses SIP `_mutex`. |
| **`POST /api/kill`** | POST | 5–12 ms | 12–30 ms | Erases elements from the active client map and triggers a snapshot rebuild during the next `tick()`. |
| **`GET /api/wifi/scan`**| GET | 1,500–2,800 ms| 1,500–3,000 ms| **High Latency.** Switch to `APSTA` mode and block while the Wi-Fi transceiver scans all channels. |
| **`POST /api/wifi/connect`**| POST | 15–35 ms | 20–50 ms | Writes SSID & password to NVS flash, returns response, and triggers restart after 1 second. |
| **`POST /api/wifi/mode_ap`**| POST | 10–25 ms | 15–45 ms | Writes AP Standalone configuration to NVS, returns response, and restarts. |

> [!NOTE]
> The `/api/status` response latency is exceptionally low ($<8\text{ ms}$) because it reads from the pre-compiled `_snapshot` structure. It does not block on active UDP processing or lock the core SIP engine.

---

## 📈 5. Live-Board Measurement Methodology (Test Plan)

To validate these theoretical estimates on physical hardware, the QA/testing team must execute the following step-by-step physical measurement protocols on live boards.

### Phase 1: Task Stack Watermark Testing
To measure the actual stack high-water mark on a running ESP32:
1. Compile the firmware with `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` and `CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=y` enabled in `sdkconfig` via `idf.py menuconfig`.
2. Add a diagnostic task or high-frequency timer in your main program that prints task diagnostics every 5 seconds:
   ```cpp
   void diagnostic_task(void *pvParameters) {
       char buffer[512];
       while (1) {
           vTaskList(buffer);
           printf("Task Name\tState\tPrio\tStack\tNum\tCore\n");
           printf("%s\n", buffer);
           vTaskDelay(pdMS_TO_TICKS(5000));
       }
   }
   ```
3. Look at the `Stack` column in the output. The number returned represents the **minimum free stack space remaining (high-water mark)** in 32-bit words (or bytes, depending on compiler configuration). Ensure no task falls below **1,024 bytes** during active call processing or rapid dashboard reloading.

### Phase 2: Heap Footprint & Fragmentation Monitoring
To monitor heap stability and detect dynamic leaks:
1. Periodically query the native ESP-IDF heap caps APIs during different execution phases:
   * **Total Free Heap:** `heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`
   * **Minimum Free Heap Ever (System Watermark):** `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`
   * **Largest Free Block (Fragmentation Indicator):** `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`
2. **Setup State Test Steps:**
   * **Step A (Baseline):** Boot the device. Wait 10 seconds. Query the values.
   * **Step B (Stress Polling):** Initiate 10 concurrent requests polling `/api/status` using a tool like Apache Bench (`ab`):
     ```bash
     ab -n 500 -c 10 http://192.168.4.1/api/status
     ```
     During execution, observe the Largest Free Block. If it drops continuously and does not recover back to the Baseline size post-test, a memory leak or severe fragmentation is present.
   * **Step C (Call Stress):** Register 10 SIP client extensions (e.g., using MicroSIP or softphone simulators). Initiate multiple simultaneous intercom calls and group paging broadcasts (extension `999`). Check the Baseline heap before, during, and after the call. The post-call heap size must match the pre-call heap size exactly, verifying that the Static Pool recycler successfully cleaned up the session slots.

### Phase 3: HTTP API Latency Measurement
To record the exact network response times:
1. Save the following curl format file as `curl-format.txt`:
   ```text
       time_namelookup:  %{time_namelookup}s\n
          time_connect:  %{time_connect}s\n
       time_appconnect:  %{time_appconnect}s\n
      time_pretransfer:  %{time_pretransfer}s\n
         time_redirect:  %{time_redirect}s\n
    time_starttransfer:  %{time_starttransfer}s\n
                       ----------\n
            time_total:  %{time_total}s\n
   ```
2. Execute curl queries against the running device over Wi-Fi/Ethernet:
   ```bash
   curl -w "@curl-format.txt" -o /dev/null -s http://192.168.4.1/api/status
   ```
3. Record `time_total` (total round-trip duration). Repeat this under load (e.g., while actively making SIP calls) to confirm the snapshotting architecture prevents network-layer latency spikes.
