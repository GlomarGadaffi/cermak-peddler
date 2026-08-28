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

### 🟡 Issue #112: Pin cppcheck in CI: version floats with ubuntu-latest, and 2.21 already finds 4 issues 2.13 doesn't
* **Status**: 🟡 Partially resolved 2026-08-19 — all findings suppressed/fixed and verified against a real 2.21.0 build, but the `ci.yml` pin itself couldn't be pushed this session (missing `workflow` OAuth scope — see below) and needs a follow-up push
* **Labels**: `cleanup`

#### Resolution
Wrote and verified the CI workflow change that pins the host job's cppcheck to **2.21.0**, built from its own tagged source (no official Linux binary, and pip's `cppcheck` package is an unrelated 1.5.1-vintage wrapper) rather than the `apt` package that floats with `ubuntu-latest`, cached by version via `actions/cache` — but this session's `gh` token lacks the `workflow` scope GitHub requires to push a `.github/workflows/*` change, so that diff could not be pushed to the PR branch. It sits, committed, on the local `backup-with-ci-workflow-change` branch and needs someone with that scope to push it or cherry-pick it onto the PR. Everything the pin would newly surface is already fixed/suppressed in source (below) and merged into the PR, so applying the pin once pushed should be a no-op against a clean CI run — it is not blocked on anything further.

Before pinning, the issue's claimed 4 findings were verified rather than trusted — both 2.13.0 and 2.21.0 were built from source and run twice: once cross-compiled on Windows (surfaced a discrepancy), and once on a real Linux build via WSL Debian (gcc 14) to confirm it wasn't a Windows-build artifact. On Linux: 2.13.0 finds **zero** issues on `main`; 2.21.0 finds **13**, across six categories. The other nine beyond the issue's four are the identical false-positive shape as its own `SipStatus.cpp` example (plain aggregates now flagged per-scalar-member even though every construction site brace-initializes or immediately overwrites the field), plus a third, previously-unreported `ESP_IDF_VERSION_VAL` `syntaxError` site inside `RequestsHandler.cpp`, and one `performance` suggestion. All thirteen resolved — pinning without addressing all of them would have made the pin itself the next red-CI surprise:

- The three `syntaxError` sites (`main/esp_main_eth.cpp:36`, `main/esp_main_eth_lan8720.cpp:43`, `src/SIP/RequestsHandler.cpp:3734`, all `#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(...)`) were investigated as the issue asked ("the dangerous one") rather than blind-suppressed: confirmed not a real parse defect — a genuine `idf.py` build resolves `esp_idf_version.h` fine; the host lint job's include path just has no ESP-IDF tree on it. Fixed with inline `// cppcheck-suppress syntaxError` at each site (verified with a standalone repro that this scopes to just that line, not the whole file) rather than the issue's suggested whole-file `--suppress=syntaxError:<path>`, which on `RequestsHandler.cpp` — the codebase's largest, most-frequently-changed file — would have silenced any future real syntax error there too.
- `SipStatus.cpp` (both its `code` and `softFail` fields — cppcheck flags each scalar member individually), `DnsServer.cpp`, `PcapCapture.hpp` (×6), `RequestsHandler.hpp` (×2) got inline `// cppcheck-suppress` comments with per-site false-positive rationale.
- `RequestsHandler.hpp`'s `getAdminExt()` performance suggestion (return by reference) was deliberately **declined**: `_adminExt` is mutated from call paths that don't share a lock with this getter, so returning a reference would let it dangle under a concurrent write — worse, not better.

Verified clean (`EXIT=0`, zero findings) with the exact CI invocation on both a Windows cross-build and a real WSL/Linux build of cppcheck 2.21.0. Full detail: https://github.com/GlomarGadaffi/pocket-dial/issues/112

---

### 🟢 Issue #108: RtpSender Linux: no destructor join for the sender thread; SSRC/seq seeded from std::rand
* **Status**: ✅ Resolved 2026-08-19 — SSRC/seq fixed; destructor was already correct on `main`
* **Labels**: `bug`, `desktop`

#### Resolution
Two items:

1. **SSRC/sequence/timestamp seeded from `std::rand()`.** Fixed: the host/Linux `rand32()` now uses a `thread_local std::mt19937` seeded from `std::random_device`, rather than a single process-wide `std::rand()` stream shared and correlated across every caller. The ESP branch (`esp_random()`, the hardware RNG) was already correct and is unchanged.
2. **No destructor join.** Investigated and found **already correct** on `main` — the issue's "no destructor is visible in the patch" referred to commit `af5fd24` (the Linux media path) not adding one, but `~RtpSender()` predates that commit (added with the original media feature, `119ca84`) and its non-ESP branch already calls `stop("")` unconditionally, which on Linux sets `_stopRequested` and joins `_senderThread` synchronously before returning. No joinable thread is ever left running when the object is destroyed. Only the destructor's stale comment (still calling it a "host stub" after `af5fd24` gave that code path a real Linux thread) was updated — no behavior change. Verified by building and running the host suite under WSL/Linux (the real `__linux__` thread path, not the Windows host-stub no-op) with a new test that starts a stream and lets the object destruct without calling `stop()` first: completes cleanly, no `std::terminate()`.

