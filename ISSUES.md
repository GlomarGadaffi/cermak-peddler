# Issue Tracking & Architectural Roadmap

This document serves as the active issue tracker and architectural roadmap for **pocket-dial**. It tracks high-impact concurrency, performance, and hardware-specific issues identified during production deployments, along with their resolution status.

---

## Active Issues & Backlog Roadmap

### 🟡 Issue #68: Smoke-test call parking on real hardware
* **Status**: ⏳ Open / Planned
* **Labels**: `hardware-testing`, `parking`
* **Severity**: Medium

#### Description
Park (700–709), retrieve, and ring-back paths were ported from production and pass the
host unit suite, but need an end-to-end smoke test on real ESP32-S3 hardware with two
registered SIP phones. Verify: (a) park INVITE gets `200 OK` with `a=inactive` hold SDP;
(b) retrieve INVITE bridges both legs peer-to-peer; (c) ring-back fires after orbit
timeout and re-bridges. Also verify NVS survives a reboot (pzones key).

---

### 🟡 Issue #67: Smoke-test BLF presence subscription on a Yealink / Polycom phone
* **Status**: ⏳ Open / Planned
* **Labels**: `hardware-testing`, `blf`
* **Severity**: Medium

#### Description
SUBSCRIBE / NOTIFY (RFC 6665) and RFC 4235 dialog-info XML are implemented and compile
cleanly. Need hardware validation: register a Yealink T4X or Polycom VVX with a BLF key
pointed at another extension; verify the BLF LED goes idle → ringing → in-call → idle
across a full call cycle. Also verify 489 Bad Event is returned for unsupported packages
and that subscription expiry triggers a terminal NOTIFY.

---

### 🟡 Issue #44: End-to-end SIP call test needed on JC3248W535EN hardware
* **Status**: ⏳ Open / Planned
* **Labels**: `hardware-testing`, `verification`
* **Severity**: Medium

#### Description
Since physical target hardware (smart display JC3248W535EN, POE boards, etc.) is currently offline, we need a physical validation sweep once hardware is re-connected to verify that display screen redraw latency doesn't interfere with real-time SIP engine ticks.

---

### 🟡 Issue #41: SIP core: Arduino IDE platform detection guards need verification (ESP32/ARDUINO defines)
* **Status**: ⏳ Open / Planned
* **Labels**: `build-system`, `compatibility`
* **Severity**: Low

#### Description
Arduino IDE build configs should be verified against platform detection macros (`ESP32`, `ARDUINO`, `ESP_PLATFORM`) to ensure smooth compatibility for hobbyist flashing.

---

### 🔵 Issue #35: [Feature Request] Zero-Touch Phone Auto-Provisioning (HTTP)
* **Status**: ⏳ Open / Planned (Backlog)
* **Labels**: `feature-request`, `provisioning`
* **Severity**: Low

#### Description
Add a background HTTP directory service to push auto-provisioning configs directly to standard SIP phone handsets (Polycom, Yealink, Cisco).

---

### 🔵 Issue #33: [Feature Request] PCAP Dump Endpoint for Wireshark analysis
* **Status**: ⏳ Open / Planned (Backlog)
* **Labels**: `feature-request`, `diagnostics`
* **Severity**: Low

#### Description
Expose an HTTP endpoint `/api/diagnostics/pcap` to dump a live ring-buffer of SIP packets in PCAP format for quick network troubleshooting.

---

### 🔵 Issue #32: [Feature Request] Live SIP Tracer in the Web Terminal
* **Status**: ⏳ Open / Planned (Backlog)
* **Labels**: `feature-request`, `diagnostics`
* **Severity**: Low

#### Description
Stream live SIP UDP signaling packets directly to the CRT console landing page using WebSockets.

---

## External Audio Connector (Epic)

The `AnchorClient` interface (`src/SIP/AnchorClient.hpp`) lets you bridge an active SIP
call to any external audio system — a SIP trunk, a PSTN gateway, a recording server, or
an AI voice pipeline. Implement `makeCall()`, `writeAudio()`, and `AudioRxCallback` in
your subclass; the engine takes care of the SIP signaling on both legs.

`LoopbackAnchorClient` (`src/SIP/LoopbackAnchorClient.cpp`) is the reference
implementation: it echoes audio back to the caller and is useful as a smoke test and as a
template for real connector implementations.

### 🔵 Issue #65: AnchorClient — real host UDP media transport
* **Status**: ⏳ Open / Planned
* **Labels**: `media`, `desktop`, `rtp`
* **Description**: `RtpSender` socket + 20 ms pacing are `#if ESP_PLATFORM`; the host
  build stubs them out. For an AnchorClient to move real audio on a desktop/Pi host
  deployment, implement the host (Linux/Windows) UDP socket and 20 ms pacing loop
  alongside the ESP path. The `PlayoutBuffer` jitter buffer is already in place.

