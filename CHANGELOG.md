# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added
- **HTTP Dashboard for Wired-Ethernet Arduino Sketches (#20)**: All three wired-Ethernet Arduino sketches (`SipServerETH`, `SipServer_T_ETH_Lite_W5500`, `SipServer_T_POE_Pro_LAN8720`) now instantiate and start `HttpServer` on port 80, bringing them to feature parity with the WiFi sketch. The dashboard URL is printed to Serial after startup.

### Fixed
- **ESP32 HTTP Stack Buffer Overflow (#9)**: The 4 KB request-read buffer in `HttpServer::handleClient` is now heap-allocated (`std::vector<char>`) instead of stack-local, preventing a crash on ESP32 where the `std::thread` default pthread stack is ~3 KB.
- **HTTP POST Body Truncation (#18)**: After the initial `recv`, `handleClient` now reads until `Content-Length` bytes have been received, ensuring POST bodies that span multiple TCP segments (e.g. WiFi credential POSTs under load) are never silently truncated.
- **CSRF on Mutating API Endpoints (#10)**: Removed the wildcard `Access-Control-Allow-Origin: *` header. `/api/kill` and `/api/wifi/connect` now reject requests carrying an `Origin` header that doesn't match the `Host` header, blocking cross-origin form submissions from other pages on the same AP.
- **`setContact` Empty-Needle Corruption (#11)**: Guarded the `_messageStr.find(_contentLength)` call in `SipMessage::setContact` against an empty `_contentLength` — `std::string::find("")` returns `0` (not `npos`), which would insert the Contact header before the start-line and corrupt the message.
- **`inet_ntoa` Non-Thread-Safe (#19)**: Replaced `inet_ntoa()` with `inet_ntop()` into a stack-local `char[INET_ADDRSTRLEN]` buffer in `RequestsHandler::getActiveClients`.
- **Landing Page Auth Framing (#12)**: Replaced the "Zero-Friction Auth" feature card copy (which framed no-authentication as a selling point) with accurate language and an explicit do-not-expose warning, matching the README.
- **Installer Supply Chain Risk (#14)**: `install.sh` and `install.ps1` now download from the `v1.0.0` release tag instead of `refs/heads/main`, so the install scripts are pinned to a known-good version and unaffected by subsequent commits.

### Changed
- **`parseRequestedExpires` Optimization (#13)**: The standalone `Expires:` header fallback no longer copies and lowercases the entire SIP message. It now scans only lines beginning with `e`/`E` and stops at the header/body boundary blank line.

### Removed
- **Dead `SipMessageHeaders.h` (#15)**: Header file deleted; `SipMessage.cpp` and `SipSdpMessage.cpp` no longer include it (the parser uses its own `matchHeader` lambda with inline string literals).
- **Dead `SipClient::renew()` (#16)**: Method removed. Re-registration constructs a fresh `SipClient` object with the new lease, so an in-place renewal method had no caller.
- **Dead RTP port fields (#17)**: `Session::_srcRtpPort`, `Session::_destRtpPort`, and `SipSdpMessage::setRtpPort` removed. Media is peer-to-peer; the server never reads these values. `Session` and `SipSdpMessage` constructors/signatures updated accordingly.

### Added
- **Registration Lease Expiry / TTL Sweep (#4)**: the registrar now honours registration lifetimes per RFC 3261 §10.2.1. `onRegister` parses the requested expiry from the Contact `expires=` parameter or a standalone `Expires:` header (case-insensitive), clamps it to `[30, 3600]` seconds (default `3600` when unspecified), and echoes the granted value back in the 200 OK Contact. `SipClient` now carries a monotonic (`steady_clock`) lease deadline. Expired bindings are evicted by an opportunistic, throttled sweep (`maybeSweep`, at most once per second) that runs under the existing handler mutex on incoming traffic and on dashboard reads (`getActiveClients`, `getClientCount`) — no additional background thread/task is introduced. `expires=0` continues to de-register immediately.

### Fixed
- **ESP32 UdpServer Shutdown Race (#5)**: `closeServer()` previously waited a fixed `vTaskDelay(10)` for the receive task to exit, which could return while `receiveLoop()` was still blocked in `recvfrom()` (500 ms `SO_RCVTIMEO`) and touching `UdpServer` members — a use-after-free. Now the receive task signals a binary semaphore on exit and `closeServer()` blocks on it (2 s backstop), mirroring `std::thread::join()` on desktop. Task/semaphore handles are value-initialized to `nullptr`.
- **Dashboard Wi-Fi SSID XSS (#6)**: `renderWifiNetworks()` interpolated the scanned SSID into an inline `onclick="selectWifi('…')"` handler; `escapeHtml()` does not escape single quotes, so a crafted SSID could break out and execute on click. Rows are now built as DOM nodes with `textContent` and the click handler bound via `addEventListener` — the SSID never reaches markup as a string.
- **Docs Terminal Self-XSS (#7)**: the simulated terminal in `docs/app.js` echoed raw user input into `innerHTML`. Added an `escapeHtml` helper and applied it to all user-controlled sinks (command echo, unknown-command name, dialed extension).
- **Build Fix**: added missing `#include <atomic>` to `RequestsHandler.hpp` (used `std::atomic<uint64_t>` without the include; broke clean desktop builds).

### Changed
- **README Security Framing (#8)**: reverted the "Zero-Friction Authentication" copy that presented the no-auth design as a feature back to neutral, factual language, and added a prominent warning against exposing SIP/admin ports to the public Internet.

### Added
- **W5500 Ethernet / PoE Server Support**:
    - Added `SipServerETH.ino` Arduino sketch for Waveshare ESP32-S3-ETH + PoE board. Initialises W5500 via `ETH.begin()` on ESP32 Arduino Core 3.x native W5500 support, DHCP with configurable static IP fallback, link status and speed monitoring.
    - Added `main/esp_main_eth.cpp` ESP-IDF entry point for W5500 Ethernet. Full `esp_eth` driver bring-up: SPI bus init, W5500 MAC/PHY configuration (`esp_eth_mac_new_w5500` / `esp_eth_phy_new_w5500`), netif glue, FreeRTOS event group synchronisation for DHCP, heartbeat logging.
    - Default W5500 SPI pin mapping for Waveshare ESP32-S3-ETH: SCLK=12, MISO=13, MOSI=11, CS=10, INT=14, RST=-1. Configurable via `constexpr` / `#define` at top of each file.

### Changed
- **Transport-Selectable Build**:
    - `main/CMakeLists.txt` now supports a `SIP_TRANSPORT` variable (`eth` or `wifi`). Defaults to `eth` for the Waveshare board. Switch with `idf.py -D SIP_TRANSPORT=wifi build` for the original SoftAP path. Adds `esp_eth` and `driver` component dependencies only for ETH builds.

---

## [Previous — ESP32-S3 + Security Hardening]

### Added
- **ESP32-S3 Support**:
    - Added ESP-IDF entry point (`main/esp_main.cpp`) with Wi-Fi SoftAP initialization (`SSID: esp32-sipserver`, open auth, DHCP server on `192.168.4.1`).
    - Created `main/CMakeLists.txt` component registration for ESP-IDF build system.
    - SIP server runs as a dedicated FreeRTOS task (8 KB stack) bound to `192.168.4.1:5060`.
- **Arduino IDE Support**:
    - Added `SipServer.ino` sketch as an alternative entry point for Arduino IDE users.
    - Uses Arduino `WiFi.h` SoftAP with identical network configuration to the ESP-IDF build.
    - Prints station count to Serial every 30 seconds for monitoring.
- **Dual-Build CMake**:
    - Root `CMakeLists.txt` auto-detects `IDF_PATH` and delegates to ESP-IDF when present, falling back to standard desktop CMake otherwise.
    - Bumped minimum CMake version to 3.16.
- **Platform Abstraction**:
    - Extended `#ifdef __linux__` guards to `#if defined(__linux__) || defined(ESP_PLATFORM)` across `UdpServer.hpp`, `UdpServer.cpp`, `SipMessage.hpp`, and `SipClient.hpp`, enabling lwIP POSIX socket compatibility on ESP32.
- **Thread Safety**:
    - Added `std::mutex` to `RequestsHandler`, locked at the entry of `handle()`, protecting `_clients` and `_sessions` maps from concurrent access on multi-core targets (ESP32 FreeRTOS).
- **Documentation**:
    - Complete rewrite of `README.md` with architecture diagram, project structure tree, SIP method/response table, build instructions for Linux/Windows/ESP-IDF/Arduino, call flow sequence diagrams (successful, cancelled, not-found), full API reference for all classes, and ESP32-S3 configuration reference.
- **Security & Robustness (prior round)**:
    - Added comprehensive error handling in `main.cpp` to catch `std::exception` and provide meaningful error messages.
    - Implemented a required check for the `--ip` command-line argument.
    - Added a null-dereference guard in `Session::setState` to prevent crashes if the destination is not set.
    - Wrapped `std::stoi` in `SipSdpMessage::extractRtpPort` with a `try/catch` block to prevent malformed packets from killing the receiver thread.
    - Added a busy-loop guard in `UdpServer` to prevent 100% CPU usage on socket closure or error.
- **Build System (prior round)**:
    - Added `-Wall -Wextra -Wpedantic` (GCC/Clang) and `/W4` (MSVC) warning flags to `CMakeLists.txt`.

### Changed
- **Crash Prevention**:
    - `UdpServer` constructor now throws `std::runtime_error` instead of calling `exit(EXIT_FAILURE)` on socket/bind failure, enabling proper error recovery — critical on ESP32 where `exit()` kills the firmware.
    - `UdpServer::closeServer()` now checks `_receiverThread.joinable()` before calling `.join()`, preventing `std::terminate()` if `startReceive()` was never called.
- **Header Parsing Hardened**:
    - `SipMessage::parse()` rewritten from O(n²) `erase(0, ...)` loop to O(n) index-walking algorithm — no longer copies or mutates the raw message string during parsing.
    - Header identification changed from substring search (`line.find("To")`) to strict prefix check (`line.compare(0, len, hdr) == 0 && line[len] == ':'`), preventing false matches against `Refer-To`, `Reply-To`, and other compound headers.
    - `SipMessage::parse()` now throws on completely malformed datagrams (missing `\r\n` delimiter).
    - Empty-line detection added to stop parsing at the SIP header/body boundary.
- **Object Safety**:
    - All SIP request handlers (`onRegister`, `onInvite`, `onOk`, `endHandle`) now deep-clone messages via `std::make_shared<SipMessage>(*data)` before mutation, eliminating the previous aliased-pointer pattern where `response` and `data` pointed to the same object.
    - `SipMessage::setContact()` now guards against empty `_contact` field — inserts before `Content-Length` instead of corrupting position 0 of the raw string.
    - Extracted duplicated contact-string construction into `RequestsHandler::buildContact()` helper.
- **SDP Parsing**:
    - Renamed `SipSdpMessage::parse()` to `parseSdp()`, removing the virtual-call-in-constructor anti-pattern. Base `SipMessage::parse()` is now non-virtual.
    - `parseSdp()` now throws `std::runtime_error` if `v=` line is missing, instead of silently erasing the entire string and leaving `_rtpPort` uninitialized.
    - `_rtpPort` default-initialized to `0` in the class definition.
- **Performance & Memory**:
    - All 12 `SipMessage` getters changed from return-by-value to `const std::string&`, eliminating heap allocations on every access — significant on ESP32's constrained memory.
    - `Session::getCallID()` changed to return `const std::string&`.
    - `SipClient::getNumber()` changed to return `const std::string&`.
    - `SipClient::getAddress()` changed to return `const sockaddr_in&`.
    - `UdpServer::send()` now takes `const sockaddr_in&` instead of by-value copy.
    - Removed misleading `std::move()` on POD `sockaddr_in` structs in `SipMessage` and `SipSdpMessage` constructors.
    - `SipClient::operator==` now takes `const SipClient&` instead of by-value copy; marked `const`.
- **Code Quality**:
    - Removed dead member variables `_onNewClient` and `_onUnregister` from `RequestsHandler`.
    - Added missing `#include <stdexcept>` to `UdpServer.hpp`, `SipMessage.hpp`, and `SipSdpMessage.cpp`.
    - Added missing `#include <string>` to `SipMessage.hpp` and `SipClient.hpp`.
    - Value-initialized `SipMessage::_src` with `{}` to prevent uninitialized POD reads.
    - Removed unused `#include <sstream>`, `<vector>`, and redundant `<string>` from `.cpp` files.
- **Prior round**:
    - Optimized `UdpServer::send` to pass strings by `const std::string&` instead of value.
    - Improved `RequestsHandler::handle` to use map iterators, avoiding double lookups.
    - Updated `IDGen` to use `std::mt19937` and `std::random_device` for thread-safe, high-quality random ID generation instead of `rand()`.
    - Updated `RequestsHandler` to refresh client addresses on re-registration, supporting clients behind dynamic NAT.
    - Fixed `SipMessage::setFrom` and `SipMessage::setTo` to extract contact numbers *before* moving the source string.
    - Normalized naming conventions in `RequestsHandler` to use `camelCase` for handler methods.
    - Refactored `SipMessage::extractNumber` with proper boundary checks to prevent undefined behavior on malformed headers.
    - Replaced `exit(0)` with `return 0` in `main.cpp` to ensure proper RAII cleanup.
    - Replaced `getchar()` with `std::cin.get()` for better portability.

### Fixed
- Fixed typo: `"Coudn't"` → `"Couldn't"` in `RequestsHandler::onOk` error message, and added missing newline.
- Fixed typo in `SipMessageTypes`: `UNAVAIALBLE` corrected to `UNAVAILABLE`.
- Removed a double semicolon in `SipMessage.cpp`.
- Fixed misleading `--port` description in command-line options.