New/updated tests: `tests/Rtp_test.cpp` (`DestructorStopsActiveStreamWithoutCrash`). Full detail: https://github.com/GlomarGadaffi/pocket-dial/issues/108

---

### 🟢 Issue #106: BLF forEachSessionInvolving else-if drops the Callee leg when an extension calls itself
* **Status**: ✅ Resolved 2026-08-19
* **Labels**: `bug`

#### Resolution
`RequestsHandler::forEachSessionInvolving()` used an `if (isSrc) ... else if (isDest) ...` chain (a regression from commit `f0446d9`'s refactor), so a session whose src *and* dest are both the watched extension — a self-call, or a hunt/ring group where the caller is also a member — only ever reported the Caller role; the Callee leg was silently dropped from `BlfSubscriptions`' dialog-info XML. Restored to two independent `if`s so both roles fire when both match, as the pre-`f0446d9` visitor did.

`tests/FakePbxEnv.hpp`'s `forEachSessionInvolving()` — the test double every BLF/beeper/park host test drives, since the production method is a private `PbxEnv` override and not directly host-testable — carried the identical bug and was fixed identically. Covered by two new tests in `tests/BlfSubscriptions_test.cpp`: one asserts the callback fires twice (once per role) for a self-call session, the other confirms `BlfSubscriptions::computeDialogState` reports the higher-ranked Callee leg rather than getting stuck on the Caller leg for a ringing self-call. Full detail: https://github.com/GlomarGadaffi/pocket-dial/issues/106

---

### 🟢 Issue #104: RegisterBeeper::sweep logs 'cancelled' and enters AwaitingCancelDone even when buildCancel fails
* **Status**: ✅ Resolved 2026-08-19
* **Labels**: `bug`

#### Resolution
Confirmed as a live bug via real-hardware evidence in the issue's comment thread (a LilyGO T-ETH-Elite S3 logging "cancelled" for a beep dialog in the same breath as message-pool exhaustion). `sweep()`'s `AwaitingInviteOk` branch unconditionally logged "cancelled" and advanced to `AwaitingCancelDone` even when `buildCancel()` returned `nullptr` under pool pressure — misrepresenting what happened, and leaving the dialog in a state that claims a CANCEL is in flight when none was sent.

Fixed: on `buildCancel()` failure the dialog stays in `AwaitingInviteOk` with a short retry deadline and a distinct "cancel build failed — will retry" log; the next `sweep()` tick retries, succeeding once pool pressure eases. Per the issue's own caution against getting stuck forever either, added a bounded retry count (`BeepDialog::cancelRetries`, capped at 5) — after five consecutive failures `sweep()` gives up and frees the slot rather than retrying indefinitely under sustained exhaustion.

Covered by two new tests in `tests/RegisterBeeper_test.cpp` (a new `FakePbxEnv::messagePoolAvailable` toggle forces `buildCancel()` to fail, matching the existing `sessionPoolAvailable` pattern): one confirms the retry-then-succeed path, the other confirms the bounded give-up-and-free path under sustained pressure. Full detail: https://github.com/GlomarGadaffi/pocket-dial/issues/104
### 🟢 Issue #105: `/api/pcap` inbound capture stores re-serialized SIP, not wire bytes
* **Status**: ✅ Resolved 2026-08-19
* **Labels**: `bug`, `network`

#### Description
The inbound `/api/pcap`/`/api/trace` capture point in `RequestsHandler::handle()` recorded `request->toString()` — a re-serialization of the **parsed** message, not the bytes `recvfrom()` actually delivered. `SipMessage::toString()` always writes CRLF line endings and rejoins the parsed `_startLine`/`_headerLines`/`_body`; it does not restore whatever line-ending convention or malformed-but-tolerated layout the wire bytes actually used. Whitespace, compact-header forms (`v:`/`f:`/`t:`/`i:`), CRLF-vs-LF tolerance, or any malformed-but-tolerated line the SEC-02 parser normalized (a blank line inside the header block is silently dropped) was lost — for a signaling-debugging tool this is a fidelity bug pointed in the worst direction: the more unusual or hostile the input, the more the capture diverges from what actually arrived. The outbound side was already correct (server-minted messages, so `toString()` genuinely is the wire bytes there).

#### Resolution
`RequestsHandler::handle()` now takes an optional `std::string_view rawBytes` and writes it verbatim into the pcap ring slot when the caller supplies it, falling back to the old `toString()` capture otherwise (every pre-existing `handle(msg)` call site, all in `tests/`, keeps compiling and keeps its old behavior unchanged). `SipServer::onNewMessage()` — the one production caller — now always supplies it: `SipMessageFactory::createMessage()` and `RequestsHandler::getMessageFromPool()` were changed to take the message by view/reference rather than by value, so the original wire-received bytes stay intact in `onNewMessage()`'s local variable all the way through to the `handle()` call, with nothing along the way copying, moving-from, or discarding them. This pairs directly with Issue #81: the same reference-only chain that makes the raw bytes available here is what made the `UdpServer` per-packet allocation removable there.

`docs/API.md`'s "exact SIP bytes" / "exactly as captured" claims for `/api/pcap` and `/api/trace` are now literally true for both directions in production — no wording change needed.

Covered by 2 new tests in `tests/PcapCapture_test.cpp`: `InboundCaptureStoresExactWireBytesNotReserializedMessage` (a bare-LF, compact-header fixture that provably re-serializes to different bytes via `toString()`, asserting the capture equals the original raw text) and `InboundCaptureFallsBackToToStringWhenNoRawBytesGiven` (pinning the no-`rawBytes` fallback path). Full host suite: 193/193 passing.

Full detail: https://github.com/GlomarGadaffi/pocket-dial/issues/105

---

### 🟢 Issue #111: `SIP_TRANSPORT=lan8720` on a non-esp32 target fails 1400 objects deep instead of at configure time
* **Status**: ✅ Resolved 2026-08-19
* **Labels**: `bug`, `esp32`

#### Description
`idf.py -D SIP_TRANSPORT=lan8720 set-target esp32s3 && idf.py build` configured without complaint and then failed ~1430 objects into the build with `main/esp_main_eth_lan8720.cpp:44:10: fatal error: esp_eth_phy_lan87xx.h: No such file or directory`. LAN8720 drives the classic ESP32's **internal EMAC**; the ESP32-S3 (and every other non-`esp32` target) has no internal EMAC, so the combination cannot work at runtime either — but nothing in the build system rejected it up front. Root cause: three gates that should have agreed didn't. `main/idf_component.yml`'s component-manager rule (`target == esp32`) correctly declined to fetch `espressif/lan87xx` on a non-`esp32` target; but `main/CMakeLists.txt`'s `lan8720` branch selected `esp_main_eth_lan8720.cpp` and added `REQUIRES espressif__lan87xx` based on `SIP_TRANSPORT` alone, with no target guard, and the source file's own `#if ESP_IDF_VERSION >= ...` include guard also carried no target check. So the source asked for a header the manifest had deliberately declined to fetch, and the resulting failure read like an ESP-IDF version incompatibility rather than an unsupported target/transport pairing.

#### Resolution
Added a configure-time guard to the `lan8720` branch of `main/CMakeLists.txt`: `if(NOT IDF_TARGET STREQUAL "esp32")` → `message(FATAL_ERROR ...)` naming `SIP_TRANSPORT=eth` (W5500) as the alternative for the target in use. `IDF_TARGET` is the same CMake cache variable the component manager's own `target ==` rule resolves against (set well before component registration — confirmed against `esp-idf/tools/cmake/targets.cmake` in this environment's installed ESP-IDF checkout), so the guard agrees with the gate that was already correct instead of introducing a fourth, independent one. Turns the 1400-object-deep missing-header error into a one-line configure failure.