### 🔵 Issue #66: AnchorClient — sample connector (SIP trunk / VoIP provider)
* **Status**: ⏳ Open / Planned
* **Labels**: `api-integration`, `sip`, `documentation`
* **Description**: Provide a sample concrete AnchorClient that connects pocket-dial to a
  standard SIP trunk (REGISTER + INVITE/BYE using `libosip2` or raw UDP). This would let
  pocket-dial place and receive PSTN calls without any cloud dependency. The interface is
  already defined; this is just a worked example.

---

## Resolved Issues

### 🟢 Issue #48: `RequestsHandler` Mutex Lock Contention under Status Polling
* **Status**: ✅ Resolved (v1.3.0 / `32166b5` & `f09a98c`)
* **Labels**: `performance`, `concurrency`

#### Resolution Details
1. Decoupled HTTP status polling endpoints from the main SIP UDP receiver thread.
2. Implemented a double-buffered `RegistrarSnapshot` and a secondary, lightweight `_snapshotMutex` in `RequestsHandler`.
3. Scheduled a 1Hz statistics snapshot sweep in `RequestsHandler::tick()` to update the snapshot within the core signaling locked section.
4. Updated dashboard query APIs (`getActiveClients()`, `getActiveSessions()`, etc.) to query the snapshot lock-free, completely bypassing core mutex lock contention.

---

### 🟢 Issue #49: Core Task Pinning Imbalance (SIP Signaling & HTTP sharing Core 0)
* **Status**: ✅ Resolved (v1.2.0 / `c7eb41d`)
* **Labels**: `architecture`, `esp32`

#### Resolution Details
1. Pinning imbalance corrected via compile-time define `POCKETDIAL_UDP_RX_CORE` to balance SIP signaling (Core 1) and background HTTP tasks (Core 0).
2. Keeps real-time signaling separate from display rendering and HTTP queries.

---

### 🟢 Issue #50: Synchronous Client Handling blocking `HttpServer` Accept Loop
* **Status**: ✅ Resolved (v1.3.0 / `32166b5` & `f09a98c`)
* **Labels**: `bug`, `network`

#### Resolution Details
1. Converted `HttpServer::acceptLoop()` to use a non-blocking POSIX `select()` architecture with a 250ms timeout.
2. Dispatched client connections to detached threads (`std::thread(...).detach()`), running `handleClient()` asynchronously.
3. Prevents slow clients or long socket operations from stalling the accept loop, securing the dashboard from connection-stall DoS.

---

### 🟢 Issue #51: Move Socket Syscalls outside `RequestsHandler` Critical Sections
* **Status**: ✅ Resolved (v1.1.0 / `eb125ab`)
* **Labels**: `performance`, `refactoring`

#### Resolution Details
1. Outbound socket operations (`sendto`) are decoupled and accumulated inside a local `_outbox` vector.
2. Mutex locked blocks are held strictly during state machine mutations, and the accumulated outbox events are dispatched outside the critical path, reducing lock-hold durations to microseconds.

---

### 🟢 Issue #53 / #54: Null Pointer Dereference `*(RequestsHandler*)nullptr` in Onboarding Setup Mode
* **Status**: ✅ Resolved (v1.3.0 / `32166b5` & `f09a98c`)
* **Labels**: `bug`, `critical`, `smart-display`

#### Resolution Details
1. Changed the `HttpServer` constructor to accept a pointer `RequestsHandler* handler = nullptr` instead of a raw reference, enabling nullable registrar instantiation.
2. Added nullptr guards `if (_handler != nullptr)` inside all web endpoints.
3. Moved `isValidMessage()` to the public section of `SipMessage` to ensure visibility across handlers.
4. Modified `main/esp_main_display.cpp` to pass `nullptr` during onboarding AP fallback mode, avoiding undefined behavior and eliminating Tensilica CPU LoadProhibited boot panics.

---

### 🟢 Issue #55: Dynamic Heap Allocation in Real-Time SIP Signaling Loop
* **Status**: ✅ Resolved (v1.3.0 / `32166b5` & `f09a98c`)
* **Labels**: `performance`, `memory-safety`, `reliability`

#### Resolution Details
1. Eliminated all dynamic runtime heap allocations (`new` and `std::make_shared`) within the active UDP signaling path.
2. Pre-allocated static vectors for up to 32 `SipClient` and 8 `Session` objects inside `RequestsHandler`.
3. Reused pooled elements via a slot-recycling `reset()` pattern. Rejects additional incoming registrations with a robust `503 Service Unavailable` when the pool is saturated, protecting against heap fragmentation and OOM crashes.

---

### 🟢 Issue #56: Buffer Overflow Risk via `strcpy` in WiFi Config Initialization
* **Status**: ✅ Resolved (v1.3.0 / `32166b5` & `f09a98c`)
* **Labels**: `bug`, `security`

