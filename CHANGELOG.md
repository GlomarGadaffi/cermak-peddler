# Changelog

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