Verified in isolation with a standalone `cmake -P` script reproducing the exact guard expression: fails immediately for `IDF_TARGET=esp32s3` with the intended message, passes through cleanly for `IDF_TARGET=esp32`. A full `idf.py` configure run was not exercised — no activated ESP-IDF environment (`export.ps1`) in this session — so this is a static/isolated verification, not an end-to-end repro-to-green. The shipped `esp32` LAN8720 path and every other `SIP_TRANSPORT` branch (`eth`, `display`, default WiFi SoftAP) are untouched.

Full detail: https://github.com/GlomarGadaffi/pocket-dial/issues/111

---

### 🟢 Issue #81: Zero-Allocation Network Ingestion (`UdpServer` Refactor)
* **Status**: ✅ Resolved 2026-08-19
* **Labels**: (none on the original issue)

#### Description
`UdpServer::receiveLoop()` heap-allocated a `std::string` copy of every incoming packet — `_onNewMessageEvent(std::string(buffer, bytesReceived), senderEndPoint)` — before the parser or message pool ever saw the bytes. The issue's blueprint proposed redefining `OnNewMessageEvent` to pass a zero-copy `std::string_view` instead.

A first pass through this issue (see the issue's own comment thread) traced the call chain and found the literal blueprint would have been a **no-op at best**: as of `main` at the time, everything below `onNewMessage` — `SipMessageFactory::createMessage`, `RequestsHandler::getMessageFromPool`, the pooled `SipMessage::reset()` — already took the string **by value** and moved it through unconditionally, with nothing rejecting or intercepting a packet in between. Switching the event to `string_view` would only have *relocated* where the same one mandatory allocation happened, not removed it. That pass also flagged the version of this that *would* deliver a real win — skipping allocation for packets the Issue #38 rate limiter would reject anyway — as carrying real risk: the admission check lives inside `RequestsHandler::handle()`, the single entry point every test in the suite calls directly, and moving it out to check pre-parse would risk silently dropping defense-in-depth for any direct caller, present or future. Recorded as not safe to do as a quick pass.

#### Resolution
Fixing Issue #105 (same PR) changed the basis that first pass reasoned from: `SipMessageFactory::createMessage()` and `RequestsHandler::getMessageFromPool()` now take the message by reference/view rather than by value (so the original wire bytes can still reach `handle()`'s pcap capture), and `SipMessage::reset()`/`splitMessage()` already only ever read through their `message` parameter — nothing in the chain below `receiveLoop()` takes ownership of it. With that in place the literal blueprint stopped being a no-op: the `std::string(buffer, n)` allocation in `receiveLoop()` had no downstream consumer left to justify it.

Implemented as scoped: `UdpServer::OnNewMessageEvent` is now `std::function<void(std::string_view, sockaddr_in)>`; `receiveLoop()` passes a view over its existing per-task stack buffer (deliberately **not** the `static` scratchpad the issue's blueprint sketched — a `static` buffer would share storage across `UdpServer` instances and introduce a race that doesn't exist today); and every link in the chain (`SipServer::onNewMessage`, `SipMessageFactory::createMessage`, `RequestsHandler::getMessageFromPool`, `SipMessage::reset`/`splitMessage`) now passes the view through instead of an owned string. Audited end to end: every consumer that needs the bytes past this call already copies them out synchronously before returning — `splitMessage()` substrs into the parsed message's owned `_startLine`/`_headerLines`/`_body`, and the #105 pcap-capture line copies into the ring slot — so nothing retains the view past the single synchronous call chain that produced it (`receiveLoop()` → `onNewMessage` → `createMessage` → `getMessageFromPool` → `reset` → back up to `handle()`).

Deliberately did **not** take on the riskier admission-check relocation the first pass identified as the only path to a *bigger* win — `handle()`'s existing rate-limit/validity checks are untouched, so no defense-in-depth regression risk was introduced. Host-level functional coverage of the real `UdpServer`/`SipServer` socket layer (as opposed to the `RequestsHandler`/`SipMessage` pool-and-parse layer these views flow into, which the existing host suite already exercises heavily) remains a gap noted by the first pass and still not closed here — left as a follow-up rather than expanding this change's scope to build new socket-level test infrastructure.

Verified via a full rebuild of the production `SipServer.exe` target (the only build target that actually compiles `UdpServer.cpp`/`SipServer.cpp`/`SipMessageFactory.cpp` — `tests/CMakeLists.txt` does not link those three), which succeeds clean with MSVC, plus the full host suite (193/193 passing) covering every downstream type this refactor touches. ESP-IDF device compilation was not exercised (no activated `idf.py` environment this session); `main/`'s only touch points (`esp_main*.cpp`) construct `SipServer` and never reference `OnNewMessageEvent` directly, so they are unaffected by the signature change.

Full detail: https://github.com/GlomarGadaffi/pocket-dial/issues/81
### 🟢 Issue #32: [Feature Request] Live SIP Tracer in the Web Terminal
* **Status**: ✅ Resolved 2026-08-24
* **Labels**: `feature-request`, `diagnostics`, `dashboard`, `enhancement`

#### Resolution
The data half of this shipped earlier as `GET /api/trace` + a polling dashboard
panel (see the `Issue #32` entries already in `CHANGELOG.md`), but this tracker
entry stayed open because the literal ask — a `trace on`/`trace off`
**command** in the terminal, not just a checkbox — was never built, and
`ISSUES.md`'s own description (WebSockets) was stale against what had actually
shipped (polling). Closing both gaps now:

- Added a small command-line interpreter to the CGA CRT terminal's "SIP Trace"
  card (`src/Helpers/index_html.h`): a `pd>` prompt input accepting `trace on`,
  `trace off`, and `help`. Both commands call the *same* `startTrace()`/
  `stopTrace()` functions the pre-existing checkbox already used, so there is
  still exactly one live-update mechanism (1.5 s polling of `/api/trace`) —
  the command surface is new, the transport is not. Command echoes render into
  the same trace screen the packets stream into, sharing its existing 200-block
  client-side cap so typing commands can't grow the DOM unbounded either.
- Added `tests/HttpTraceCommand_test.cpp`, driving a real `HttpServer` +
  `RequestsHandler` over a real socket with a synthetic SIP message carrying an
  escaped quote/backslash (a realistic `Authorization: Digest ... nonce="a\"b"`
  shape), and asserting the exact bytes `GET /api/trace` serves survive the
  JSON round trip intact. Prior tests only ever checked the C++-side
  `getTraceRecords()` accessor, never the actual wire bytes the terminal's
  `trace on` command renders.
- The terminal command interpreter itself is pure client-side JS with no new
  C++ surface by design (see the comments beside `termExec()`), so it isn't
  separately host-tested beyond the data-path test above.
### 🟢 Issue #115: 777 echo-test INVITE answers 200 OK even when the Session pool is full (bypasses MAX_SESSIONS)
* **Status**: ✅ Resolved 2026-08-28
* **Labels**: `bug`, `reliability`

#### Resolution
Found while executing #79's multi-source-IP load test: driving 16 concurrent `777` SDP-loopback echo calls against `POCKETDIAL_MAX_SESSIONS=8` produced exactly 8 `Session Created` log lines but **16/16 200 OK** responses at the client — the other 8 calls were never tracked server-side yet still looked like they succeeded.

`RequestsHandler::onInvite`'s `destNumber == "777"` branch built and enqueued the `180 Ringing` and `200 OK` responses **unconditionally**, and only afterward called `allocateSession()`. When the pool was already full, `allocateSession()` returned `nullptr` and the `if` body (session bookkeeping) was simply skipped — but the caller had already received its `200 OK` moments earlier, with no `_sessions` entry ever created to back it. The ordinary call-to-call INVITE path (`RequestsHandler.cpp` ~line 1009) already got this right: it calls `allocateSession()` **first** and replies `503 Service Unavailable` before ever building a success response.

Fixed by reordering the `777` branch to match: `allocateSession()` runs first; on `nullptr` it now builds and sends `SIP/2.0 503 Service Unavailable` (same shape as the hunt-group path's 503 a few hundred lines up — `_outbox.emplace_back(data->getSource(), ...)` directly to the packet's source address, since `777` is a virtual extension, not a registered client looked up by number the way the ordinary path's `endHandle(data->getFromNumber(), ...)` resolves its target — sending to the source keeps this consistent with a NAT'd phone whose registered Contact could differ from the address the INVITE actually arrived from) and returns immediately.

Both `180`/`200` messages are then drawn from the message pool **before** `_sessions.emplace()` + `setState(Connected)` run, not after. Publishing the session first and drawing messages second — the first version of this fix — would trade the reported bug for a smaller one: `RequestsHandler::onMediaInvite` (the `440` sibling, same per-session-dummy-dest pattern) already carries a comment for exactly this hazard ("The 200 OK is drawn BEFORE the session is published, because past `_sessions.emplace()` + `Connected` there is no clean way back") — a message-pool refusal after publishing would abandon a `Connected` session with no `180`/`200` ever sent, and the retransmission guard at the top of `onInvite` would then silently drop the caller's retry for that Call-ID, since it's keyed on the session already being `Connected`. Checked `onMediaInvite` itself while investigating this: it already draws its `200 OK` before its `_sessions.emplace()`, so the `440` path does not have this bug and needed no change. Refusing before either mutation leaves nothing behind for `777` either: `allocateSession()` already reset the pooled `Session`'s Call-ID via `reset()`, but since it was never published to `_sessions`, its own reclaim scan (a slot whose Call-ID isn't a key in `_sessions`) treats that slot as free again on the very next call.

