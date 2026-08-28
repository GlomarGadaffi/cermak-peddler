# Changelog

## Unreleased (issue-106-104-108-112-misc-bugs) - 2026-08-19

Four small, independently-filed correctness bugs plus one CI hygiene fix,
bundled into one PR per the triage bucket. Full host suite: 196/196 passing
(WSL/Linux — see Verification).

### Fixed — BLF `forEachSessionInvolving` drops the Callee leg on a self-call (#106)

- `RequestsHandler::forEachSessionInvolving()` (the production `PbxEnv`
  implementation) used an `if (isSrc) ... else if (isDest) ...` chain, a
  regression from commit `f0446d9`'s refactor away from independent flags. When
  a session's src *and* dest are both the watched extension (an extension
  calling itself, or a hunt/ring group where the caller is also a member), the
  `else if` meant only the Caller role ever fired — `BlfSubscriptions::
  computeDialogState` never saw the Callee leg, so a watcher's dialog-info XML
  reported the wrong role/state for that dialog. Restored to two independent
  `if`s so both roles fire when both match — matching the pre-`f0446d9`
  behavior where the visitor received both flags at once.

- `tests/FakePbxEnv.hpp`'s `forEachSessionInvolving()` (the test double every
  `BlfSubscriptions`/`RegisterBeeper`/`ParkOrbit` host test drives) carried the
  identical bug, since it was written to mirror the production method. Fixed
  identically.

  `RequestsHandler::forEachSessionInvolving()` itself is a private `PbxEnv`
  override (`RequestsHandler` privately inherits `PbxEnv`) and so isn't
  directly host-testable in isolation; the fake mirrors it exactly and is
  covered by two new tests in `tests/BlfSubscriptions_test.cpp`: one asserts
  the callback fires twice (once per `DialogRole`) for a self-call session,
  the other drives the real `BlfSubscriptions::computeDialogState` end-to-end
  through the fake and confirms a ringing self-call reports the higher-ranked
  Callee leg (`early`/`recipient`) rather than getting stuck on the
  lower-ranked Caller leg (`trying`/`initiator`) — which is exactly the
  externally-observable symptom the `else if` bug produced.

### Fixed — `RegisterBeeper::sweep` claims "cancelled" when the CANCEL was never built (#104)

Real-hardware evidence (see the issue's comment thread): a LilyGO T-ETH-Elite
S3 logged `"Register beep: no answer from 321, cancelled"` immediately
alongside `"[tx] pool exhausted — INVITE sent without retransmit tracking"` —
i.e. `buildCancel()` failing under the exact message-pool pressure Issue
#101(A) bounds, not a hypothetical.

- `sweep()`'s `AwaitingInviteOk` branch unconditionally logged `"cancelled"`
  and advanced to `AwaitingCancelDone` even when `buildCancel(i)` returned
  `nullptr` (pool exhausted). `AwaitingCancelDone` means "a CANCEL is in
  flight, keep matching this Call-ID for a raced 200 OK" — asserting that when
  no CANCEL went out is both a misleading log during exactly the flood an
  operator would be reading it in, and leaves the dialog in a half-cancelled
  limbo state that nothing ever revisits (the phone was never actually
  CANCELled, and the dialog no longer looks like it needs to be).

  Now: on `buildCancel()` failure the dialog stays in `AwaitingInviteOk` with a
  short (1s) deadline, logs a distinct "cancel build failed — will retry"
  message, and the next `sweep()` tick retries — succeeding once pool pressure
  eases, at which point it follows the original cancelled/`AwaitingCancelDone`
  path unchanged.

- Per the issue's own caution ("don't leave it stuck forever either"): added a
  bounded retry count (`BeepDialog::cancelRetries`, capped at
  `RegisterBeeper::kMaxCancelRetries` = 5). If pool pressure hasn't eased after
  five consecutive retry ticks, `sweep()` gives up and frees the slot outright
  (logged as an error) instead of retrying indefinitely under sustained
  exhaustion — the phone simply rings out on its own INVITE timeout instead of
  ever being CANCELled, which is strictly better than pinning a beeper slot
  forever.

  Covered by two new tests in `tests/RegisterBeeper_test.cpp`: one forces
  `buildCancel()` to fail once (via a new `FakePbxEnv::messagePoolAvailable`
  toggle, matching the existing `sessionPoolAvailable` pattern) and asserts the
  log/state behavior plus a successful retry once the pool recovers; the other
  forces ten consecutive failures and asserts sweep gives up and frees the slot
  rather than retrying forever.

### Fixed — `RtpSender` SSRC/sequence/timestamp seeded from `std::rand()` (#108)

- RFC 3550 §3 wants the RTP SSRC genuinely random so colliding streams can be
  told apart, and the initial sequence/timestamp random so a receiver can
  detect restarts. The host/Linux `rand32()` used `std::rand()` — a single
  process-wide stream, shared and correlated across every caller, which is the
  opposite of what SSRC randomness needs. Replaced with a `thread_local
  std::mt19937` seeded from `std::random_device`, one generator per thread
  rather than one shared global stream. The ESP branch is unchanged
  (`esp_random()`, the hardware RNG, was already correct).

- **Destructor join, investigated and found already correct on `main`:** the
  issue flagged "no destructor is visible in the patch" (referring to commit
  `af5fd24`'s Linux media path). `~RtpSender()` does exist — it predates
  `af5fd24` (added with the original media feature, commit `119ca84`) and was
  carried through unchanged — and its non-ESP branch unconditionally calls
  `stop("")`. On Linux, `stop()` sets `_stopRequested` and **joins
  `_senderThread` synchronously** before returning (see `stop()`'s own
  comment), so a joinable `std::thread` is never left running when the object
  is destroyed; on the plain host stub there is no real thread to join at all.
  No code change was needed for this half — only a comment update, since the
  destructor's comment still called it a "host stub" after `af5fd24` gave the
  same code path a real Linux thread to join. Verified directly: built and ran
  the host test suite under WSL/Linux (`__linux__`, the real thread path, not
  the Windows host-stub one) with a new test that starts a stream and lets the
  `RtpSender` destruct without calling `stop()` first — the process completes
  cleanly rather than calling `std::terminate()`. New test in `tests/Rtp_test.cpp`.

### Fixed — cppcheck version floats with `ubuntu-latest`; scope grew during verification (#112)

- **The suppression half is landed here; the version-pin half needs a
  follow-up push.** This session's `gh` token lacks the `workflow` OAuth
  scope GitHub requires to push a change under `.github/workflows/`, so the
  `ci.yml` diff that actually pins cppcheck to 2.21.0 (built from its own
  tagged source — no official Linux binary, and pip's `cppcheck` package is
  an unrelated 1.5.1-vintage wrapper — cached by version via `actions/cache`
  so a version bump is deliberate, not a runner-image surprise) could not be
  pushed to this branch. It's fully written and verified (see below) and sits
  on the local `backup-with-ci-workflow-change` branch; someone with that
  scope needs to cherry-pick it onto this PR (or push that branch and merge
  it) before this issue is truly closed. Until then CI keeps running the
  floating apt 2.13.0, which is a no-op with respect to every suppression
  below (2.13.0 reports none of these findings), so nothing regresses in the
  meantime — the pin is additive, not a prerequisite for the rest of this PR.

- **The issue's own count was verified and found incomplete.** The issue
  documented exactly 4 findings 2.21.0 has that 2.13.0 doesn't. To pin
  responsibly this needed verifying on the real toolchain rather than trusted
  at face value, so both versions were built from source and run **twice**:
  once on this Windows sandbox's cross-compiled cppcheck (caught the
  discrepancy) and once on a real Linux build via WSL Debian (gcc 14, matching
  the actual `ubuntu-latest` job) to rule out a Windows-build artifact.
  Confirmed on Linux: 2.13.0 finds **zero** issues on `main`; 2.21.0 finds
  **13**, across six categories, not four. The other nine are the identical
  false-positive shape as the issue's own `SipStatus.cpp` example — plain
  aggregates cppcheck now flags per-scalar-member (`uninitMemberVarNoCtor`)
  even though every construction site brace-initializes or immediately
  overwrites the field before any read — plus a third, previously-unreported
  `ESP_IDF_VERSION_VAL` `syntaxError` site and one `performance` finding.
  Pinning to 2.21.0 without addressing all thirteen would have made the pin
  itself the next red-CI surprise, so all thirteen are resolved here:

  - **The two `syntaxError` findings the issue asked to investigate "for
    real"** (`main/esp_main_eth.cpp:36`, `main/esp_main_eth_lan8720.cpp:43`,
    both `#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(...)`) — plus a third,
    identical, previously-unreported instance found during verification
    (`src/SIP/RequestsHandler.cpp:3734`, inside the DTMF NTP-resync
    handler) — are **not** a real parse defect: `esp_idf_version.h` is a real
    ESP-IDF header that a genuine `idf.py` build (Job 2 of this workflow)
    resolves fine. The host lint job's include path (`-I src/Helpers -I
    src/SIP -I main`) has no ESP-IDF tree on it, so cppcheck can't expand
    `ESP_IDF_VERSION_VAL` and aborts that `#if`. Suppressed with inline
    `// cppcheck-suppress syntaxError` comments at each site (verified this
    scopes to just that line and doesn't blind cppcheck to the rest of the
    file — confirmed with a standalone repro before applying), rather than
    the issue's suggested whole-file `--suppress=syntaxError:<path>`, which
    would have silenced *any* future syntax error anywhere in
    `RequestsHandler.cpp` — the codebase's largest, most-frequently-changed
    file, and precisely the "dangerous" outcome the issue was warning about,
    just from the flag rather than from the finding itself.
  - **`SipStatus.cpp:11` `uninitMemberVarNoCtor`** (and, found during
    verification, the same struct's `code` field at line 9 — cppcheck flags
    each scalar member individually, not once per struct): `StatusEntry` is a
    deliberate plain aggregate; its only instance, the constexpr
    `kStatusTable`, brace-initializes every field. Suppressed inline with the
    issue's own rationale.
  - **`main/wifi/DnsServer.cpp:113,141` `dangerousTypeCast`**: the standard
    BSD-sockets `(struct sockaddr *)&x` idiom for `bind()`/`recvfrom()`.
    Suppressed inline with the issue's own rationale.
  - **`src/SIP/PcapCapture.hpp` `uninitMemberVarNoCtor` ×6**
    (`TraceRecord::seq/tsUs/outbound`, `Entry::seq/outbound/tsUs`, found during
    verification, not in the issue): `TraceRecord`'s one construction site
    brace-initializes every field; `Entry`'s one populator (`recordInto()`)
    assigns every field immediately after the `emplace_back()`/slot-reuse that
    default-constructs it, before any read. Suppressed inline.
  - **`src/SIP/RequestsHandler.hpp` `uninitMemberVarNoCtor` ×2**
    (`ProvisioningInfo::authRequired`, `RateBucket::tokens`, found during
    verification): both are always fully brace-initialized or immediately
    overwritten at their one construction site each
    (`findProvisioningInfo()`'s return; `_rateBuckets[ip] = {40.0, now}`).
    Suppressed inline.
  - **`src/SIP/AnchorClient.hpp` `uninitMemberVarNoCtor` ×1** (`CallEvent::
    type`, found during verification): every `CallEvent` construction site in
    `LoopbackAnchorClient.cpp` brace-initializes all four fields. Suppressed
    inline.
  - **`src/SIP/RequestsHandler.hpp:160` `performance: returnByReference`**
    (`getAdminExt()`, found during verification) — deliberately **declined**,
    not applied: `_adminExt` is mutated by `saveAdminExt()`/`loadAdminExt()`
    from call paths that don't share a lock with this getter (callers read it
    off the SIP thread, e.g. the dashboard/HTTP plane). Returning by value at
    least hands the caller an independent copy the instant this call returns;
    returning `const std::string&` as suggested would additionally let that
    reference dangle/tear if a concurrent save reallocates the string while
    the caller still holds it — strictly worse, not a free performance win.
    Suppressed inline with that rationale.

