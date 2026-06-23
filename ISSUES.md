# Issue Tracking & Architectural Roadmap

This document serves as the active issue tracker and architectural roadmap for **pocket-dial**. It tracks high-impact concurrency, performance, and hardware-specific issues identified during production deployments, along with their resolution status.

---

## Active Issues & Backlog Roadmap

### 🟡 Issue #70: Hold/resume on broadcast (ring-group) calls is not yet supported
* **Status**: ⏳ Open / Planned
* **Labels**: `bug`, `hold-resume`, `broadcast`
* **Severity**: Medium

#### Description
When a phone in a ring-group (or 999 broadcast) call sends a re-INVITE to hold, the
200 OK answer from the peer is silently discarded. The hold relay block in `onOk` is
correctly guarded by `!isBroadcast()` to avoid looping, but there is no equivalent
broadcast-aware relay path. As a result the phone's hold request is never forwarded, the
re-INVITE transaction times out after 32 s (Timer D), and the phone treats hold as
failed. Unicast hold/resume is unaffected.

**Root cause (found by code review):** `onOk`'s broadcast first-answer block was guarded
by `state != Connected`, which is also true for `Held` — so a re-INVITE 200 OK re-ran
the connect path, overwrote the established dest, and sent CANCEL to already-gone targets.
Fixed in this release: the guard now requires `state == Invited` so only a genuine first
answer triggers the connect path. The 200 OK is now discarded (better than the previous
destructive behaviour). A full relay path for broadcast hold/resume is tracked here.

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

### 🟢 Issue #73: `Held` state CDR-logged as Failed with zero duration
* **Status**: ✅ Resolved (sip-backport)
* **Labels**: `bug`, `cdr`, `hold-resume`

#### Resolution
`recordCdr()` switch did not handle `Session::State::Held`; it fell to `default: Failed`
with `durationSec = 0`. Any call torn down while on hold (e.g. session-timer expiry,
`sweepSessionTimers`) produced a zero-duration Failed CDR record even for calls that had
been answered and talked for minutes. Fixed by adding `Held` to the `Connected`/`Bye`
`CdrResult::Answered` case, which also computes talk time from `_startTime` (preserved
across hold/resume by the `prev == Held` guard in `Session::setState`).

---

### 🟢 Issue #72: `sweepSessionTimers` sends malformed BYE when dialog-To header is empty
* **Status**: ✅ Resolved (sip-backport)
* **Labels**: `bug`, `session-timers`, `sip`

#### Resolution
Both BYE guards in `sweepSessionTimers` checked only `!dFrom.empty()`. If `dTo` was
empty (possible when `armSessionTimer` was called from `onReinvite` before dialog headers
were fully captured by `onOk`), `buildServerBye` received an empty `toHeader`, producing
a `To: \r\n` line that phones drop as malformed. The session then re-fired malformed BYEs
on every sweep tick without ever freeing the slot. Fixed by requiring `!dTo.empty()` on
both guards before building either BYE.

---

### 🟢 Issue #71: `onParkInvite` retrieve path cleared the park slot before checking session-pool availability
* **Status**: ✅ Resolved (sip-backport)
* **Labels**: `bug`, `parking`, `reliability`

#### Resolution
The retrieve path sent the re-INVITE to the parked party and pushed the Call-ID to
`_parkPendingAcks` before calling `allocateSession` for the retriever. If the session
pool was exhausted, `allocateSession` returned nullptr and the `slot = ParkSlot{}` clear
ran unconditionally, leaving a live untracked parked dialog: the parked phone answered the
re-INVITE, an ACK was sent, but there was no retriever session, so all subsequent BYEs
returned 481 and CDR was never recorded. Fixed by allocating the retriever session first;
on failure a `503 Service Unavailable` is returned and the slot is left intact. The 200 OK
and re-INVITE are sent only after a successful allocation.

---

### 🟢 Issue #70: tick()-originated INVITE forks had no RFC 3261 §17 retransmit coverage
* **Status**: ✅ Resolved (sip-backport)
* **Labels**: `bug`, `transaction-layer`, `reliability`

#### Resolution
The `registerTx` outbox scan (which registers outgoing INVITEs for Timer A/B retransmit)
existed only in `handle()`. `tick()` populates `_outbox` with new INVITEs via
`parkSweep()` → `startParkRingback()`, `huntRingNext()` → `buildInviteFork()`, and
`redirectInvite()` → `buildInviteFork()`, but had no equivalent scan — all three paths
sent INVITEs fire-and-forget with no retransmit. On a lossy link (Wi-Fi, cross-VLAN)
these silently fail: park ring-back never reaches the parker, CFNA redirect never arrives,
hunt-group next-ring never rings the next member. Fixed by adding the same `classifyTxType`
/ `registerTx` scan loop in `tick()` before the outbox drain.

---

### 🟢 Issue #69b (fix): Broadcast re-INVITE hold re-triggered the first-answer connect path
* **Status**: ✅ Resolved (sip-backport, part of #69 fix)
* **Labels**: `bug`, `hold-resume`, `broadcast`

#### Resolution
`onOk`'s broadcast first-answer block was guarded by
`session.value()->getState() != Session::State::Connected`. After `onReinvite()` set state
to `Held`, the condition was true and the block executed: it overwrote the established
dest with `setDest(answeringClient)`, forced state back to `Connected`, and sent CANCEL to
already-gone pending targets. The hold 200 OK was forwarded as if it were a fresh call
answer; no ACK was sent to the answering phone; Timer D fired 32 s later. Fixed by
changing the guard to `state == Session::State::Invited` so the first-answer path only
fires for a genuine new answer from a pending fork.

---

### 🟢 Issue #68b (fix): `sendParkReinvite` emitted Call-ID without mandatory header name
* **Status**: ✅ Resolved (sip-backport, found during #68 code review)
* **Labels**: `bug`, `parking`, `sip`

#### Resolution
The retrieve re-INVITE assembled by `sendParkReinvite` emitted `slot.callID` as a bare
line with no `"Call-ID: "` label (e.g. `abc123@192.168.1.1\r\n`), violating RFC 3261
§20.8 which requires Call-ID in every request. The parked phone received an invalid SIP
request and rejected it with 400 or dropped it silently, stranding the parked dialog even
though the retriever had already been answered with 200 OK. Fixed by prepending `"Call-ID: "`.

---

### 🟢 Issue #67b (fix): `getPrimaryLocalIP()` called inside `_mutex` across 7 new functions
* **Status**: ✅ Resolved (sip-backport, found during #67 code review)
* **Labels**: `bug`, `performance`, `concurrency`

#### Resolution
`onReinvite`, `onUpdate`, `buildDialogNotify`, `sendParkReinvite`, `startParkRingback`,
`handleParkOk`, and `handleTransferOk` all called `getPrimaryLocalIP()` while holding
`_mutex` inside `handle()` or `tick()`. `getPrimaryLocalIP()` performs a
`socket`/`connect`/`getsockname`/`close` syscall chain (plus `WSAStartup`/`WSACleanup`
on Windows), violating CLAUDE.md's "no blocking I/O under the registrar lock" rule. The
same functions also constructed `std::ostringstream` objects (heap allocation) in the
hot path, violating the "zero heap in the packet hot path" rule. Fixed by resolving the
local IP once at construction time into `_localIp` and replacing all
`(_serverIp == "0.0.0.0") ? getPrimaryLocalIP() : _serverIp` expressions with `_localIp`
throughout `RequestsHandler.cpp`.

---

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