**BYE-for-untracked-Call-ID trace (required before landing this fix):** `RequestsHandler::onBye`'s `destNumber == "777"` branch unconditionally replies `200 OK` and calls `endCall()` regardless of whether `getSession(callID)` found anything — the spoofed-teardown authorization check (`isDialogSourceAuthorized`) is skipped when there's no session to authorize against (RFC 3261 would favor a `481 Call/Transaction Does Not Exist` here instead; pre-existing and out of scope for this fix since the teardown itself is harmless either way), but nothing downstream assumes a session exists. `endCall()` itself is fully defensive against an unknown Call-ID, verified by reading each call site: `TransactionLayer::freeForCallId()` and `ParkOrbit::freeForCallId()` both just scan their pool/slots for a matching Call-ID and no-op when nothing matches; `_dtmfState.erase(...)` and `_rtpSender.stop(...)` are no-ops on a key/Call-ID they don't hold; `_sessions.erase(...)` returning 0 skips the CDR record and the "disconnected" log line; and the `_sessionPool` scan finds no match to release. So a BYE for a Call-ID that was correctly refused with a `503` under the fix (no dialog was ever established, so a spec-compliant UA won't send one, but a stray/malicious one could) is already handled sanely today — 200 OK, silent no-op teardown, no crash, no phantom CDR entry. Nothing else in the codebase leaves a session referenced without a corresponding `_sessions` entry, so this fix doesn't introduce a new inconsistency for the BYE path to trip over.

