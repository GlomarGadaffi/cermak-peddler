# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

pocket-dial is a self-contained SIP PBX that runs on a single ESP32-S3 (no router/SIP trunk needed) **and** as a desktop/server binary. One C++17 SIP engine (`src/`) is compiled three ways: ESP-IDF firmware, Arduino sketches, and a host executable. RTP media flows **peer-to-peer** between phones; the device only brokers signaling.

## Build & test

The engine is platform-abstracted with preprocessor guards. The same `src/` builds under all of:

### Host / desktop (this is what CI gates on, and the fastest dev loop)
```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/SipServer --ip 127.0.0.1 --port 5060 --web 8080   # main.cpp entry point
```
When `IDF_PATH` is **unset**, the root `CMakeLists.txt` builds the host binary + GoogleTest suite. When `IDF_PATH` is **set**, the same file delegates to ESP-IDF instead — so unset the IDF env for host work.

`-D PD_HOST_SSH=ON` additionally serves the SSH sysop-terminal TUI from the host binary (port via `-D PD_HOST_SSH_PORT`, default 2222): wolfSSL comes from FetchContent, the wolfSSH sources are compiled directly into a static lib, and `cmake/patch_wolfssl.py` re-applies the pty-req fixes to the fetched tree (it understands both the managed-component and `_deps` layouts).

### Unit tests (GoogleTest, pulled via FetchContent)
```bash
ctest --test-dir build/tests --output-on-failure          # all tests
./build/tests/sip_parser_tests --gtest_filter='*Pbx*'     # a single test/group
```
Tests live in `tests/*.cpp` (SIP parser, PBX star-codes, RTP/SDP). They link a subset of `src/SIP/` compiled as host stubs (`RtpSender.cpp` has no-socket fallbacks under `!ESP_PLATFORM`).

### ESP-IDF firmware (ESP-IDF v5.1+ through v6.0.1)
Transport is selected by the `SIP_TRANSPORT` CMake var, which picks the `main/esp_main_*.cpp` entry point and its driver/component set:

| `SIP_TRANSPORT` | Entry point | Board / target |
|---|---|---|
| `eth` (default) | `esp_main_eth.cpp` | W5500 wired/PoE (esp32s3) — pin map via `-D PD_ETH_BOARD=elite`(default, LilyGO T-ETH-ELITE)`/waveshare` |
| `wifi` | `esp_main.cpp` | generic ESP32-S3 SoftAP |
| `display` | `esp_main_display.cpp` | Guition JC3248W535 AXS15231B touch display (LVGL) |
| `lan8720` | `esp_main_eth_lan8720.cpp` | classic ESP32 internal EMAC — needs `set-target esp32` |

```bash
idf.py set-target esp32s3
idf.py -D SIP_TRANSPORT=display build
idf.py -p COM4 -D SIP_TRANSPORT=display flash monitor
```
- This machine's local build/flash workflow (IDF env sourcing, the COM4 display board, NVS-injection provisioning) is documented in auto-memory `build-flash-test-workflow.md` — read it before flashing.
- `SIP_CONSTRAINED=1` shrinks the object pools for low-RAM classic ESP32 (pair with `sdkconfig.defaults.esp32_constrained` + `partitions_4mb.csv`).

### Cross-compile (Orbic ARMv7 musl static, issue #82)
```bash
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=cmake/armv7-linux-musleabihf.cmake -DBUILD_TESTING=OFF
```

### CI gates (`.github/workflows/ci.yml`)
Host job runs **cppcheck** (`--error-exitcode=1`, blocking), clang-tidy (non-blocking baseline ~59), the GoogleTest suite, a `partitions.csv` dual-OTA layout guard, and `tests/http/test_api.sh` against the running host binary. A separate matrix job compiles the firmware across IDF v5.1.2/v5.2.1 × esp32/esp32s3 × wifi/eth (+ one display leg). PRs that touch `src/` or `main/` must pass cppcheck and the unit suite.

## Architecture

