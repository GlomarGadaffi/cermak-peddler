# Issue Tracking & Architectural Roadmap

This document serves as the active issue tracker and architectural roadmap for **pocket-dial**. It tracks high-impact concurrency, performance, and hardware-specific issues identified during production deployments, along with their resolution status.

---

## Active Issues & Backlog Roadmap

### 🟡 Issue #74: Hold/resume on broadcast (ring-group) calls is not yet supported
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

### 🟢 Issue #65: [Feature] Call Park / Park-Orbit
* **Status**: ✅ Shipped (`onParkInvite` + retrieve + ring-back, orbits `700`–`709`)
* **Labels**: `feature`, `sip`, `call-control`
* **Description**: Park an active call to a virtual "orbit" slot (e.g. `700`–`709`): the parking phone dials a park code, its leg is held in an orbit slot, and **any** extension retrieves the call by dialing that orbit number. **Signaling-state only — no server media.** On retrieval the call re-establishes peer-to-peer RTP between the retriever and the held party, so this needs no media bridge. Reuses the existing `Session` pool and the `onInvite` virtual-extension intercept (à la `777`/`999`).
* **Acceptance**: (1) mid-call park → caller held, orbit number announced/shown; (2) dialing the orbit from another extension connects to the held party via fresh P2P SDP O/A; (3) one `Session` slot per parked call (documented against `MAX_SESSIONS`); (4) park timeout rings back the parker; (5) double-retrieve of an orbit returns `486`. Host-tested; a real-hardware smoke-test pass is tracked separately above (`hardware-testing` label).
* **Notes**: Promotes the `FEATURE_ROADMAP.md` P1 "Call parking / park-orbit" item.

### 🟢 Issue #66: [Feature] Paging Zones (multi-zone `999`)
* **Status**: ✅ Shipped (`isPageZoneExt`/`findPageZone`, extensions `980`–`989`)
* **Labels**: `feature`, `sip`, `paging`
* **Description**: Generalize the single all-page (`999`) into named zones (e.g. `981` = floor-1) backed by a zone→members map, reusing the existing `startBroadcastFork` / `huntRingNext` forking path. Bounded zone table + per-zone member cap so message-pool use stays bounded (same discipline as the `999` page). Keeps the current fork-and-answer model — no media mixing.
* **Acceptance**: a zone code pages only that zone's members; membership configurable via the existing ring-group API; per-zone cap enforced; heap returns to baseline after the page (Static Pool recycle, per `BENCHMARKS.md`). Host-tested (`PageZone_test.cpp`).
* **Notes**: Promotes `FEATURE_ROADMAP.md` P2 "Paging zones."

### 🟢 Issue #67: [Feature] BLF / Presence (`SUBSCRIBE`/`NOTIFY`, dialog-info)
* **Status**: ✅ Shipped (`onSubscribe`, RFC 4235 `dialog-info+xml`)
* **Labels**: `feature`, `sip`, `presence`
* **Description**: Let desk-phone busy-lamp-field keys reflect extension/line state the server already tracks (registration + active `Session`). Implement `SUBSCRIBE`/`NOTIFY` for `dialog-info+xml` with a **bounded** subscription table and NOTIFY fan-out on state change.
* **Acceptance**: a phone subscribed to ext X gets NOTIFY on X ringing/answered/idle; subscription table is capacity-capped; expiry refresh handled; no unbounded fan-out. A real-hardware smoke-test pass on a BLF-capable phone is tracked separately above (`hardware-testing` label).
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
pocket-dial's default call path stays **LAN-only, peer-to-peer media** — every feature above is
signaling-only and needs none of the below. **Out of scope, intentionally not implemented here:**
* **Specific commercial telephony-provider connectors** (3CX, or any other named vendor's
  call-control API). `TelephonyProviderRegistry` (`src/SIP/TelephonyProvider.hpp`) is the
  extension point — implement your own `AnchorClient` and register it; pocket-dial ships only
  the `Loopback` reference, and `TelephonyProviderType` names no commercial vendor.
