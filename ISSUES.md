# Issue Tracking & Architectural Roadmap

This document serves as the active issue tracker and architectural roadmap for **pocket-dial**. It tracks high-impact concurrency, performance, and hardware-specific issues identified during production deployments, along with their resolution status.

---

## Active Issues & Backlog Roadmap

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

## API Integration: 3CX Call Control Connector (Epic)

> **Goal**: let a pocket-dial / tincan handset place calls to **3CX extensions** over WAN by bridging pocket-dial's SIP/RTP world to 3CX's HTTP-based **Call Control API**.
>
> **Why no SBC / NAT / STUN / TURN / ICE is needed**: the connector *terminates the handset's RTP locally* (one SIP hop, on-box) and re-originates the call into 3CX over **HTTPS, not SIP/RTP** — so there is no second SIP/RTP peer for ICE to traverse. Both legs are 8 kHz, so media is a pure companding swap (G.711 ⇄ PCM16) with **no resampling and no media server**. Over WAN this only requires a flat L3 (WireGuard/overlay or public IP) so the connector's advertised SDP address is directly reachable.

### Architecture Decision
The connector is a **media-terminating SIP endpoint** that `REGISTER`s to pocketdial-desktop as an ordinary extension (e.g. `3000`, or a dialing prefix). pocket-dial's **existing** `onInvite` forward path (`RequestsHandler.cpp`) delivers the INVITE to it with **zero changes to the signaling-only server** — the registrar keeps sourcing no media. The connector mirrors the **`440` `RtpSender` beachhead**, except it:
* answers `200 OK` with its **own** SDP carrying a real media address and `a=sendrecv` (the `440` path uses the server media port but is send-only/tone),
* bridges audio to 3CX instead of synthesizing a tone,
* maps SIP `BYE`/`CANCEL` ↔ 3CX participant `drop`.

**Open fork (must decide — gates the language of all sub-issues below):**
* **(A) C++ connector binary in this repo** — new target reusing `SipMessage`/`SipSdpMessage`; needs a TLS HTTP/WS client (libcurl) for 3CX. One repo, one deploy.
* **(B) Lean Node sidecar** — separate process on the Pi, registers as an extension; trivial 3CX HTTPS/WSS, no TLS-dep friction; pocket-dial untouched.

### Reference: 3CX Call Control API (verified)
* **License**: 3CX **Enterprise + CFD**; an API Client scoped to **Call Control Access**; a **Route Point DN** (`type Wroutepoint`) for origination.
* **Auth**: OAuth2 `client_credentials` → `POST https://{fqdn}/connect/token` → Bearer. ⚠️ `expires_in` misreports ~60 s for a ~1 h grant — **trust the JWT `exp` claim**, and do **not** refresh early (a new token invalidates the one a live media stream is holding).
* **Two planes** — don't confuse them: `/xapi/v1` (a.k.a. "Configuration REST API", OData, config/management only) vs **`/callcontrol`** (calls + media). This connector is entirely `/callcontrol`.
* **Originate**: `POST /callcontrol/{dn}/makecall` (or `/callcontrol/{dn}/devices/{deviceId}/makecall` for a deterministic participant id under concurrency).
* **Participant control**: `POST /callcontrol/{dn}/participants/{id}/{drop|answer|divert|routeto|transferto}`.
* **Audio (bidirectional)**: `GET /callcontrol/{dn}/participants/{id}/stream` receives the party's audio; `POST` to the same path **injects** audio. Format both ways: **PCM 16-bit, 8 kHz, mono**, HTTP chunked octet-stream.
* **Events**: `wss://{fqdn}/callcontrol/ws` (participant-updated, DTMF, etc.), each event carrying an `entity` path to `GET`. Polling `participants` at ~300 ms is a proven fallback.
* **Codec alignment**: 3CX PCM16 @ 8 kHz ⇄ G.711 @ 8 kHz is a 256-entry lookup-table companding conversion — same rate, **no resampling**. 20 ms / 160-sample framing matches `RtpSender`.

