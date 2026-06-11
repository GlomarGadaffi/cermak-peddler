# Changelog

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