New test `tests/Invite777SessionPool_test.cpp` (`Invite777SessionPool.RefusedWith503NotFalse200WhenSessionPoolIsFull`): drives a real `RequestsHandler` through `handle()`, fills every one of `POCKETDIAL_MAX_SESSIONS` slots with ordinary calls (verified live via `getSession()` — the dashboard's `getSessionCount()`/`getClientCount()` only refresh their snapshot inside `tick()`, not synchronously per packet, so they're the wrong tool for asserting mid-`handle()` state), then sends a `777` INVITE and asserts the response is `503` with no `200`/`180`, and that no `_sessions` entry was created for the refused Call-ID or disturbed for the pre-existing ones. Confirmed the test fails against the pre-fix code (`saw503=false`, `saw200=true`, `saw180=true`) before confirming it passes against the fix.

Full detail: https://github.com/GlomarGadaffi/pocket-dial/issues/115

---

### 🟢 Issue #101: Pool-exhaustion backpressure, SDP re-parse, PcapCapture alloc-under-lock, incomplete firmware audit
* **Status**: ✅ Resolved 2026-08-17 — **A**, **B**, **D**, **E** fixed; **C** formally declined (see below)
* **Labels**: `performance`, `reliability`, `memory-safety`, `cleanup`, `refactoring`, `esp32`
* **Severity**: Medium (bundles five sub-items, ranked A-E in the issue body; A and D are the medium-severity ones)

