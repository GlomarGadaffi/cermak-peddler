# Changelog

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