#### Resolution Details
1. Replaced all unsafe `strcpy` calls with size-limited, bounds-checking `strlcpy` inside `main/esp_main.cpp` and `main/esp_main_display.cpp`.
2. All SSID (32 bytes) and password (64 bytes) operations are strictly bounded to prevent stack and heap buffer overflows.

---

### 🟢 Issue #57: Unchecked NVS and Driver Return Codes in Display Boot Path
* **Status**: ✅ Resolved (v1.3.0 / `32166b5` & `f09a98c`)
* **Labels**: `bug`, `reliability`

#### Resolution Details
1. Enforced strict return status validations on all `nvs_get_u8` and `nvs_get_str` calls inside `main/esp_main_display.cpp`, defaulting safely to fallback setup mode if flash keys do not exist.
2. Added status validation on the DNS socket `sendto` syscall in `main/wifi/DnsServer.cpp`.

---

### 🟢 Issue #54: Session Pool Slots Permanent Exhaustion
* **Status**: ✅ Resolved (v1.4.0 / `b5c1a1f`)
* **Labels**: `bug`, `critical`, `regression-risk`

#### Resolution Details
1. Implemented a `release()` method on the `Session` class to clear all references (`_src`, `_dest`, `_inviteMessage`, `_pendingTargets`, and `_callID`).
2. Configured the `RequestsHandler`'s `endCall()`, `sweepExpired()`, and `forceDisconnect()` methods to explicitly invoke `release()` on active session objects from the pre-allocated pool upon termination.
3. Updated `allocateSession()` to scan and reclaim inactive session slots in the `_sessionPool` whose `Call-ID` is empty or no longer present in the active `_sessions` map, allowing infinite setup/teardown cycles.

---

### 🟢 Issue #55: Address of Record (AOR) Input Injection
* **Status**: ✅ Resolved (v1.4.0 / `b5c1a1f`)
* **Labels**: `bug`, `security`, `input-validation`

#### Resolution Details
1. Created `RequestsHandler::isValidAor()` which strictly whitelists alphanumeric characters and `.`, `-`, `_`, `+`.
2. Added AOR sanitization checks inside `onRegister()` and `onInvite()`, rejecting malformed inputs with a `400 Bad Request` response.

---

### 🟢 Issue #56: Compile-time Gated Default-Open Mode (Option B)
* **Status**: ✅ Resolved (v1.4.0 / `b5c1a1f`)
* **Labels**: `security`, `configuration`

#### Resolution Details
1. Introduced compile-time guard `POCKETDIAL_OPEN_REGISTRAR` in `RequestsHandler.hpp`, defined by default so that the registrar starts "open" for easy deployment.
2. If `POCKETDIAL_OPEN_REGISTRAR` is commented/undefined, the registrar switches to closed mode, rejecting unauthenticated or non-matching registrations and invites with a secure `403 Forbidden` response.

---

### 🟢 Issue #57 (B): Thread-Safe Buffered Logging Under Lock
* **Status**: ✅ Resolved (v1.4.0 / `b5c1a1f`)
* **Labels**: `performance`, `concurrency`

#### Resolution Details
1. Implemented a private `_logQueue` and helper `queueLog()` inside `RequestsHandler` to buffer all `std::cout`/`std::cerr` print statements inside locked critical sections.
2. Configured `handle()`, `tick()`, and `forceDisconnect()` to capture the accumulated logs, clear the queue, release the main mutex `_mutex`, and safely output the logs to the console completely outside of the locked section.

---

### 🟢 Issue #58: Distributed Scanner Memory Exhaustion via Rate-Limit Buckets
* **Status**: ✅ Resolved (v1.4.0 / `b5c1a1f`)
* **Labels**: `bug`, `security`, `dos-prevention`

#### Resolution Details
1. Configured `RequestsHandler::tick()` to periodically sweep rate-limit buckets older than 60 seconds from the `_rateBuckets` map.
2. Added a hard cap of `MAX_BUCKETS = 256` inside `allowPacket()`, falling back to drop additional scanning source IP packets to prevent denial-of-service memory exhaustion.

---

### 🟢 Issue #59: Whole-Message Header Mutations Corrupting SIP Body
* **Status**: ✅ Resolved (v1.4.0 / `b5c1a1f`)
* **Labels**: `bug`, `reliability`

#### Resolution Details
1. Implemented `SipMessage::findHeader()` to calculate the header-to-body boundary (`\r\n\r\n` or `\n\n`) and restrict searches strictly within the `[0, headerLimit)` range.
2. Modified all header setters in `SipMessage.cpp` (`setVia`, `setFrom`, `setTo`, etc.) to use `findHeader()`, protecting identical substrings in the SDP media/audio body from accidental mutations.