#### Description
Follow-up from a two-pass review of `src/SIP` (a laziness/reuse-discipline pass plus an embedded-firmware hardware-risk pass) run after PR #100. Two duplication findings and one pre-existing host-build break from that review were fixed directly on `main` (see commit); this issue tracks the five items deliberately **not** fixed in that pass:

- ~~**A** — `getMessageFromPool()`/`allocateVirtualPeer()` (`RequestsHandler.cpp`) still fall back to an unbounded heap allocation on pool exhaustion.~~ ✅ **Fixed 2026-08-17.** Both now fall back to a *bounded* number of heap allocations alive at once (`POCKETDIAL_MSG_HEAP_FALLBACK_MAX`, `POCKETDIAL_VPEER_HEAP_FALLBACK_MAX` in `PoolConfig.hpp`), tracked by an atomic in-flight counter decremented from the shared_ptr deleter, and return `nullptr` past that.

  On refusal the contract is **drop, not 503** — building a 503 would need a message out of the very pool that just came up empty, so the reject-and-503 pattern `allocateSession()` uses is not available here. SIP over UDP retransmits (RFC 3261 §17), so a dropped packet costs latency rather than the call.

  67 call sites updated (55 `getMessageFromPool` in `RequestsHandler.cpp`, 12 `messageFromPool` across the decomposed machines). Two things kept that from being 67 hand-written checks: `enqueue()` now drops a null centrally, which covers every inline `enqueue(addr, messageFromPool(...))`; and the highest-volume path — inbound packets — needed no new code at all, because `SipMessageFactory::createMessage` already returned `nullopt` on null and `handle()` already dropped it. Where a builder returns null its callers mostly checked already. Covered by `tests/RequestsHandler_pool_test.cpp`.