### Verification

Full host suite built and run on **two** toolchains for this bucket, since
issue #112's own build was cross-verified on real Linux (see above) and the
Windows sandbox's Smart App Control policy (enterprise code-integrity
enforcement, unrelated to this change) began blocking execution of freshly-
linked, unsigned test binaries partway through this session:

- **Windows, MSVC 19.44 (Visual Studio 17 2022)**: for #106 and #104,
  temporarily reverted just the production fix (`RequestsHandler.hpp`,
  `RegisterBeeper.cpp`, and the `forEachSessionInvolving()` half of
  `FakePbxEnv.hpp`) with the new tests left in place, confirming all three new
  tests fail against the pre-fix behavior, then restored the fix and confirmed
  they pass. Full suite (171 pre-existing + new tests) passing on the fixed
  tree, prior to the Smart App Control block described below.
- **WSL Debian, gcc 14.2.0** (matches the CI runner's OS/toolchain family):
  configured, built, and ran the fixed tree's full suite — **196/196
  passing**, including the `RtpSender` destructor test running the real
  `__linux__` `std::thread` path (not the Windows host-stub no-op path, which
  would have proven nothing about the join fix).
- **Windows, MSVC 19.44 (Visual Studio 17 2022)**: built clean through
  `RegisterBeeper.cpp`, `RequestsHandler.hpp`/`.cpp`, `RtpSender.cpp`,
  `AnchorClient.hpp`, `PcapCapture.hpp`, `SipStatus.cpp`, and all touched test
  files with no new warnings; test *execution* is currently blocked on this
  machine by Smart App Control (`CodeIntegrity` event log:
  "did not meet the Enterprise signing level requirements") for any
  freshly-rebuilt, unsigned `sip_parser_tests.exe` — confirmed unrelated to
  these changes (the already-built `SipServer.exe` runs fine; only newly
  re-linked test binaries are affected) and not something fixable from within
  this session.
- **cppcheck 2.21.0**, built from source on both a Windows cross-compile and a
  real WSL/Linux build, run against the final tree with the workflow's exact
  invocation: `EXIT=0`, zero findings, on both.
## Unreleased (issue-41-80-verification) - 2026-08-19

Verification pass on two "verification needed" tracker issues, neither of which had
a committed bug repro. No firmware code changed — see the linked GitHub comments for
the full writeups.

### Docs

- `docs/HARDWARE.md` §2 (Guition JC3248W535): added the microSD pin table
  (Issue #80's blocker). SD_MMC (SDIO) 1-bit mode, CLK/CMD/D0 = GPIO 12/11/13,
  cross-referenced against the vendor's own Arduino demo `pincfg.h`, the sibling
  `jc3248-display-driver` bring-up repo, and this repo's own hardware-verified
  `esp_main_display.cpp` comment (three independent sources, all agreeing) — and
  checked for GPIO conflicts against the panel/touch/battery pins already in the
  table (none found). Left open: whether the physical slot is populated on a given
  board (the vendor spec sheet calls it "reserved" and its rear-connector photo
  shows no slot), so no `esp_vfs_fat_*` mounting code was written against this —
  see the GitHub comment on #80 for the recommended next step (a continuity probe).

- Issue #41 (Arduino IDE platform-detection guards): re-audited every `#if`/`#ifdef`
  touched by the original change (`src/Helpers/UdpServer.hpp/.cpp`,
  `src/SIP/RequestsHandler.hpp`, `src/SIP/SipClient.hpp`, `src/SIP/SipMessage.hpp`)
  plus the guards a prior audit (see `ISSUES.md`) already fixed elsewhere
  (`SipMessage.hpp`, `TelephonyApiConfig.cpp`). No new bug found: the
  `#undef INADDR_NONE` in `SipClient.hpp`/`SipMessage.hpp` is preprocessor-scoped to
  the translation unit that includes it (confirmed nothing in `src/`, `main/`, or
  `sketches/` reads `INADDR_NONE` afterward), and `SipServer.cpp`'s
  `#if defined(ARDUINO) ... #elif defined(ESP_PLATFORM)` mDNS-header selection
  correctly prioritizes `ARDUINO` (needed because Arduino-ESP32 3.x builds through
  the IDF build system and can define both). Host build re-verified clean
  (`cmake --build`, no warnings); `ctest` could not be executed in this environment
  (a Device Guard / application-control policy blocks running newly-built,
  unsigned executables here — confirmed with the exact toolchain-build binary, not
  a stale cache). Arduino IDE / `arduino-cli` compile and physical-hardware runtime
  test remain unverified, as before — full details posted as a GitHub comment on
  #41 rather than duplicated here.
## Unreleased (issue-105-111-81-network-build) - 2026-08-19

Three network/build-correctness issues found by code review, bundled as one
themed pass: a signaling-capture fidelity bug (#105), a build-system footgun
(#111), and a per-packet allocation on the SIP receive hot path (#81).

### Fixed — `/api/pcap` inbound capture stored re-serialized SIP, not wire bytes (#105)

- `RequestsHandler::handle()`'s inbound capture point called
  `request->toString()` — a re-serialization of the **parsed** message — not the
  bytes `recvfrom()` actually delivered. `SipMessage::toString()` always writes
  CRLF line endings and rejoins the parsed pieces; it does not restore whatever
  line-ending convention or malformed-but-tolerated layout the wire bytes
  actually used, so a bare-LF message, a compact-header form (`v:`/`f:`/`t:`/
  `i:`), or a blank line the parser silently drops (SEC-02 tolerance) all
  captured differently than what arrived. The outbound side was never affected —
  those messages are server-minted, so `toString()` genuinely is the wire bytes.
- `handle()` now takes an optional `std::string_view rawBytes` and, when the
  caller has wire bytes to offer, writes them into the pcap ring slot verbatim
  instead of calling `toString()`. `SipServer::onNewMessage()` is the only
  production caller and always supplies it — the received bytes flow through
  `SipMessageFactory::createMessage()` and `RequestsHandler::getMessageFromPool()`
  by view (see #81 below) with nothing copying or discarding them before they
  reach the capture point. Every pre-existing `handle(msg)` call site (all of
  `tests/`) keeps compiling unchanged and keeps its old `toString()`-based
  capture, since `rawBytes` defaults to empty.
- `docs/API.md`'s "exact SIP bytes" claim for `/api/pcap` (and `/api/trace`'s
  "raw SIP message bytes, exactly as captured") is now actually true for both
  directions in production — no qualification needed, no doc change required.
- Covered by 2 new tests in `tests/PcapCapture_test.cpp`: one fixture with
  bare-LF endings and compact headers where `toString()` demonstrably
  re-serializes to different bytes (asserting the capture equals the *original*
  raw text, not the re-render), and one confirming the no-`rawBytes` fallback
  still matches pre-#105 behavior exactly.

### Fixed — `SIP_TRANSPORT=lan8720` on a non-esp32 target failed ~1400 objects into the build instead of at configure time (#111)

- LAN8720 drives the classic ESP32's **internal EMAC** — ESP32-S3 and every
  other target has no internal EMAC, so the combination cannot work at runtime
  either. Nothing in `main/CMakeLists.txt` rejected it up front: the
  `lan8720` branch selected `esp_main_eth_lan8720.cpp` and added
  `REQUIRES espressif__lan87xx` based on `SIP_TRANSPORT` alone, with no target
  guard, while `main/idf_component.yml`'s component-manager rule (`target ==
  esp32`) correctly declined to fetch the PHY driver on any other target — so
  the eventual failure was a missing header 1400+ objects into the build,
  reading like an ESP-IDF version mismatch rather than an unsupported
  target/transport pairing.
- `main/CMakeLists.txt`'s `lan8720` branch now checks `IDF_TARGET` (the same
  CMake cache variable the component manager's own `target ==` rule resolves
  against, set well before component registration — confirmed against
  `esp-idf/tools/cmake/targets.cmake`) and fails with `message(FATAL_ERROR ...)`
  at configure time when it isn't `"esp32"`, naming `SIP_TRANSPORT=eth` (W5500)
  as the alternative. Turns the repro in the issue into a one-line configure
  error instead of a confusing compile failure.
- Verified in isolation: a standalone `cmake -P` script reproducing the exact
  `if(NOT IDF_TARGET STREQUAL "esp32")` guard fails immediately for
  `IDF_TARGET=esp32s3` and passes through cleanly for `IDF_TARGET=esp32`. A full
  `idf.py` configure run was not exercised (no activated ESP-IDF environment in
  this session) — the shipped `esp32` LAN8720 path and every other
  `SIP_TRANSPORT` branch are untouched by this change.

### Changed — zero-allocation network ingestion, `UdpServer` → `RequestsHandler` (#81)

- `UdpServer::receiveLoop()` heap-allocated a `std::string` copy of every
  incoming packet's bytes before the parser or message pool ever saw them —
  `_onNewMessageEvent(std::string(buffer, bytesReceived), ...)`. Whether that
  allocation actually bought anything downstream depended on the exact shape of
  the call chain: an earlier pass through this issue found that, as of `main`
  at the time, everything below `onNewMessage` already took the string **by
  value** and moved it through unconditionally with nothing intercepting in
  between — so swapping the event to `std::string_view` would only have
  *relocated* the same one allocation, not removed it, and flagged the change
  as not worth the risk on that basis.
- The #105 fix above changes that basis: `SipMessageFactory::createMessage()`
  and `RequestsHandler::getMessageFromPool()` now take the message **by
  reference** (so `SipServer::onNewMessage()` can still hand the original bytes
  to `handle()` afterwards), and `SipMessage::reset()` already only ever reads
  through its `message` parameter — `splitMessage()` copies what it needs
  (`substr` into the owned `_startLine`/`_headerLines`/`_body`) and nothing
  retains the parameter itself. With that in place, the chain below
  `receiveLoop()` is reference-only end to end, so the original allocation had
  no downstream consumer left to justify it.
- `UdpServer::OnNewMessageEvent` is now
  `std::function<void(std::string_view, sockaddr_in)>`, `receiveLoop()` passes
  a `string_view` over its own stack-local receive buffer (unchanged
  allocation-wise — still per-task, still not `static`, so no shared-buffer race
  between instances is introduced), and every link in the chain
  (`SipServer::onNewMessage`, `SipMessageFactory::createMessage`,
  `RequestsHandler::getMessageFromPool`, `SipMessage::reset`/`splitMessage`) was
  updated to pass the view through rather than an owned string. Every consumer
  that needs the bytes to outlive the call already copies them out
  synchronously before returning — `splitMessage()` into the parsed message,
  and the #105 pcap-capture line into the ring slot — so nothing holds the view
  past the single synchronous call chain `receiveLoop()` → ... → `handle()`
  that produced it.
- Scoped deliberately smaller than the alternative the earlier pass floated
  (moving the Issue #38 rate-limit admission check out of `RequestsHandler::
  handle()` to skip allocation for rejected packets before parsing): that would
  have meant duplicating or relocating defense-in-depth admission logic away
  from the one function every test in the suite calls directly
  (`handler.handle(msg)`), which is a defense-in-depth regression risk this
  change does not take on. `handle()`'s existing rate-limit/validity checks are
  untouched.
- Host-level functional coverage of the actual `UdpServer`/`SipServer` socket
  layer remains a pre-existing gap (noted in the original issue discussion) —
  not added here to keep this change scoped to the data-plumbing types it
  actually touches, which host tests already cover in `RequestsHandler.cpp`/
  `SipMessage.cpp` (the pool/parse layer every one of these views ultimately
  flows into). Left as a good follow-up.

### Verification

- Host suite: **193/193** passing (191 before this work; 2 new tests added for
  #105). Full rebuild of the production `SipServer.exe` target (which is what
  actually compiles `UdpServer.cpp`/`SipServer.cpp`/`SipMessageFactory.cpp` —
  `tests/CMakeLists.txt` does not link those three) succeeds clean with MSVC,
  confirming the `string_view` plumbing type-checks end to end.
- Not covered: ESP-IDF device build (no activated `idf.py` environment this
  session — see #111's verification note above) and an on-hardware/real-socket
  run of the `UdpServer` receive path.

## Unreleased (issue-101-closeout) - 2026-08-17

Closes Issue #101: items **A**, **B**, **D** and **E** are fixed, **C** is
formally declined and recorded as such.

### Fixed — data race in the message-pool handout (#101(E), found by the audit)

- `findFreePoolSlot()` scanned the **static** `_messagePool` for `use_count()==1`
  and then copied the `shared_ptr` — a check-then-take with no synchronization,
  reachable concurrently from two tasks: the UDP receive task
  (`UdpServer.cpp:134` → `SipServer::onNewMessage` →
  `SipMessageFactory::createMessage` → `getMessageFromPool`), which holds **no
  lock** because `handle()` only takes `_mutex` afterwards on the
  already-allocated message; and the tick task (`esp_main.cpp:192`, or
  `SipServer::tickLoop` on desktop) via `TransactionLayer::sweep`, under
  `_mutex`.

  Both could observe `use_count()==1` on the same slot and both take it, then
  `reset()` it concurrently — two owners writing one `SipMessage` and
  reallocating its strings under each other. Holding `_mutex` on one side does
  not help: a lock only excludes other holders of that same lock. It fires under
  exactly the load that item A exists to harden against.

  Fixed with a dedicated **leaf** mutex around the scan-and-take (nothing inside
  its critical section may take another lock, so it is safe regardless of whether
  the caller already holds `_mutex`). Message initialization stays outside the
  critical section, which is by then exclusively owned. The fallback deleter is
  documented as never taking that mutex — the last reference can drop while it is
  held, so a locking deleter could self-deadlock.

### Fixed — pool-exhaustion backpressure (#101(A))

- `getMessageFromPool()` and `allocateVirtualPeer()` fell back to **unbounded**
  heap allocation once their pools were drawn down. Both now allow a bounded
  number of heap fallbacks alive at once — `POCKETDIAL_MSG_HEAP_FALLBACK_MAX` (8)
  and `POCKETDIAL_VPEER_HEAP_FALLBACK_MAX` (4) in `PoolConfig.hpp` — tracked by
  an atomic in-flight counter that the `shared_ptr` deleter decrements, and
  return `nullptr` past that. It is a ceiling on concurrent use, not a rate, so a
  burst is absorbed and only sustained over-subscription is refused.

  On refusal the contract is **drop, not 503**: building a 503 would need a
  message out of the very pool that just came up empty, so `allocateSession()`'s
  reject-and-503 pattern is not available here. SIP over UDP retransmits
  (RFC 3261 §17), so a dropped packet costs latency rather than the call. The
  allocation is also exception-balanced — see the comments; the ownership rules
  around a throwing `shared_ptr` constructor are easy to get wrong in both
  directions.

- 67 call sites updated (55 `getMessageFromPool` in `RequestsHandler.cpp`, 12
  `messageFromPool` across the decomposed machines), with the failure action
  matched to context: `return` in void handlers, `continue` in fork/sweep loops
  so one refusal skips a target rather than aborting a broadcast, and `nullptr`
  propagation out of builders. Resources are acquired before state is mutated, so
  a refusal never leaves a half-built session — the ordering discipline Issue #71
  established for the park path.

  Two things kept that from being 67 hand-written checks. `enqueue()` now drops a
  null centrally, covering every inline `enqueue(addr, messageFromPool(...))` and
  guaranteeing `drainOutbox()` — which dereferences every entry — never sees one.
  And the highest-volume path needed **no** new code: `createMessage()` already
  returned `nullopt` on null and `handle()` already dropped it, so an inbound
  packet that cannot be represented is discarded at the earliest possible point.

- `BlfSubscriptions` no longer records `lastState`/`version` when the NOTIFY it
  just built was refused. Banking a state that was never sent would suppress the
  retry and strand that watcher's busy-lamp until the target's state changed
  *again*; leaving it untouched means the next `refresh()` retries.

### Declined — `maybeTrack()` duplication (#101(C))

- Re-examined and deliberately **not** changed, matching the original call. The
  four blocks each copy a different `string_view` into a different fixed `char[]`
  member and share only their shape; folding them into a helper trades four
  obvious bounds-safe copies for one indirection, on the retransmit path, for no
  behavioral gain. Recorded in `ISSUES.md` so it is not re-litigated.

### Fixed — PcapCapture allocation under the caller's lock (#101(D))

- The ring no longer pops and pushes. Once full it overwrites the oldest entry in
  place and recycles that entry's buffer, so the steady-state capture path
  allocates nothing; it still grows lazily, so a quiet node never pays for slots
  it has not used. Readers now walk from the ring head, since storage order stops
  matching capture order after a wrap.
- `recordInto()` hands the slot's buffer to the caller and a new
  `SipMessage::toString(std::string&)` serializes straight into it, removing the
  per-packet temporary that both capture sites built *before* `record()` even
  copied it. Worth doing properly because capture is unconditional — there is no
  enable flag, so this ran on every inbound and outbound message under `_mutex`.

### Verification

- Host suite: **188/188** passing (168 before this work). The new tests are
  mutation-verified — with each fix reverted in turn they fail, including the two
  that initially did not: same-shape SDP bodies let stale offsets land on correct
  text, and assigning two `SipSdpMessage` lvalues does not reproduce the pool's
  base-subobject assignment.
- Device: all seven edited translation units compile clean for ESP32-S3
  (`xtensa-esp32s3-elf-g++`) under `-Wall -Wextra`, and the full `idf.py build`
  links for the esp32s3 target.
- Not covered: no on-hardware or QEMU run. The concurrency fix in particular is
  argued from the code and pinned by a host-thread test; it has not been observed
  under real dual-task load on a device.

### #101(E) audit — remaining findings, all clean

- **Endianness / packed structs**: `RtpSender::buildRtpHeader` writes every
  multi-byte RTP field byte-by-byte in big-endian and `RtpReceiver` parses the
  same way — no packed structs, no buffer-to-header casts, so no strict-aliasing
  exposure and no host-endianness assumption. `SipWireUtil.hpp` is clean.
- **Blocking calls on the retransmit-timer path**: `sweep()` does no socket I/O
  (sends are deferred to the outbox and drained after unlock, per Issue #51), no
  NVS, no sleep, and already null-checked its pool draw.
- **Watchdog starvation**: every long-lived loop blocks or yields each iteration
  — `recvfrom()` with `SO_RCVTIMEO` in both receive loops, `vTaskDelayUntil` /
  `sleep_until` in the RTP send loops, `vTaskDelay` backoff in the bind-retry
  loops. No spin-waits.

---

## Unreleased (issue-101-B-sdp-parse-cache) - 2026-08-17

Issue #101 item **B**.

### Fixed

- `SipSdpMessage`: the six SDP accessors re-parsed the entire body on every
  call — `getRtpPort()` alone walked it twice, and call setup touches several
  of them per INVITE. They now share one single-pass parse, cached until the
  body changes (Issue #101.B).

- `SipMessage`: added a private `_bodyGen` counter, bumped by every `_body`
  mutation (`reset()`, `setBody()`, `clearBody()`, `enforceG711()`) and
  published to derived classes as `bodyGeneration()`. This is what makes the
  cache above safe on a **pooled** message: `RequestsHandler` recycles the same
  `SipSdpMessage` objects across unrelated calls via `reset()` and
  `*msg = source`, so a cache keyed on a plain valid/dirty flag would serve the
  previous call's `c=`/`m=` lines to the next call — wrong media endpoint, not
  merely a stale read.

  The cache stores `(offset, length)` spans rather than `string_view`s, so the
  copy used by the pool's `*msg = source` recycle stays correct instead of
  leaving the destination pointing into the source's body. This preserves the
  "no `string_view` to fix up after a copy" property `SipMessage` already
  documents.

- `SipMessage::operator=` is no longer `= default`: it now copies every member
  except `_bodyGen`, which it advances instead. The pool assigns through a base
  reference — `getMessageFromPool(const SipMessage&)` does `*msg = source` on a
  `shared_ptr<SipMessage>`, so only the `SipMessage` subobject is assigned and a
  derived class's cache is left untouched. With the generation copied, the
  destination's stale cache could match the incoming generation and be treated
  as fresh. Bumping keeps the counter a strictly per-object timeline, so
  "recorded generation == current generation" can only ever mean "parsed from
  exactly these bytes". Implicit move operations were already suppressed (both
  copy operations were previously user-declared as `= default`), so this is not
  a change in value-category behavior.

- `SipSdpMessage::operator=` is likewise hand-written, for the same reason one
  level down: the implicit one advanced this object's body generation through
  the base operator and then overwrote its *cache* generation with the source's,
  crossing two objects' private counters. When those collided, a stale cache
  read as current against a body it was never parsed from. Assignment now drops
  the cache instead of copying it. The copy *constructor* stays defaulted and is
  correct as-is — it takes both counters from the same object, so a fresh cache
  stays fresh and a stale one stays stale.

### Testing

- New `tests/SipSdpMessage_cache_test.cpp` (13 tests): accessor parity with the
  old per-call parse, and cache invalidation across every body-mutation path,
  including both pool-recycle paths and both assignment defects above. Verified
  by mutation — with the invalidation deliberately disabled, 8 of them fail.

  Three of these only got teeth after being rewritten, and the failure modes are
  worth knowing about. First, the two SDP bodies the tests switch between must
  differ in line *length*, not just in content: with same-shape bodies the stale
  offsets landed on the new body's correct text and the invalidation tests
  passed against a knowingly broken cache. Second, a test that assigns one
  `SipSdpMessage` lvalue to another does NOT reproduce the pool's assignment —
  that one goes through a `SipMessage&` and assigns only the base subobject.
  Third, the derived-assignment defect is a *collision* between two counters, so
  that test sweeps a range of prior-mutation counts on both objects instead of
  hardcoding the one pair that happens to line up today.
- Full host suite: 180/180 passing (168 before this change).
- Device build: all seven edited translation units compile clean for ESP32-S3
  under `-Wall -Wextra`, and the full `idf.py build` links (see the closeout
  section above).
## Unreleased (issue-107-provisioning-crlf) - 2026-08-17

Closes Issue #107. Defense in depth around the zero-touch provisioning config
served by `GET /config/<mac>.cfg` (Issue #35, commit `b79570c`).

### Fixed — config-line injection via the provisioned extension (#107)

- `provisioning::yealinkConfigFor()` interpolates the extension into
  `key = value\r\n` lines. An extension carrying a CR or LF would have appended
  config keys nobody wrote — `account.1.password` being the interesting one —
  to the file the handset parses. The builder now refuses outright (returns an
  empty config) when the extension contains CR or LF, and `sendConfigCfg()`
  treats an empty build as a 404 rather than serving a 200 with an empty body.

- `RequestsHandler::findProvisioningInfo()` re-checks the adopted extension
  against `isValidAor()` before handing it to the builder, so provisioning
  fails closed instead of depending on a gate three call layers away.

**Reachability, since the issue left it open:** not reachable today. Every path
that writes `DeviceRecord::extension` is downstream of `onRegister()`'s
`isValidAor()` check — `admitLearn()`'s adopt (`Registrar.cpp:177`) and
re-extension (`:189`) paths both are — and that charset (alnum plus `. - _ + * #`)
excludes CR/LF. The remaining writer, `loadDevices()` (`:348`), restores from an
NVS blob whose own field/record delimiters are tab/newline, so a CR/LF-bearing
extension could not have round-tripped through it either. Both guards are
therefore backstops, not a live-hole patch: they keep the property local to the
code that depends on it, for future callers wiring in less-trusted input.

Deliberately *not* narrowed to `[0-9A-Za-z]` as the issue suggested — that
would silently mangle `*55`, `+15551234`, and the rest of the AOR charset the
registrar already accepts. Only CR/LF, the actual injection vector, is rejected.

Covered by 3 new tests in `tests/ProvisioningConfig_test.cpp`
(mutation-verified: with the guard removed the injected
`account.1.password = hunter2` line appears 5x in the served config).
Full host suite: 171/171 passing.

## Unreleased (post-100-review-followups) - 2026-08-09

A pass through `src/SIP` following a laziness/reuse-discipline review plus an
embedded-firmware hardware-risk review run after PR #100. See Issue #101 for
the items found but deliberately not fixed here (pool-exhaustion backpressure
redesign, `SipSdpMessage` re-parse caching, `PcapCapture` alloc-under-lock,
and an incomplete firmware audit of the RTP/wire-format files).

### Fixed

- `RequestsHandler.cpp`: consolidated 10 hand-rolled `inet_ntop` +
  IP:port-formatting blocks onto the existing `sipwire::addrToIpPort()`
  helper, which the file already included and used correctly once but had
  re-derived by hand 10 more times as it grew.

- `RequestsHandler.cpp`: consolidated 3 duplicate To-header `;tag=` splice
  blocks onto a new `siphdr::appendTagFrom()` helper in `SipHeaderUtil.hpp`.

- Rate-limited (1-in-100) the `SIP Message pool exhausted` /
  `Virtual-peer pool exhausted` warning logs, previously an unthrottled
  `stderr` write per occurrence — a flood that exhausts the pool would
  otherwise also flood the log/serial pipe. The underlying unbounded
  heap-fallback behavior itself is unchanged and tracked as Issue #101.

- Host build (MSVC): defined `NOMINMAX` for the non-IDF CMake branch.
  `Registrar::noteChange()`'s `std::max(...)` call was colliding with
  `windows.h`'s `max()` macro, breaking the host build MSVC gates on
  independent of this review. Full host suite: 168/168 passing.

## Unreleased (tracker-followups) - 2026-08-04

A pass through the open issue tracker: small hardening/perf fixes, docs
catch-up, and a few feature requests, each on its own commit.

### Fixed

- Raised `CONFIG_LWIP_UDP_RECVMBOX_SIZE` (6 → 32) and set
  `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=32` in `sdkconfig.defaults` to absorb
  bursts of simultaneous SIP packets from one source, which previously
  overran the lwIP receive mailbox and were dropped before the SIP task ever
  saw them (Issue #78, `tests/load/STRESS_FINDINGS.md` finding #1). The
  RAM-constrained profile keeps its own smaller value.

- `RegisterBeeper`: `sweep()` no longer frees a beep dialog's slot in the same
  pass it sends the CANCEL. RFC 3261 §9.1 lets the phone's 200 OK race an
  in-flight CANCEL past the point the CANCEL had any effect; freeing
  immediately meant that raced answer was unrecognized (a beep dialog has no
  `Session`) and never got ACKed or BYEd. The dialog now moves to
  `AwaitingCancelDone` — a state the enum already declared but nothing ever
  entered — for a bounded 5 s window in which `handleOk()` still matches it,
  then frees on either a raced answer being handled or the window elapsing
  with nothing further (Issue #98).

- `onDtmfInfo`: best-effort behavioral detection for a `4887`-prefixed admin
  PIN provisioned before `POST /api/admin/set-pin` started rejecting that
  prefix (Issue #93). The `*4887` HTTP-open star-code matches the instant the
  accumulated sequence equals it — before the `*PIN#code` parser runs — so a
  PIN beginning `4887` is shadowed and can never complete over DTMF. If the
  star-code fires and the admin's next digits then shape up as an interrupted
  `*PIN#code` continuation (`#` + 3+ digits, no leading `*`), a targeted
  warning is logged suggesting a PIN rotation. Imperfect by construction — the
  PIN is salted+hashed, so this can only be inferred behaviorally, never
  confirmed. See `docs/THREAT_MODEL.md` §5.5 for the residual-risk writeup.

### Added

- `GET /api/pcap` (Issue #33): downloads the last `POCKETDIAL_PCAP_RING_SIZE`
  (default 64) SIP signaling packets as a classic libpcap file, ready to open
  in Wireshark. Both directions are captured through the two choke points
  every packet already passes through — `handle()` for inbound,
  `drainOutbox()` for outbound — so no call site needed to remember to
  record anything. RTP/media is out of scope by construction: pocket-dial
  brokers it peer-to-peer and never relays it, so there's nothing server-side
  to capture. Each entry is synthesized into a minimal Ethernet+IPv4+UDP
  frame (dummy MACs, real IP:port) around the exact captured bytes so
  Wireshark's SIP dissector decodes it like a real capture. Session-gated
  (no same-origin check — it's a download, not a mutation). New
  `src/SIP/PcapCapture.hpp` (host-tested ring + serializer).

- `GET /api/trace` + a "SIP Trace" dashboard panel (Issue #32): a live-ish SIP
  signaling tracer for the web dashboard, reusing `#33`'s capture ring rather
  than adding a second one. The tracker/roadmap framed this as a WebSocket
  push stream, but the HTTP server has no WebSocket support today (RFC 6455
  framing, the Sec-WebSocket-Accept handshake, frame masking — none of it
  exists), so building that from scratch felt like too large and too
  security-adjacent an addition for a tracker sweep. Implemented as polling
  instead: the dashboard's toggle starts a 1.5 s poll of `/api/trace` (JSON,
  the same ring as `/api/pcap`, monotonic `seq` per entry) and appends
  whatever it hasn't already rendered, capped client-side at 200 blocks so a
  long-running session doesn't grow the DOM without bound. Verified end to
  end: built and ran the real server, drove real SIP traffic at it, and
  checked the rendered dashboard in a real (Playwright) browser.

- `GET /config/<mac>.cfg` (Issue #35): zero-touch Yealink auto-provisioning
  for already-adopted devices — point a phone's provisioning URL at
  `/config/` and it gets its account/server/codec settings back with no
  manual entry. Covers **re**-provisioning (factory reset, handset swap)
  rather than a device's very first contact, since a MAC has to already be
  in the Registrar's adopted-device registry (Learn/Secure mode) to be
  served — the first-ever REGISTER is what gets it adopted, and still needs
  the phone told its extension by some other means. Never carries a working
  password: `SipSecretStore` only stores a one-way HA1 hash, so a
  Secure-mode (or individually `secure()`'d) device's config flags that the
  admin has to set the password by hand instead of silently omitting it.
  Deliberately not session-gated (a booting phone has no session cookie) —
  the MAC itself (2^48 space, only served if already adopted) plus the
  existing dark-by-default transport gate are the protection. New
  `src/SIP/ProvisioningConfig.hpp` (pure, host-tested config builder). Not
  verified against physical Yealink hardware.

- Linux desktop build now sources real RTP media for extension 440 (Issue
  #82's concrete named gap: `RtpSender`'s `#else` branch was a no-op stub on
  every non-ESP platform, so 440 connected but produced no audio on Linux).
  New `#elif defined(__linux__)` path mirrors the ESP implementation
  one-for-one (`std::thread` + `sleep_until` pacing in place of the FreeRTOS
  task + `vTaskDelayUntil`), with one deliberate divergence: `stop()` joins
  the sender thread synchronously rather than the ESP path's fire-and-forget
  poll, since the existing host test suite (`Rtp_test.cpp`) requires
  `isActive()` to be false the instant `stop()` returns, and `std::thread`
  has no equivalent to poll instead. Bounded to one 20ms pacing tick, and
  only on the 440 diagnostic-tone teardown path — real calls' RTP is
  peer-to-peer and never touches `RtpSender`. Verified against the actual
  built binary: real RTP packets at the correct cadence, clean teardown with
  zero packets after BYE, no deadlock between `stop()`'s join and the
  sender thread's own cleanup lock. The ARMv7 musl cross-compile toolchain
  file this issue also asked for already landed in a prior commit
  (`cmake/armv7-linux-musleabihf.cmake`); the rayhunter Orbic installer
  integration and actual ARM hardware verification remain out of scope
  here (different repo / no hardware in this session).

### Docs

- `docs/API.md` §4 now specifies `/api/cdr`, `/api/dnd`, `/api/forward`,
  `/api/group`, `/api/configuring`, `/api/factory-reset`, `/api/ota/status`,
  `/api/ota/upload`, and `/api/ota/reboot` — request/response schemas, status
  codes, and auth requirements, sourced from `HttpServer::handleClient()`'s
  dispatch table. Also added the missing `401`/session notes to `/api/kill`
  and the two Wi-Fi mutation endpoints, which gained the session gate after
  their original specs were written (Issue #94).

## Unreleased (sip-decomposition-followups) - 2026-08-03

Review follow-ups on the SIP engine decomposition. Host-verified (145/145
GoogleTest, up from 131 — the park, register-beep and BLF machines had no host
coverage at all, which is how the wire defects below survived a green suite).

### Fixed — malformed headers on server-minted requests

`SipMessage::getCallID()` / `getTo()` / `getFrom()` return the **full header
line**, not the bare value. Four hand-built request paths treated them as bare
values and re-stamped the header name on top. All predate the decomposition
(they came in with the `b3cf191` protocol backport) and were carried through it
untouched:

- **Park retrieve re-INVITE** shipped `Call-ID: Call-ID: x@host`, so the parked
  phone could not match the request to its dialog and media never renegotiated.
- **Register beep** matched dialogs by comparing the bare Call-ID it generated
  against the full header line — a comparison that could never succeed. An
  answered beep was therefore never ACKed and never BYEd, and five seconds later
  `sweep()` sent a CANCEL for an INVITE that already had a final response
  (illegal per RFC 3261 §9.1). Its ACK also emitted `To: To: <...>`.
- **REFER progress NOTIFY** (RFC 3515 §2.4.5) emitted `From: To: ...`,
  `To: From: ...` and `Call-ID: Call-ID: ...`.

### Changed — shared plumbing instead of per-machine copies

- `SipWireUtil.hpp` owns `addrToIpPort()` and the `a=inactive` hold offer, which
  had been pasted into ParkOrbit, RegisterBeeper and BlfSubscriptions (each with
  its own socket-include block, none with a Windows branch — `inet_ntop` needs
  `ws2tcpip.h`, so those files could not build on MSVC).
- `BlfSubscriptions` drops its three private header parsers for the shared
  `siphdr::stripHeaderName` and a new `SipMessage::getEvent()` accessor. The old
  event-package scanner re-serialised the whole message and matched any line
  named `o` — which is also the SDP origin line's name.
- `ParkOrbit::consumeParkChanged()` mirrors `Registrar::consumeDevicesChanged()`,
  so the dashboard mirror is polled once per packet instead of being a duty each
  mutating call site had to remember. Paths that never signalled at all
  (`sweep()`, `freeForCallId()` via `endCall`) now do.
- `PbxEnv::sessionsView()` (a reference to `RequestsHandler`'s whole `_sessions`
  map) is replaced by `forEachSessionInvolving()`, keeping the session table's
  representation private.
- `RequestsHandler::drainOutbox()` is the single point messages leave the outbox
  by, so RFC 3261 §17 retransmit registration is structural rather than a scan
  duplicated at each flush site.
- Registrar registry changes are now classified: an online-flag flip patches the
  snapshot rows in place instead of rebuilding the vector and two strings per
  device, which is what a post-reboot registration storm would otherwise cost.

### Removed

- `handleTransferOk()` and `_transferPendingAcks` — unreachable since they were
  introduced; no commit ever added an entry to the list, so the lookup always
  failed and `onOk()` paid a wasted call on every 200 OK.

## Unreleased (sip-engine-decomposition) - 2026-08-03

Structural refactor, behavior-neutral by design. Host-verified (131/131 GoogleTest).

### Changed — RequestsHandler decomposed into discrete state machines

The ~6,200-line RequestsHandler monolith interleaved several independent state
machines behind one mutex. Each now lives in its own class, wired to the shared
engine infrastructure through the narrow `PbxEnv` interface (outbox enqueue,
message pool, deferred log, server identity, registration/session tables). All
of them keep the fixed-slot, no-heap-growth pools and the caller-holds-`_mutex`
convention of the code they were extracted from.

- **`TransactionLayer`** — RFC 3261 §17 INVITE client transactions
  (Timer A retransmit, Timer B timeout, RFC 6026 Timer L absorb).
- **`Registrar`** — REGISTER admission policy (Open/Learn/Secure), digest
  challenge/verify, and the Learn-mode TOFU + MAC-lock adopted-device registry
  (NVS-persisted). The device online flag now lives in the registry record;
  the dashboard snapshot is a plain mirror refreshed via a dirty flag.
- **`RegisterBeeper`** — the register-beep outbound UAC dialog machine
  (auto-answer INVITE → ACK → BYE, CANCEL on timeout).
- **`ParkOrbit`** — call parking: orbit slots 700..70N, park/retrieve SDP swap,
  ring-back on timeout, park sweep.
- **`BlfSubscriptions`** — RFC 6665/4235 BLF watcher dialogs: SUBSCRIBE gate,
  202 + immediate NOTIFY, change-detection NOTIFYs, expiry sweep.

`RequestsHandler` remains the call engine (INVITE routing, forks/groups/hunt,
forwarding, transfer, session timers, CDR, DTMF admin menu) and the thin
dispatcher that owns `_mutex`, the outbox flush and the dashboard snapshot.
Shared helpers moved to `PbxPersist.hpp` (NVS blob format) and
`SipHeaderUtil.hpp` (tag/header-name parsing). Public API unchanged
(`RegistrarMode`/`AdoptedDevice` are now aliases into `Registrar`).

## Unreleased (admin-http-only) - 2026-07-16

Admin-plane hardening: the HTTP dashboard becomes the only admin surface and goes
dark by default on a provisioned device. Host-verified (130/130 GoogleTest).

### Changed — dark-by-default HTTP admin plane

- **Transport gate**: on a provisioned device the HTTP listener is closed by default.
  `HttpServer`'s bind/listen moved out of the constructor into
  `openListenSocket()`/`closeListenSocket()`; the accept loop re-evaluates the gate
  every ~250 ms and fails closed. An unprovisioned device still listens immediately
  (onboarding needs the web UI before any credential exists).
- **DTMF trigger `*4887`** (spells HTTP on the keypad, no PIN): opens the dashboard
  for a bounded TTL (default 600 s), gated on caller extension == admin extension
  (default now `1001`), that extension currently registered, and the SIP INFO's
  source IP matching the registration's bound IP.
- **Provisioning grace window**: a successful `POST /api/admin/set-pin` grants the
  same TTL window, so first-run onboarding isn't cut off the instant provisioning
  flips the gate on.
- **`POST /api/admin/keepalive`** (authenticated): extends the open window by 1 hour;
  surfaced as a "Keep open (1h)" dashboard button.
- **Admin PINs may not begin `4887`** (reserved for the star-code): a PIN with that
  prefix would be shadowed mid-entry by the trigger and could never drive the DTMF
  admin menu. Enforced at set-pin; previously-provisioned PINs are unaffected (see
  ISSUES.md).

### Removed — SSH sysop terminal

- `SshServer`/`Tui` and the wolfSSH transport are **deleted, not hardened** — the
  second, separately-wired admin surface no longer exists (THREAT_MODEL.md E-3/§5.5).
  Removes wolfSSL/wolfSSH from `main/idf_component.yml` and all build wiring; the
  `docs/design/` TUI design folder is retained as historical reference only.

### Fixed

- Removed dead `persistAdminHttpTtl()` (no runtime path ever changed the TTL).
- Stale comments claiming the admin extension defaults to `101` (actual: `1001`).

## Unreleased (anchor-bridge-port) - 2026-06-30

Lands the previously-prepared `sip-backport` branch (below) onto `main`, relicenses
the project, and ports the vendor-agnostic media-anchor architecture and conference
mixer from the project's commercial sibling. Build: zero errors. Tests: all passing
(host; ESP-IDF firmware build pending verification).

### License

- **Relicensed MIT → Apache 2.0.** The original BarGabriel/SipServer MIT notice is
  preserved verbatim in `LICENSE-MIT` (not erased); `NOTICE` documents the dual
  heritage; `LICENSE` is now Apache 2.0 for everything contributed since the fork.
  README's companion-projects link corrected to point at the actual upstream
  (it pointed at the vendored resiprocate stack instead).

### Added — vendor-agnostic anchored media (opt-in, not wired into call routing)

- **`MediaBridge`** (`src/SIP/MediaBridge.cpp/hpp`): RTP↔`AnchorClient` glue. Already
  vendor-neutral as authored (depends only on the abstract `AnchorClient` interface) —
  ported with comment genericization only (no logic changes): stripped prose that named
  a specific commercial transport (HTTP GET/POST stream framing, internal issue-number
  references) in favor of generic anchor-interface language.
- **`TelephonyProvider` / `TelephonyApiConfig`** (`src/SIP/`): provider registry/factory
  + credential-slot table. `TelephonyProviderType` trimmed to ship only `Loopback` — no
  unimplemented vendor enumerators. The registry pattern is the extension point for a
  fork's own connector.
- **`RtpSender::FrameProvider`**: additive optional pull-callback (default `nullptr`,
  existing 440-tone callers unaffected) so `MediaBridge` can feed real audio into the
  sender instead of the synthesized test tone. Also adds the encode-side
  `ulawEncodeBuffer` mirror of the existing `RtpReceiver::mulawDecodeBuffer` (was
  missing; `MediaBridge` needs it).
- **`MixBus`** (`src/SIP/MixBus.cpp/hpp`, `mix_kernels*`, `pie/mix_sum4_s16.S`): N-way
  conference audio mixer — int32 accumulate, saturate exactly once on output,
  lock-free per-port attach/detach/tick lifecycle. Scalar reference is the default;
  the ESP32-S3 PIE vector kernels are opt-in behind `POCKETDIAL_MIXBUS_PIE`. See
  `docs/CONFERENCE_MIXER.md`. Ships standalone and tested, not yet wired into
  `MediaBridge` (tracked: `ISSUES.md` #75) — explicitly NOT a call-routing change.

### Added — tests

- `tests/MediaBridge_test.cpp`: lifecycle/identity bookkeeping, `feedRx` →
  `PlayoutBuffer`, and an end-to-end run through a real `LoopbackAnchorClient` wired
  the way a future caller would wire any `AnchorClient`'s single rx callback.
- `tests/MixBus_test.cpp`: minus-self mixing, no-over-saturation on loud legs,
  attach/detach/reclaim lifecycle, `mix_sum4_s16` primitive (ported from the original
  standalone self-test as GoogleTest cases).

### Fixed

- `ISSUES.md`: issues #65/#66/#67 (call park, paging zones, BLF) were still marked
  "planned" despite being implemented by the sip-backport landing — flipped to shipped.
- `ISSUES.md` Non-Goals: the prior blanket "no server-side media bridging" / "no
  telephony-provider connectors" language predated this pass and would have
  contradicted the shipped (but unwired) code above. Rewritten to the real boundary:
  no vendor connector *implementations*, no PSTN/trunk origination *policy*, no
  `RequestsHandler` call-routing wiring — all a fork's own decision, not built here.
- `main/CMakeLists.txt`: the ESP-IDF firmware SRCS list was missing
  `PlayoutBuffer.cpp` even after the sip-backport landing (it lists sources
  explicitly, unlike the host build's glob) — firmware build was silently
  incomplete. Added, along with the new files above.
- `docs/FEATURE_ROADMAP.md`, `CLAUDE.md`: reconciled the signalling-only framing and
  current-capabilities table with what's actually shipped (the backport features were
  never added to either after landing).

---

## Unreleased (sip-backport) - 2026-06-23

Full SIP protocol feature backport into the engine. Build: zero errors (one
pre-existing `inet_addr` deprecation warning on MSVC, pre-existing). Tests: all
passing.

### Added — SIP protocol

- **RFC 3261 §17 transaction layer** (`RequestsHandler.cpp`): `SipTransaction` pool, Timer A
  retransmit with exponential back-off, Timer B 32 s timeout; `sweepTransactions()` runs
  each tick; `freeTxsForCallId()` cleans up on teardown. Outgoing INVITEs are
  auto-registered on every `handle()` / `tick()` pass.
- **RFC 4028 session timers** (`RequestsHandler.cpp`): `armSessionTimer()` called on call
  connect; `sweepSessionTimers()` sends server-originated BYE to both legs on expiry.
  Server is always refresher (UAS policy).
- **RFC 3311 mid-dialog UPDATE** (`onUpdate()`): relay SDP-bearing UPDATE to the peer leg;
  bodiless UPDATE responds `200 OK` and resets the session timer.
- **RFC 3261 §12.2 mid-dialog re-INVITE** (`onReinvite()`): hold (`a=sendonly`/`inactive`)
  sets `Session::State::Held`; resume restores `Connected`. CDR talk-time preserved across
  hold/resume. 200 OK relay in `onOk()` via `sameAddress()` peer lookup.
- **Call parking** (`onParkInvite`, `handleParkOk`, `parkSweep`, `startParkRingback`, …):
  virtual orbit extensions 700–709; park with `a=inactive` hold SDP; SDP-swap retrieve;
  ring-back on timeout with configurable orbit; bridged BYE relay on teardown.
- **Paging zones** (`onInvite`, `findPageZone`, `setPageZone`, `getPageZones`, `persistPageZones`):
  extensions 980–989; intercom auto-answer fork via `startBroadcastFork()`; NVS persistence
  under key `"pzones"`; mirrored into `_snapshot.pageZones` for the dashboard.
- **BLF / presence** (`onSubscribe`, `refreshSubscriptions`, `sweepSubscriptions`,
  `buildDialogInfoXml`, `computeDialogState`, `buildDialogNotify`): RFC 6665 SUBSCRIBE /
  NOTIFY with RFC 4235 dialog-info+xml body; 489 Bad Event for unsupported packages; change
  detection on every `handle()` pass; terminal NOTIFY on subscription expiry.
- **`isDialogSourceAuthorized()`**: rejects forged BYE / CANCEL from off-path addresses
  (issue #46); wired into `onBye()` and `onCancel()`.
- **`AnchorClient` interface** (`src/SIP/AnchorClient.hpp`): abstract class for external
  audio systems; `makeCall`, `writeAudio`, `AudioRxCallback`, `dropCall`, lifecycle.
- **`LoopbackAnchorClient`** (`src/SIP/LoopbackAnchorClient.cpp/hpp`): reference
  implementation; echoes audio back to the caller; useful as a smoke test and as a
  starting template for SIP trunk / recorder / AI pipeline integrations.
- **`PlayoutBuffer`** (`src/SIP/PlayoutBuffer.cpp/hpp`): adaptive jitter buffer (200 ms
  ceiling, comfort-noise underrun fill, overrun drop-oldest); used by AnchorClient paths.
- **Virtual peer pool** (`_virtualPeerPool`): pre-allocated `POCKETDIAL_VIRTUAL_PEERS`
  `SipClient` shared_ptrs; `allocateVirtualPeer()` recycles by use-count; park and BLF
  use this pool instead of heap-allocating per-call.
- **File-scope static helpers**: `sameAddress()`, `parkTagOf()`, `stripHeaderName()`;
  forward-declared at top of `RequestsHandler.cpp`.
- **`SipMessageTypes`**: `UPDATE`, `SUBSCRIBE`, `BAD_EVENT` constants.

### Added — tests

- `tests/PageZone_test.cpp`: `isPageZoneExt`, `splitZoneMembers`, `joinMembers`
  round-trip and cap tests.
- `tests/PlayoutBuffer_test.cpp`: write/read, underrun/overrun, clear, null-guard,
  target-depth tests.
- `tests/AnchorClient_test.cpp`: `LoopbackAnchorClient` lifecycle, `makeCall` event
  delivery, audio loopback, `dropCall` Dropped event.
- `tests/CMakeLists.txt`: added `PlayoutBuffer.cpp`, `LoopbackAnchorClient.cpp`, and
  the three new test files.

### Fixed (code-review pass — all confirmed during sip-backport review)

- **Broadcast re-INVITE hold triggered first-answer connect path** (`onOk`): the guard
  `state != Connected` was also true for `Held`, so a hold 200 OK re-overwrote the
  established dest and cancelled already-gone pending targets. Changed guard to
  `state == Invited`. (#69)
- **tick()-originated INVITE forks had no Timer A/B coverage** (`tick()`): added the
  same `classifyTxType`/`registerTx` outbox scan that exists in `handle()`, so park
  ring-back, hunt-group next-ring, and CFNA redirect are all registered for RFC 3261
  §17 retransmit. (#70)
- **Park retrieve slot cleared on session-pool exhaustion** (`onParkInvite`): moved
  `allocateSession` before queuing the 200 OK and sending the re-INVITE; returns 503
  and leaves the slot intact if the pool is exhausted. (#71)
- **`sweepSessionTimers` emitted malformed BYE when dialog-To was empty**: added
  `!dTo.empty()` guard to both BYE paths. (#72)
- **`Held` state CDR-logged as Failed with zero duration** (`recordCdr`): added
  `Session::State::Held` to the `Connected`/`Bye` `CdrResult::Answered` case. (#73)
- **`sendParkReinvite` emitted bare Call-ID token** (missing `"Call-ID: "` label):
  single-character fix; RFC 3261 §20.8 violation — phones rejected or dropped the
  retrieve re-INVITE. (part of #68 work)
- **`getPrimaryLocalIP()` called under `_mutex`** in 7 new functions: resolved once at
  construction into `_localIp`; all 20+ `(_serverIp == "0.0.0.0") ? getPrimaryLocalIP()
  : _serverIp` sites replaced. Eliminates a socket/connect syscall from the packet hot
  path. CLAUDE.md §"No blocking I/O under the registrar lock". (#67 follow-on)

### Fixed (pre-existing)

- `onBye` and `onCancel`: spoofed-teardown guard (`isDialogSourceAuthorized`).
- `onCancel`: park-orbit CANCEL now tears down the orbit slot and refreshes the
  dashboard snapshot; paging zone CANCEL now forks correctly (was only handling `999`).
- `onBye`: paging zone BYE now forks correctly (was only handling `999`).
- `onOk`: `handleParkOk` / `handleTransferOk` intercepts added before the session
  lookup; re-INVITE 200 OK relay (hold/resume) added; `setDialogHeaders` +
  `setRemoteSdp` + `armSessionTimer` called on call connect.
- `.gitignore`: `docs/` removed from exclusion list; `LINEAGE.md` and scratch artefacts
  remain excluded.

---

## Unreleased (fix/code-review-hardening) - 2026-06-10

Code-review pass across the cross-platform SIP engine (`src/`) and all four ESP-IDF
entry points (`main/`). Engine changes verified on the host build: clean compile,
113/113 GoogleTest cases, and the `tests/http/test_api.sh` smoke suite. Firmware
changes are compile-gated by the CI ESP-IDF matrix.

### Fixed — engine (`src/`)
- **CRITICAL `getFormParam` token-boundary collision** (`Helpers/HttpServer.cpp`):
  the form parser matched a key as a bare substring, so `getFormParam("…&on=1", "on")`
  could match the `n=` inside `extension=…` and return the wrong value — silently
  **inverting `POST /api/dnd`** when `extension` preceded `on`. Now anchors each match
  to a real parameter boundary (start-of-body or after `&`).
- **`_dummyClient` shared mutable state** (`SIP/RequestsHandler.cpp`): the echo (`777`),
  media (`440`) and `*11`/`*69` star-code paths all reset and shared one
  `_dummyClient`, so a second concurrent virtual call overwrote the first session's
  destination identity. Each dummy session now owns its own `SipClient`; the shared
  member is removed.
- **`440` media answered before the RTP stream started** (`SIP/RequestsHandler.cpp`):
  `onMediaInvite` sent `200 OK` before `RtpSender::start()`, so a socket/task failure
  left the caller in a silently-answered call. Now starts RTP first (→ `500` on
  failure), allocates the session (→ `503` + stream stop if the pool is full), and only
  then sends `200 OK`.
- **Rate limiting serialized on the main registrar mutex** (`SIP/RequestsHandler.cpp`):
  every packet — including ones from already-blocked IPs — took the global `_mutex`
  before being dropped. Per-source admission now runs under a dedicated `_rateMutex`
  *before* `_mutex`, so a flood can't serialize against legitimate signaling.
- **Nonce tag was MD5(secret‖msg), length-extension forgeable** (`Helpers/SipDigest.cpp`):
  replaced with a proper HMAC-MD5 (RFC 2104) over the timestamp.
- **Admin session had no sliding expiry** (`Helpers/AdminAuth.cpp`): an active admin was
  logged out at the absolute TTL mid-session; `validateSession` now extends the deadline
  on each successful check.
- **Port-blind same-origin check** (`Helpers/HttpServer.cpp`): `isSameOrigin` stripped
  ports before comparing; it now also requires the Origin/Host ports to match when both
  are explicitly present.
- Removed the dead no-op `registerClient()` hook and made `setCallState()` return `void`
  (its result was always ignored).

### Fixed — firmware (`main/`)
- **CRITICAL cross-core `g_sipServer` data race** (all four entry points —
  `esp_main.cpp`, `esp_main_eth.cpp`, `esp_main_eth_lan8720.cpp`, `esp_main_display.cpp`):
  the pointer was published by the SIP task and polled by the HTTP/status tasks as a
  plain `SipServer*` with no barrier (stale-null / half-built-object read; hoistable
  poll). Now `std::atomic<SipServer*>` with release-store / acquire-load.
- **CRITICAL DnsServer data race + use-after-free** (`wifi/DnsServer.cpp`/`.hpp`):
  `_running`/`_socketFd` are now `std::atomic`; `stop()` closes the socket once
  (single-owner `exchange`) and joins `dns_task` via an exit semaphore before returning,
  so the object can't be destroyed out from under a running task. Added a 500 ms
  `SO_RCVTIMEO` so the loop observes `_running` without depending on close-unblocks-recv.
- **CRITICAL unbounded `g_sipServer` poll hangs the display dashboard**
  (`esp_main_display.cpp`): `http_server_task` spun forever if the SIP server failed to
  construct (e.g. OOM). Now bounded (~30 s) then self-exits with an error log.
- **HIGH unbounded provisioning spin** (`esp_main.cpp`, `esp_main_eth.cpp`,
  `esp_main_eth_lan8720.cpp`): the "wait for admin credential" loop could hang forever
  with no watchdog; now capped at 30 min, then `esp_restart()` to retry cleanly.
- **HIGH `esp_restart()` racing concurrent flash writes** (`esp_main_display.cpp`):
  the captive-decay reboot now calls `esp_wifi_stop()` to quiesce the radio first.
- **MEDIUM deprecated `esp_event_handler_register`** (`esp_main_eth.cpp`,
  `esp_main_eth_lan8720.cpp`): switched to `esp_event_handler_instance_register` (no
  more duplicate-handler risk on a re-init path), matching the wifi/display builds.
- **MEDIUM ISR-unsafe log hook** (`esp_main_display.cpp`): `screen_log_vprintf` is
  installed via `esp_log_set_vprintf` and can run in ISR context; it now dispatches
  `xQueueSendFromISR` when `xPortInIsrContext()`.
- **LOW** discarded `esp_eth_new_netif_glue`/`esp_netif_attach` returns (eth builds),
  leaked STA/ETH event groups on the failure/fallback paths, and a missing erase/
  wear-leveling contract comment on the raw `prompts` partition (`partitions.csv`).

### CI
- Switched the firmware build matrix to **ESP-IDF v6.0.1 only** — the managed
  components (espressif/w5500, espressif/lan87xx, wolfSSL/wolfSSH) and the source
  guards have moved to the v6 APIs, so the old v5.1.2/v5.2.1 legs no longer build.
  Excluded the meaningless `esp32 + eth` leg (the W5500 default pin maps use
  ESP32-S3-only GPIOs — internal-EMAC Ethernet on the classic ESP32 is the separate
  `lan8720` transport). All three esp32s3 transports (display, eth, wifi) were verified
  building locally on v6.0.1 with these changes (`SipServer.bin` generated for each).
- Suppressed a pre-existing cppcheck `unknownMacro` false-positive on
  `main/ui/ui.cpp` (the LVGL `LV_SYMBOL_RIGHT` glyph macro — cppcheck can't see
  LVGL's headers). This had been failing the host CI job since the Learn-mode
  display work landed; the suppression is scoped to `unknownMacro` on that one file
  so genuine warnings there still fire.
- Cleared the five pre-existing cppcheck `performance` findings that had also been
  failing the host job (real fixes, not suppressions): `SipDigest.cpp` nonce split now
  uses `string_view::substr` instead of pointer/length construction; `SipSecretStore.cpp`
  trims the NVS buffer via `resize(len-1)` + move instead of copying through `c_str()`;
  two `Tui.cpp` self-prefix `substr` assignments became `resize()`; and
  `RequestsHandler::startBroadcastFork` takes its `targets` vector by const reference.
  Host build + GoogleTest suite re-verified green.

### Deliberately not changed (reviewed, declined)
- **RTP task core affinity**: the media TX/RX tasks stay on Core 0 — on the display
  build LVGL owns Core 1, and the existing comments make the placement a deliberate
  choice so the 20 ms RTP cadence isn't stolen by full-screen LVGL blits. Moving them
  would *create* the contention the design avoids.
- **`_messagePool` static storage**: `getMessageFromPool` is a static method called by
  `SipMessageFactory` (which has no handler instance), so the pool must stay static; the
  feared "dangling on handler destruction" doesn't occur (static storage outlives any
  instance).
- **pthread default stack for wolfSSH**: already handled — wolfSSH runs in its own
  explicit 24 KB FreeRTOS task, not a `std::thread` (see `sdkconfig.defaults`).
- A handful of hardware-verified display-timing / PSRAM-routing items were left as-is to
  avoid regressing the validated panel path; see the review notes.

## v1.2.0 - 2026-06-04

Production-hardening + feature release. Verified end-to-end on a JC3248W535
display board (ESP-IDF v5.3.5): builds, flashes, boots, joins Wi-Fi as a client,
serves the dashboard, and handles SIP registration/calls at single-digit-ms
latency.

### Added
- **Admin authentication**: PIN + server-side session layer (salted, iterated
  SHA-256; HttpOnly cookie; brute-force lockout). Dashboard Security panel
  (set-PIN / login / logout) gating every state-changing control. `THREAT_MODEL.md`.
- **OTA firmware updates**: dual-OTA partition table, streaming `/api/ota/upload`
  (bypasses the 16 KB body cap), anti-rollback confirmation on healthy boot, and
  a dashboard Firmware-Update panel. `OTA.md`.
- **Configurable SIP memory pools** (`PoolConfig.hpp`) with 3 documented hardware
  tiers. `SCALING.md`.
- **PBX features**: Call Detail Records (`GET /api/cdr`) and per-extension
  Do-Not-Disturb (`POST /api/dnd`, 480-on-INVITE).
- **Operator docs**: Setup, Hardware-Selection, Phone-Compatibility, Troubleshooting;
  plus server-side RTP design (`RTP.md`) and a technical feature roadmap.
- **SIP load/stress suite** (`tests/load/`) + on-device findings; CI now runs the
  unit suite, a partition-table guard, and a tag-triggered release workflow.

### Fixed
- **Display STA-mode watchdog**: coalesce/rate-limit the on-screen log so a log
  flood can't pin Core 1 on full-screen repaints (task-WDT eliminated).
- **Dashboard unreachable in STATION mode**: the HTTP accept loop's `std::thread`
  had a 3 KB pthread stack that `sendApiStatus` overflowed on-device; raised to
  8 KB and bound the listener to `INADDR_ANY` (robust across AP/STA + DHCP changes).

## Unreleased (fix/esp-idf-ci-failures) - 2026-06-03

### Fixed
- Isolate `esp_wifi` requirement in `main/CMakeLists.txt` to only `wifi` and `display` transport components, ensuring the `eth` transport compiles without wifi dependencies.
- Add `cppcheck` suppressions in `.github/workflows/ci.yml` for third-party QR code files (`qrcode.c` and `qrcode.h`).
- Support compiling under ESP-IDF v6.0+ locally by adding version-conditional CMake target dependencies for the split GPIO and SPI drivers, and adding conditional registry dependency for W5500 Ethernet driver (`espressif/w5500`).
- Resolve application binary partition overflow on local debug configuration by changing the default partition table setting to `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y` (1.5MB).


## Unreleased (fix/cppcheck-warnings) - 2026-06-03

### Fixed
- Resolve cppcheck static analysis warnings and style issues:
  - Initialize `bytesReceived` in `src/Helpers/UdpServer.cpp` to avoid potential use of an uninitialized variable.
  - Add `main/host_compat.h` to provide guarded host-side `MACSTR` / `MAC2STR` macros so host builds and static analysis can process Wi‑Fi event logging without defining ESP-IDF headers.
  - Replace C-style cast with `reinterpret_cast` for `sockaddr` in `main/wifi/DnsServer.cpp` to satisfy cppcheck style checks.

Commit: https://github.com/GlomarGadaffi/pocket-dial/commit/825354bf88afd1fcd965c7ab743999ce10b9e919

### Notes
- `host_compat.h` only defines macros when they are not already defined, so ESP-IDF builds remain unaffected.
- This change targets CI cleanliness and static analysis; runtime behavior on ESP32 devices is unchanged.