### Foundations (✅ Completed — the enabling base, branch `2.0`)
* 🟢 **`RtpSender` media beachhead** (`119ca84`): first server-sourced RTP path — virtual ext **`440`** streams a one-way G.711 µ-law tone (PCMU PT0, 8 kHz, 20 ms) to the caller, answering with the server's **own** SDP (media port `5062`). Pure helpers `linearToUlaw` / `synthTone` / `buildRtpHeader` are platform-independent and host-unit-tested (`tests/Rtp_test.cpp`); the real socket + 20 ms FreeRTOS pacing task are ESP-only; single-stream cap (2nd dial → `486`).
* 🟢 **Media crashloop fixes** (`b7e82d5`): `udp_receiver_task` 8→16 KB and `rtp_media_tx` 4→6 KB stacks (the new SDP/UAC chain overflowed them), plus an `HttpServer::acceptLoop` guard for `std::thread`-spawn `std::system_error` (uncaught throw was rebooting the device).

### 🔵 Issue #60: 3CX Connector — Form-Factor Decision (C++ in-repo vs Node sidecar)
* **Status**: ⏳ Open / **Blocking** (gates #61–#64)
* **Labels**: `api-integration`, `architecture`, `decision`
* **Description**: Choose option **(A)** or **(B)** above. Determines language, dependency surface (libcurl vs none), and whether the connector ships inside this repo or beside it on the Pi.

### 🔵 Issue #61: RTP Receive Path + µ-law→PCM16 Decode (uplink)
* **Status**: ⏳ Open / Planned
* **Labels**: `api-integration`, `media`, `rtp`
* **Description**: Add the inverse of `RtpSender` — receive the handset's RTP on the media socket, strip the RTP header, decode G.711 µ-law→PCM16 (inverse of `linearToUlaw`), and feed it to the 3CX `POST /stream`. Needs a small jitter/ordering tolerance (the `440` sender path has none).

### 🔵 Issue #62: Real Desktop (host) Media Transport
* **Status**: ⏳ Open / Planned
* **Labels**: `api-integration`, `media`, `desktop`
* **Description**: `RtpSender`'s socket + pacing are `#if ESP_PLATFORM`; on the desktop build they are **no-op stubs** that only flip `_active`. For the connector to move audio on the Pi (`SipServer.exe`), implement the real host (Linux/Windows) UDP socket + 20 ms pacing loop alongside the ESP path.

### 🔵 Issue #63: 3CX Call Control Client (token + makecall + /stream + events)
* **Status**: ⏳ Open / Planned
* **Labels**: `api-integration`, `3cx`, `http`
* **Description**: Implement the 3CX leg per the Reference above — JWT-`exp` token lifecycle, `makecall` to the target extension, concurrent `GET`/`POST /stream` (chunked), and `wss /callcontrol/ws` (or 300 ms participant polling) to detect `Connected` and far-end hangup. (libcurl if option A.)

### 🔵 Issue #64: Bridge Orchestration (virtual-ext intercept + leg mapping)
* **Status**: ⏳ Open / Planned
* **Labels**: `api-integration`, `sip`, `media`
* **Description**: Wire it together — a virtual extension / prefix (`777`/`999`/`440`-style intercept in `onInvite`) hands the call to the connector, which answers with its own `a=sendrecv` SDP (real media addr), opens both 3CX streams, and replaces the `synthTone` frame source with audio pulled from 3CX. Map `INVITE`→`makecall`, `BYE`/`CANCEL`→participant `drop`, and 3CX far-end hangup→SIP `BYE`. Honor the no-ICE addressing rule (SDP `c=` must be a handset-reachable IP).

---

## Feature-Parity Backlog (LAN PBX)

Generic desk-PBX features to bring pocket-dial to parity with full-size PBXes. **All of these are
designed to need _no_ server-side media path** — media stays peer-to-peer; the server only brokers
signaling and tracks state. See **Non-Goals** below for the hard scope boundary.

### 🔵 Issue #65: [Feature] Call Park / Park-Orbit
* **Status**: ⏳ Open / Planned (Backlog)
* **Labels**: `feature`, `sip`, `call-control`
* **Description**: Park an active call to a virtual "orbit" slot (e.g. `700`–`709`): the parking phone dials a park code, its leg is held in an orbit slot, and **any** extension retrieves the call by dialing that orbit number. **Signaling-state only — no server media.** On retrieval the call re-establishes peer-to-peer RTP between the retriever and the held party, so this needs no media bridge. Reuses the existing `Session` pool and the `onInvite` virtual-extension intercept (à la `777`/`999`).
* **Acceptance**: (1) mid-call park → caller held, orbit number announced/shown; (2) dialing the orbit from another extension connects to the held party via fresh P2P SDP O/A; (3) one `Session` slot per parked call (documented against `MAX_SESSIONS`); (4) park timeout rings back the parker; (5) double-retrieve of an orbit returns `486`.
* **Notes**: Promotes the `FEATURE_ROADMAP.md` P1 "Call parking / park-orbit" item.

### 🔵 Issue #66: [Feature] Paging Zones (multi-zone `999`)
* **Status**: ⏳ Open / Planned (Backlog)
* **Labels**: `feature`, `sip`, `paging`
* **Description**: Generalize the single all-page (`999`) into named zones (e.g. `981` = floor-1) backed by a zone→members map, reusing the existing `startBroadcastFork` / `huntRingNext` forking path. Bounded zone table + per-zone member cap so message-pool use stays bounded (same discipline as the `999` page). Keeps the current fork-and-answer model — no media mixing.
* **Acceptance**: a zone code pages only that zone's members; membership configurable via the existing ring-group API; per-zone cap enforced; heap returns to baseline after the page (Static Pool recycle, per `BENCHMARKS.md`).
* **Notes**: Promotes `FEATURE_ROADMAP.md` P2 "Paging zones."

### 🔵 Issue #67: [Feature] BLF / Presence (`SUBSCRIBE`/`NOTIFY`, dialog-info)
* **Status**: ⏳ Open / Planned (Backlog)
* **Labels**: `feature`, `sip`, `presence`
* **Description**: Let desk-phone busy-lamp-field keys reflect extension/line state the server already tracks (registration + active `Session`). Implement `SUBSCRIBE`/`NOTIFY` for `dialog-info+xml` with a **bounded** subscription table and NOTIFY fan-out on state change.
* **Acceptance**: a phone subscribed to ext X gets NOTIFY on X ringing/answered/idle; subscription table is capacity-capped; expiry refresh handled; no unbounded fan-out.
* **Notes**: Promotes `FEATURE_ROADMAP.md` P2 "BLF / presence." Pairs with provisioning (BLF keys are provisionable).

### 🔵 Issue #68: [Feature] Directed Call Pickup (pickup groups)
* **Status**: ⏳ Open / Planned (Backlog)
* **Labels**: `feature`, `sip`, `call-control`
* **Description**: Answer a ringing peer's call from your own phone via a pickup code (`*8` group pickup or `**<ext>` directed). Reuses the fork/`Session` machinery and the registrar's view of in-progress INVITEs; the call lands P2P on the picker — no server media.
* **Acceptance**: while ext A rings, ext B dials the pickup code and connects to the caller with P2P SDP; only same-pickup-group calls eligible; race with A answering resolves cleanly (one wins, other `486`/cancel).

### 🔵 Issue #69: [Feature] Dial-Plan / Hunt-Group Generalization
* **Status**: ⏳ Open / Planned (Backlog)
* **Labels**: `feature`, `sip`, `routing`
* **Description**: Extend the existing `ringall`/`hunt` ring groups (`setRingGroup`/`getRingGroups`) into a small **pattern → action** dial plan for LAN routing (match an extension/prefix → group ring / hunt / page zone / park orbit / pickup). Table-driven, bounded, LAN-only.
* **Acceptance**: a configurable rule table maps dialed patterns to existing actions, evaluated in order; bounded table size.
* **Notes**: Promotes `FEATURE_ROADMAP.md` "Dial plan / hunt groups."

### ⚪ Non-Goals (hard scope boundary)
pocket-dial stays a **LAN-only, peer-to-peer-media PBX**. The following are **out of scope** and intentionally not implemented here — every feature above is designed to need none of them:
* **Server-side media bridging / mixing / relay** (any "media anchor" that terminates and relays or mixes RTP). All media stays phone↔phone.
* **Upstream trunk / PSTN integration** and **third-party telephony-provider connectors**. pocket-dial does not originate or terminate external trunks.

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