- ~~**B** — `SipSdpMessage` re-parses the full SDP body on every accessor call (`SipSdpMessage.cpp:92-120`), no caching.~~ ✅ **Fixed 2026-08-17.** The six accessors now share one single-pass parse, cached until the body changes. Invalidation is driven by a `_bodyGen` counter on `SipMessage`, bumped by every `_body` mutation and published as `bodyGeneration()` — not a valid/dirty flag, because pooled messages are recycled through `reset()` and `*msg = source` and a flag-based cache would hand the previous call's `c=`/`m=` lines to the next call. The cache holds `(offset, length)` spans rather than `string_view`s so the pool's copy stays correct, and `SipMessage::operator=` is no longer `= default` (it advances `_bodyGen` instead of adopting the source's — the pool assigns through a base reference, so only the base subobject is assigned and a copied generation could make the destination's stale cache look fresh). Covered by `tests/SipSdpMessage_cache_test.cpp` (12 tests, mutation-verified).
- **C** — `TransactionLayer::maybeTrack()` (`TransactionLayer.cpp:39-57`) repeats a correct bounds-safe copy pattern 4x. ⛔ **Declined 2026-08-17, deliberately not fixed.** Re-examined and the original call stands: the four blocks each copy a different `std::string_view` into a different fixed `char[]` member, and the only thing they share is shape. Folding them into a helper trades four obvious bounds-safe copies for one indirection over a member pointer or a lambda per field — the code gets shorter and the review surface gets worse, on the retransmit path, for no behavioral gain. Recorded here so the next reviewer does not re-litigate it.
- ~~**D** — `PcapCapture::record()` (`PcapCapture.hpp:60-66`) does a per-SIP-packet `std::string` alloc/free while holding the caller's mutex.~~ ✅ **Fixed 2026-08-17.** The ring no longer pops and pushes; once full it overwrites the oldest entry in place and recycles that entry's buffer, so the steady-state capture path allocates nothing. `recordInto()` additionally hands the slot's buffer back to the caller and a new `SipMessage::toString(std::string&)` serializes straight into it, removing the per-packet temporary the two capture sites were building *before* `record()` even copied it. Worth doing properly because capture is unconditional — there is no enable flag, so this ran on every inbound and outbound message under `_mutex`. Covered by 3 new tests in `tests/PcapCapture_test.cpp`.
- ~~**E** — The firmware-lens pass didn't finish walking `SipWireUtil.hpp`/`RtpReceiver.cpp`/`RtpSender.cpp` (endianness/packed-struct), `TransactionLayer.cpp` (blocking calls on the retransmit-timer path), or watchdog-starvation risk in long-running loops.~~ ✅ **Completed 2026-08-17.** Findings below.

#### #101(E) audit findings

**1. Data race in the message-pool handout — FOUND AND FIXED (the significant one).**
`findFreePoolSlot()` scanned the *static* `_messagePool` for `use_count()==1` and then copied the `shared_ptr` — a check-then-take with no synchronization, reachable concurrently from two tasks:

* the UDP receive task (`UdpServer.cpp:134` → `SipServer::onNewMessage` → `SipMessageFactory::createMessage` → `getMessageFromPool`), holding **no lock** — `handle()` only takes `_mutex` afterwards, on the already-allocated message;
* the tick task (`esp_main.cpp:192`, or `SipServer::tickLoop` on desktop) → `tick()` → `TransactionLayer::sweep` → `messageFromPool`, under `_mutex`.

Both could observe `use_count()==1` on the same slot and both take it, then `reset()` it concurrently — two owners writing one `SipMessage` and reallocating its strings under each other. Holding `_mutex` on one side does not help: a lock only excludes other holders of that same lock. It fires precisely under load (retransmit sweep active while packets arrive), which is the scenario item A exists to harden. Fixed with a dedicated leaf mutex around the scan-and-take; the fallback deleter is documented as never taking it, since the last reference can drop while it is held.

**2. Endianness / packed structs — clean.** `RtpSender::buildRtpHeader` writes every multi-byte RTP field byte-by-byte in big-endian (`RtpSender.cpp:122-142`); `RtpReceiver` parses the same way. No packed structs, no casting a buffer to a header type, so no strict-aliasing exposure and no host-endianness assumption. `SipWireUtil.hpp` uses `ntohs` on a real `sockaddr_in` field and `inet_ntop` with a correctly-sized `INET_ADDRSTRLEN` buffer.