### Engine core (`src/SIP/`)
- `SipServer` owns the `UdpServer` socket + `RequestsHandler` + `SipMessageFactory`; on host builds it also runs a `_tickThread`. On ESP, the platform entry point drives `tick()`.
- `UdpServer` (`src/Helpers/`) is the threading/socket abstraction — one receive thread, platform-pinned to a core.
- `SipMessageFactory` parses raw UDP into `SipMessage`/`SipSdpMessage` (O(n) non-mutating index-walking parser, no RTTI), then `RequestsHandler` dispatches by method through a handler table.
- `RequestsHandler` is the heart: the registrar (`_clients`), call state machines (`_sessions`), star-codes/PBX features — **call hold/resume** (re-INVITE + RFC 3311 UPDATE), **call park** on orbits `700`+, **paging zones** `98x`, **BLF/presence** (`SUBSCRIBE`/`NOTIFY`), **session timers** (RFC 4028), ring/hunt groups — the `777` echo and `999` broadcast virtual extensions, and the thread-safe **dashboard query API** (`getActiveClients`, `getActiveSessions`, `getParkedCalls`, CDR, DND, call-forwarding). `AnchorClient`/`MediaBridge`/`TelephonyProvider`/`MixBus` (`src/SIP/`) are separate, compiled-and-tested building blocks for bridging a call to an external audio system or N-way mixing — **not** wired into `RequestsHandler`'s call routing (see `ISSUES.md` Non-Goals and #75).

### The two invariants that shape all engine code
1. **Zero heap allocation in the packet hot path.** Every `SipClient`, `Session`, and `SipMessage` is pre-allocated in fixed pools at boot (`src/SIP/PoolConfig.hpp`). These pool sizes ARE the device's hard concurrency limits; exhaustion degrades gracefully to `503 Service Unavailable` (or a one-off heap fallback for the message pool) — it never crashes. Don't `new`/`make_shared` in `RequestsHandler` or any UDP loop; use the pool allocators.
2. **No blocking I/O under the registrar lock.** Generate responses into a local outbox vector inside the `_mutex` scope, then fire `sendto`/socket calls *after* releasing it. See the worked GOOD/BAD examples in `CONTRIBUTING_FIRMWARE.md` — that file is the enforced coding standard (bounds-checked strings via `strlcpy`/`snprintf`, all NVS/driver/syscall returns checked, no unchecked pointer derefs in onboarding paths).

### Dual-core topology (ESP only)
Core affinity differs by transport and is wired through compile defs in `main/CMakeLists.txt`:
- Non-pure-Ethernet transports define `POCKETDIAL_HAS_WIFI`.
- The **display** build reserves Core 1 for the LVGL graphics task and runs the SIP engine + UDP receiver on Core 0 via `POCKETDIAL_UDP_RX_CORE=0`. Other transports leave SIP on Core 1 (the `UdpServer` default).
- Never dispatch networking/file I/O onto the LVGL core. Cross-core state must go through the snapshotted dashboard getters, not raw shared mutexes.

### Platform abstraction pattern
Code branches on `defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)` vs `__linux__` vs `_WIN32`. When editing `src/`, preserve all three arms. Headers like `host_compat.h` (in `main/`) and the `!ESP_PLATFORM` stubs in `RtpSender.cpp` exist so the engine stays host-compilable and unit-testable.

### SIP interoperability quirks (don't "fix" these — they're deliberate)
- `clearBody()` strips caller SDP from forwarded `180 Ringing` (prevents early-media loops on Yealink-class phones) and rewrites `Content-Length`.
- `enforceG711()` forces the codec list to `0 8 101` on forked INVITE / 200 OK SDP. Any SDP mutation MUST resync `Content-Length` afterward (this was the historical `777`/`999` bug class).