* **PSTN/trunk call-origination policy** — deciding *when* a dialed number routes to an anchor
  instead of a local extension, registering to an upstream trunk, NAT/SBC concerns. That's the
  policy a fork writes alongside its own `AnchorClient`; wiring it into `RequestsHandler`'s call
  routing is out of scope here.

**What DOES ship, opt-in:** `AnchorClient` / `MediaBridge` / `TelephonyProvider` /
`TelephonyApiConfig` (`src/SIP/`) are the vendor-neutral "bones" for bridging a call to an
external audio system — compiled, unit-tested, and ready to extend, but not wired into any call
path by default. `MixBus` (`src/SIP/MixBus.*`, see
[docs/CONFERENCE_MIXER.md](docs/CONFERENCE_MIXER.md)) is a standalone, tested N-way audio mixer,
also not yet wired into `MediaBridge` (tracked: #75 below). **Dialing a number does nothing with
any of this today** — these are building blocks, not a feature you can pick up a phone and use,
until a fork (or a future pass here) adds the routing policy.

---

## Anchored Media (Opt-In, Vendor-Agnostic)

### 🔵 Issue #75: [Feature] Wire MixBus into MediaBridge for local N-way conferencing
* **Status**: ⏳ Open / Planned (Backlog)
* **Labels**: `feature`, `media`, `conferencing`
* **Description**: `MixBus` (the N-way audio summing junction, §1–6 of
  [docs/CONFERENCE_MIXER.md](docs/CONFERENCE_MIXER.md)) is implemented, unit-tested, and
  vendor-neutral, but `MediaBridge` still serves one 1:1 leg per instance — the bus isn't wired
  into the call path. §7 of that doc sketches the diff: `MediaBridge`'s RX/TX callbacks swap to
  `bus.inputFrame`/`outputFrame`, `startBridge`/`stopBridge` gain `bus.attach`/`detach`, and a
  single periodic tick driver replaces nothing else. This is generic LAN-conferencing
  functionality (no anchor/vendor required — handset legs alone are enough ports), distinct from
  and not blocked on the anchor call-routing policy noted in Non-Goals above.
* **Acceptance**: 3+ registered extensions can be bridged onto one `MixBus` instance (via
  whatever dial mechanism lands with this issue — e.g. a conference extension/star-code, design
  TBD); each hears the sum of the others, minus itself; a leg leaving doesn't disturb the rest.
* **Notes**: Scalar kernel cost is ~64k adds/s at ≤8 narrowband ports — negligible on a 240 MHz
  core (see CONFERENCE_MIXER.md §4). Vectorising (the PIE kernels, `src/SIP/pie/`) stays opt-in
  behind `POCKETDIAL_MIXBUS_PIE` until profiling says it's needed.

---

## Resolved Issues

### 🟢 Issue #94: `docs/API.md` endpoint catalog is stale
* **Status**: ✅ Resolved
* **Labels**: `documentation`

#### Resolution
Cross-checked `docs/API.md`'s §4 catalog against the actual route dispatch in `src/Helpers/HttpServer.cpp::handleClient()` (grepped every `req.path ==` / request-line match, including `/api/ota/upload`'s special-cased streaming interception and `/config/<mac>.cfg`'s prefix match). Every route the server actually serves now has a full per-endpoint spec: `/`, `/api/status`, `/api/kill`, `/api/cdr`, `/api/pcap`, `/api/trace`, `/config/<mac>.cfg`, `/api/dnd`, `/api/forward`, `/api/group`, `/api/wifi/scan`, `/api/wifi/connect`, `/api/wifi/mode_ap`, `/api/configuring`, `/api/factory-reset`, `/api/ota/status`, `/api/ota/upload`, `/api/ota/reboot` — all present with method, auth requirements, request params, response codes, and example payloads. The admin session endpoints (`/api/admin/*`) are covered in §0 by design (the doc's own note explains why they're not repeated in §4), not a gap. The partial-catalog banner this issue referenced is already gone.

This was done by an earlier commit (`3a65c13 docs(api): fill in the stale endpoint catalog`, plus later additions as `/api/pcap`/`/api/trace`/`/config/<mac>.cfg` shipped) — `ISSUES.md` just hadn't been updated to reflect it. No doc changes were needed here beyond closing out the tracker entry.

---

### 🟢 Issue #93: Already-provisioned admin PINs beginning `4887` are shadowed by the `*4887` HTTP-open star-code
* **Status**: ✅ Resolved (to its practical ceiling)
* **Labels**: `security`, `dtmf`

#### Resolution
`*4887` is matched incrementally in `onDtmfInfo` before the `*PIN#code` parser, so a PIN beginning with those four digits can never complete a DTMF admin command — the star-code fires mid-entry and clears the accumulator. The PIN is stored salted+hashed, so a boot-time scan for already-provisioned collisions is fundamentally impossible; that residual is accepted, not fixable, and stays true regardless of anything else here. What the issue asked for is everything short of that impossible scan, and both pieces are now in place and verified:

1. **Guard new/changed PINs.** `POST /api/admin/set-pin` rejects any PIN starting with `4887` (`AdminHttpGate.SetPin_RejectsReservedStarCodePrefix`), so the collision can't be freshly created going forward.
2. **Behavioral warning for the existing-device case.** `onDtmfInfo` detects the shape of an interrupted `*PIN#code` entry immediately after `*4887` fires (`accum.starCodeFiredAtTick`) and logs a targeted warning naming the `4887` collision and pointing at `POST /api/admin/set-pin` to rotate the PIN — one warning per incident, verified by `AdminHttpGate.Trigger_ContinuedEntryAfterStarCodeLogsShadowedPinWarning` (warns) and `AdminHttpGate.Trigger_PlainStarCodeWithNoContinuationDoesNotWarn` (doesn't false-positive on an ordinary `*4887`-only open).

Both are also documented outside the code: `CHANGELOG.md` records the guard and the detection, and `docs/THREAT_MODEL.md` §E-4/residuals carries the full writeup (mitigation, false-positive/negative shape, and the rotate-your-PIN remediation). Verified via the full host suite (168/168, including the three tests above) — no code change was needed, only closing out `ISSUES.md` to match what had already shipped.

---

### 🟢 Issue #77: DTMF CLASS codes bypass `setDnd()`/`setForward()`, dashboard goes stale
* **Status**: ✅ Resolved
* **Labels**: `concurrency`, `dashboard`, `tech-debt`

#### Resolution
`onDtmfInfo` runs inside `handle()`'s `_mutex` lock (non-recursive). `setDnd()`/`setForward()` each independently took that same `_mutex`, so `onDtmfInfo` couldn't call them without deadlocking — and didn't. `*60`/`*80` wrote `_dnd` directly, and `*73`/`*72NNNN` wrote `_forwards` directly, bypassing the setters' dashboard-snapshot refresh entirely: `_snapshot.dnd`/`_snapshot.forwards` (what the HTTP dashboard's status endpoint actually reads) were only ever refreshed inside `setDnd()`/`setForward()`, so a DTMF-triggered DND/forward change never appeared on the dashboard until an unrelated HTTP-side call happened to touch the same extension.

Fixed with the lock-already-held internal setter variants the issue called for, not a recursive mutex: `setDndLocked()`/`setForwardLocked()` now hold the mutation + snapshot-refresh body that used to live directly inside `setDnd()`/`setForward()`. The public setters take `_mutex` and call the `*Locked` core; `onDtmfInfo`'s `*60`/`*80`/`*73`/`*72NNNN` call the same `*Locked` core directly, since they're already inside `handle()`'s `_mutex`. One code path, one snapshot refresh, for both the HTTP and DTMF trigger.

This also closed the `*72NNNN` gap noted in the issue: `setForward()`'s guard rejecting `777`/`999` as the extension being configured now applies to the DTMF path too, since `*72NNNN`/`*73` route through `setForwardLocked()` instead of touching `_forwards` inline — a crafted mid-dialog INFO with `From: 777` can no longer set up a forward entry on the virtual echo extension (regression test: `DtmfClassCodes.Star72NNNN_RejectsVirtualExtensionAsTheConfiguredExtension`).

New regression coverage (`tests/DtmfClassCodes_test.cpp`, 5 tests) drives real DTMF INFO packets through `handle()` and asserts against `getDndExtensions()`/`getForwards()` — the same snapshot-reading getters the dashboard uses — not the internal maps, so the tests fail the way the original bug actually manifested (stale dashboard) rather than passing against the live-map state that was never the problem. Full host suite: 168/168 passing.

**Not changed, flagged for a deliberate decision:** the issue also asked to confirm whether `*PIN#999`'s DTMF factory-reset diverging from the HTTP factory-reset is intentional. Confirmed it's a real, non-trivial divergence, not a documentation gap: the DTMF path calls `nvs_flash_erase()` — a full NVS partition wipe (PBX config, CDR history, adopted-device registry, per-extension SIP secrets, everything) — while `HttpServer::sendApiFactoryReset` only erases `wifi_mode`/`wifi_ssid`/`wifi_pass`/`decayed` plus the admin credential via `AdminAuth::clearCredential()` (also scoped — salt/hash + session state only). One is a hard wipe, the other a soft network/credential reset, under the same "factory reset" name reached by two different triggers. Left unchanged here since picking a behavior is a product/security decision (how much a physical-access DTMF factory-reset should destroy) rather than a bug fix; the two paths' actual scope is now documented precisely enough for that decision to be made deliberately rather than by omission.

---

### 🟢 Issue #76: `SipMessage` representation — parse-mutate-reparse on every setter
* **Status**: ✅ Resolved
* **Labels**: `performance`, `sip-core`, `tech-debt`

#### Resolution
The `_messageStr`-plus-full-reparse representation this issue described no longer matches the code — `src/SIP/SipMessage.cpp` was already rewritten (`dfeecba`, cppcheck follow-up in `146f63b`) to own its parsed pieces directly: a `_startLine` string, an ordered `_headerLines` vector, and a `_body` string, no shared buffer. Setters (`setVia`/`setTo`/`setContact`/`setCSeq`/...) do a single by-name lookup-and-replace against `_headerLines`; there is no cached derived state to keep in sync, so nothing needs a reparse after a mutation. `reparse()` is gone from both `SipMessage` and `SipSdpMessage`.

Auditing this turned up one real residual: every response-building call site in `RequestsHandler.cpp` (51 of them) cloned a message into the static pool via `getMessageFromPool(source->toString(), source->getSource())` — serializing the already-parsed source back into one wire string and then re-splitting that string, even though `SipMessage`'s copy assignment (`= default`, added by the same rewrite specifically so messages could be cloned cheaply) is already a plain owned-string/vector copy with nothing to reconstruct. Added a `getMessageFromPool(const SipMessage&)` overload that copies the parsed fields directly (`*msg = source`, no stringify/resplit) and switched all 51 sites to it. The 4 call sites that build a message from scratch (`ss.str()`, no prior `SipMessage` to clone) were left untouched — there's no redundant reparse to remove there. Verified with the full host test suite (163/163 passing, host build) and a live REGISTER + echo-call (777) smoke test against the built `SipServer` binary (no `SIP Message pool exhausted` fallback observed under a 10-client registration + 5-concurrent-call load).

Also corrected `docs/REALITY_CHECK.md`'s stale "transient stack-to-heap cloning of `SipMessage` objects remains" note (§2 of the scorecard): that had already been resolved separately by the static `_messagePool` (`RequestsHandler::getMessageFromPool()`), which every response-building call site already used before this change; only the wasted reparse on top of it remained, fixed above.

---

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