**3. Blocking calls on the retransmit-timer path — clean, with one note.** `TransactionLayer::sweep()` allocates nothing but a pooled message, does no socket I/O (sends are deferred to the outbox and drained after unlock, per Issue #51), touches no NVS, and never sleeps. It already null-checked `messageFromPool` before this change. Note for the record: every `TransactionLayer` method runs *under* the engine `_mutex` by design (documented at `TransactionLayer.hpp:20`), and `maybeTrack()` does one `msg->toString()` heap allocation on the send path — bounded and not on the timer path, so left alone.

**4. Watchdog starvation in long-running loops — clean.** Every long-lived loop blocks or yields on each iteration, so the idle task always runs: `UdpServer::receiveLoop` and `RtpReceiver::receiveLoop` block in `recvfrom()` with `SO_RCVTIMEO` set; `RtpSender`'s ESP send loop paces with `vTaskDelayUntil` and its host counterpart with `sleep_until`; the bind-retry loops back off with `vTaskDelay`. No spin-waits, no unbounded no-yield work.

Full detail per item: https://github.com/GlomarGadaffi/pocket-dial/issues/101

---


### 🟢 Issue #41: SIP core: Arduino IDE platform detection guards need verification (ESP32/ARDUINO defines)
* **Status**: ✅ Resolved (static audit; no Arduino IDE/hardware available to compile-verify)
* **Labels**: `build-system`, `compatibility`

#### Resolution
No Arduino IDE, `arduino-cli`, or ESP32 toolchain is available in this environment, so this couldn't be closed with an actual compile — what follows is a full static audit of every platform-detection guard in `src/`, `main/`, and `sketches/`, plus what was safe to fix without a compiler to check the result.

**Inventory.** `src/` uses one dominant, deliberate pattern — `#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)` — in ~70 places across `RequestsHandler`, `Registrar`, `RtpSender`/`RtpReceiver`, `UdpServer`, `SipClient`, `SipDigest`, `AdminAuth`, `SipSecretStore`, `ArpLookup`, `PcapCapture`, `PbxEnv`, `SipWireUtil`. This is clearly intentional defensive coverage: `ESP_PLATFORM` is the ESP-IDF native define, `ESP32`/`ARDUINO` are what the Arduino-ESP32 core defines, and different core generations have varied in which of the three they set.

**Narrower than that pattern, audited individually:**
* `src/SIP/SipMessage.hpp` (socket header selection) and `src/SIP/TelephonyApiConfig.cpp` (×2, NVS header selection) checked only `ESP_PLATFORM || ESP32`, missing `ARDUINO`. Fixed to the 3-way form. This project only ships ESP32 sketches, and the Arduino-ESP32 core has always defined `ESP32` alongside `ARDUINO` for any ESP32 board, so `ARDUINO` without `ESP32` can't actually occur here — the fix has zero behavioral effect on any real build, it's pure consistency with the established convention. Verified with the full host suite (168/168) since these headers also compile on host (hitting the `#elif __linux__`/`#else` branches there either way).
* `main/ui/ui.cpp` checks `ESP_PLATFORM` alone. Left as-is: `main/` is exclusively the ESP-IDF-native app entry (`idf.py`-only, per `main/CMakeLists.txt`'s use of `IDF_VERSION_MAJOR` etc.) — the Arduino sketches in `sketches/` have their own `setup()`/`loop()` and never compile anything under `main/`. Not reachable from the Arduino build path at all, so out of scope for this issue.
* `src/Helpers/HttpServer.hpp`/`.cpp` (socket includes, 6 sites) and `src/Helpers/OtaUpdater.hpp`/`.cpp` (the ESP-IDF `esp_ota_*`/`esp_partition_t` wrapper) check `ESP_PLATFORM` alone, with **no** `ESP32`/`ARDUINO` fallback — and both *are* compiled into every Arduino sketch (each `.ino`'s header comment says to add every `.cpp` from `src/Helpers/`, and `HttpServer.cpp` calls into `OtaUpdater`). **Not changed** — I could not verify on a real toolchain whether this is a live bug, and the two plausible outcomes differ (compile failure, which is loud and immediately visible to a hobbyist flashing it, vs. a "silent host-stub" OTA on some older core, which the `OtaUpdater.cpp` header comment frames as one of two *deliberate* side-by-side implementations, not an oversight). Mitigating: every actively-maintained sketch's own header comment pins **"ESP32 Arduino Core ≥ 3.0" / "3.x"** (`SipServerETH.ino`, `SipServer_T_ETH_Lite_W5500.ino`, `SipServer_T_POE_Pro_LAN8720.ino`, `SipServer_JC3248W535.ino`) — core 3.x is built as an ESP-IDF component and reliably defines `ESP_PLATFORM`, so against the documented minimum supported core this is not a live gap. It would only bite on an older (2.x or earlier) core, which isn't what these sketches say to install. Flagged here rather than changed blind, since getting this wrong in either direction (widening a guard that turns out to matter, or leaving a real gap) isn't something I can confirm without a real Arduino IDE + `arduino-esp32` compile — left for whoever next has that hardware/toolchain available.

No sketch-level (`.ino`) platform-detection branching exists to audit — the `.ino` files are pure hardware pin-mapping/setup code with no `#ifdef` on `ESP_PLATFORM`/`ESP32`/`ARDUINO` of their own; the `ARDUINO_EVENT_ETH_*` cases in the Ethernet sketches are event-enum switches (WiFi/ETH event callback dispatch), not platform guards.

---

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