## Layout notes
- `src/` — cross-platform engine (Helpers + SIP). The single source of truth; sketches and firmware all compile it.
- `main/` — ESP-IDF component: per-transport entry points, `drivers/` (AXS15231B QSPI panel), `ui/` (LVGL CGA dashboard + QR), `wifi/` (captive-portal DNS).
- `sketches/` — standalone Arduino IDE ports (the `SipServer_JC3248W535` sketch is **deprecated** in favor of the ESP-IDF display build).
- `.smoke/` — hardware smoke scripts (serial capture, SIP probe, NVS provisioning generator).
- `tests/http/test_api.sh` is the single HTTP smoke suite used by both CI and on-hardware runs.
- `docs/` — deep-dives (ARCHITECTURE, SCALING, THREAT_MODEL, OTA, PROVISIONING, RTP, CONFERENCE_MIXER). `ISSUES.md` is the live architectural roadmap/issue tracker.
- Build output dirs (`build*/`) and `*.log` files in the repo root are local scratch — don't commit them.

## SSH sysop terminal & TUI (the primary config surface)
`src/Helpers/SshServer.cpp` runs a **wolfSSH server** (TCP/22, gated by `POCKETDIAL_HAS_WOLFSSH`, display build) that drives an ANSI **"sysop terminal" TUI**. wolfSSL `^5.8.2` + wolfSSH `^1.4.20` are enabled in `main/idf_component.yml`. Hardware-verified end-to-end.

- **wolfSSL builds on ESP-IDF v6 only because of `cmake/patch_wolfssl.py`** — an idempotent patcher run at CMake configure time. It MUST exist because `managed_components/` is gitignored, so the fixes can't be committed in place: force software crypto (dodges the v6-removed `periph_ctrl.h`/`clk_gate_ll.h`), rename the C23 `thread_local` enum, and two genuine wolfSSH pty-req bugs (`GetStringRef` rejecting a 0-length trailing string; `WMALLOC(0)` on empty terminal modes). **Gotcha:** after the patcher edits a managed-component source, force a recompile (touch the file / `idf.py` may need a nudge) — an incremental build can reuse a stale object and silently ship the unpatched code.
- **The TUI lives in `src/Helpers/Tui.cpp` / `Tui.hpp`** — a transport-agnostic ANSI engine (a `std::function` writer + `feed(bytes)` input; no wolfSSH dependency), driven from `SshServer::runTuiSession`, host-compilable (`tests/Tui_test.cpp`). Screens: banner → hub → System Monitor · Network · PBX Config (tabbed) · Security · Reports/CDR · About, wired to the existing thread-safe `RequestsHandler` getters/mutators.
- **Width discipline (non-negotiable):** every framed row pads via `dispWidth()` / `padCols()` / `truncCols()` — count DISPLAY COLUMNS, never bytes / `strlen` / `%-Ns`. The box-drawing + lexicon glyphs (`●○⊘↳▲`, `←→↑↓`, `·`, `—`) are multibyte UTF-8 but 1 column; byte-counting breaks the 80×24 frame (it bit three times in review). Status is always glyph+LABEL+colour (never colour alone); help is context-scoped; destructive actions use the shared guarded-confirm box; honest in-screen stubs where a backing store doesn't exist yet (never faked data).
- **Hardware-verify loop:** flash `idf.py -D SIP_TRANSPORT=display -p COM4 flash`, then render the live screens through a real terminal emulator with `.smoke/ssh_tui.py <ip>` (uses `pyte` to reconstruct the 80×24 grid). A connection right after a flash can EOF once (board not ready) — just retry.

## Documentation discretion
Keep **committed / public docs vendor-neutral and secret-free**: describe capabilities generically, and never commit credentials, vendor-specific test targets, or commercial product names. pocket-dial's default call path is a LAN-only, peer-to-peer-media PBX. `src/SIP/AnchorClient.hpp` / `MediaBridge` / `TelephonyProvider` are a vendor-neutral, opt-in extension point for bridging a call to an external audio system (ships with only a `Loopback` reference — see `ISSUES.md` Non-Goals); implementing or naming a specific commercial connector stays out of this repo. PSTN/trunk call-origination policy and wiring the anchor into `RequestsHandler`'s call routing are likewise a fork's own decision, not built here.
