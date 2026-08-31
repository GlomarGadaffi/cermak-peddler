# pocket-dial

**A complete SIP phone exchange that fits on a microcontroller.**

Point any SIP phone (desk phone, ATA, or softphone) at pocket-dial and dial each other. No router, no SIP trunk, no cloud—everything runs on a single ESP32-S3 board with Ethernet or WiFi. Audio flows peer-to-peer directly between phones; the microcontroller only choreographs the signaling.

The same C++17 engine compiles to a desktop binary for development and testing. GoogleTest-covered, CI-gated, and field-deployable.

## What it does

### Core exchange
- **Registrar + PBX**: SIP phones register and authenticate. Dial by extension number.
- **Call signaling**: INVITE, BYE, CANCEL, transfer (REFER), hold/resume, session timers, presence subscriptions (BLF).
- **Peer-to-peer audio**: G.711 (μ-law) RTP streams flow directly between phones—the microcontroller never touches call audio.

### Call features
- **Ring groups** (ring all, hunt-to-first-available)
- **Call parking** (orbits `700`–`709`) and retrieval
- **Paging zones** (`980`–`989`)
- **Call-forward** (unconditional, on busy, on no-answer)
- **Do-not-disturb** + DND dials
- **Star codes** (echo test, DND toggle, etc.)

### Admin & provisioning
- **Web dashboard** (HTTP API) — manage extensions, DND, monitor live calls
- **LVGL touchscreen UI** (Guition JC3248W535, optional) — glossy retro operator-board aesthetic
- **NVS provisioning** — inject credentials and config without rebuilding firmware
- **Dual-OTA firmware updates** — safe binary rollout with fallback
- **Dark-by-default HTTP admin plane** — unreachable except during provisioning or after a DTMF gating sequence

### Hardware flexibility
| Transport | Board | Use case |
|-----------|-------|----------|
| **eth** (default) | W5500 wired Ethernet or LAN8720 on classic ESP32 | Office / PoE powered |
| **wifi** | Generic ESP32-S3 + SoftAP | Portable / no cable |
| **display** | Guition JC3248W535 (AXS15231B + LVGL) | Wallboard + touch UI |

## Get it running in 2 minutes (desktop)

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/SipServer --ip 127.0.0.1 --port 5060 --web 8080
```

Open the web UI: **http://127.0.0.1:8080**

Register two SIP softphones (Linphone, Zoiper, etc.) to `sip:127.0.0.1:5060`, dial each other, then dial `777` for the echo test.

## Run it on hardware (ESP32-S3)

**No toolchain?** Flash a release straight from Chrome or Edge at
**<https://glomargadaffi.github.io/pocket-dial/flasher/>** — plug the board in over USB,
pick Ethernet / display / Wi-Fi, click Flash. Or build it yourself:

```bash
# WiFi SoftAP (default for standard ESP32-S3 boards)
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor

# Or, Ethernet + touchscreen display
idf.py set-target esp32s3
idf.py -D SIP_TRANSPORT=display build
idf.py -p /dev/ttyUSB0 -D SIP_TRANSPORT=display flash monitor
```

Low on RAM? Add `-D SIP_CONSTRAINED=1` to drop features and fit the classic ESP32 (512 KB RAM).

For detailed setup, see **[docs/SETUP_GUIDE.md](docs/SETUP_GUIDE.md)** and **[docs/HARDWARE.md](docs/HARDWARE.md)**.

## How it works inside

- **[ARCHITECTURE.md](docs/ARCHITECTURE.md)** — concurrency model, zero-heap-alloc hot path, outbox pattern for safe sockets
- **[RTP.md](docs/RTP.md)** — media bridge, G.711 codec, how peer-to-peer streaming stays under 2 KB/s overhead
- **[THREAT_MODEL.md](docs/THREAT_MODEL.md)** — security posture (rate limiting, digest auth, CIDR allowlist, no SSH surface)

## Capacity & constraints

Compile-time pools; graceful degradation on exhaustion (503 responses). Three tiers:

| Tier | Extensions | Concurrent calls | Board |
|------|-----------|------------------|-------|
| Pocket | 8 | 2 | Classic ESP32 (4 MB flash, constrained) |
| Office | 32 | 8 | ESP32-S3 (16 MB flash, standard) |
| Rack | 128+ | 32+ | Desktop (unlimited) |

See **[docs/SCALING.md](docs/SCALING.md)** for details.

## Building & testing

Host build (fast dev loop, what CI gates on):

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build/tests --output-on-failure
```

CI cross-compiles firmware for ESP-IDF v5.1–v6.0.1, tests across esp32/esp32s3, WiFi/Ethernet, and runs the full GoogleTest suite.

See **[CONTRIBUTING_FIRMWARE.md](CONTRIBUTING_FIRMWARE.md)** before sending firmware PRs.

## Optional: Conference mixing & external audio

The **`MixBus`** and **`MediaBridge`** are fully tested but **not wired into call routing by default** — they're extension points for a fork:

- **`MediaBridge`** glues RTP streams to a vendor-neutral `AnchorClient` interface (e.g., a radio transmitter, recording system, external audio processor)
- **`MixBus`**: N-way conference mixer with assembly-optimized kernels

If you need conference calls or external bridging, see **[docs/CONFERENCE_MIXER.md](docs/CONFERENCE_MIXER.md)** and **[ISSUES.md](ISSUES.md#non-goals)** for the design rationale.

## Docs

**Getting started:**
- [SETUP_GUIDE.md](docs/SETUP_GUIDE.md) — first steps
- [HARDWARE.md](docs/HARDWARE.md) — boards, wiring, PoE
- [PHONE_COMPATIBILITY.md](docs/PHONE_COMPATIBILITY.md) — phones people have actually registered

**Hacking:**
- [ARCHITECTURE.md](docs/ARCHITECTURE.md) — concurrency, memory model, hot path
- [RTP.md](docs/RTP.md) — media bridge, codec handling
- [CONFERENCE_MIXER.md](docs/CONFERENCE_MIXER.md) — optional audio mixing

**Deployment:**
- [PROVISIONING.md](docs/PROVISIONING.md) — inject credentials without rebuilding
- [OTA.md](docs/OTA.md) — dual-OTA safe firmware updates
- [THREAT_MODEL.md](docs/THREAT_MODEL.md) — security architecture
- [SECURITY_AUDIT.md](docs/SECURITY_AUDIT.md) — known constraints

**Ops:**
- [docs/API.md](docs/API.md) — HTTP API reference (extensions, DND, CDR, live calls)
- [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) — "it's not working"
- [Browser flasher](https://glomargadaffi.github.io/pocket-dial/flasher/) — flash a release over USB from Chrome/Edge, no toolchain
- [FLASHING.md](docs/FLASHING.md) — ESP-IDF & build recipes

## License

**Apache License 2.0** — see [LICENSE](LICENSE).

The original SIP engine (BarGabriel/SipServer, MIT-licensed) is preserved verbatim in [LICENSE-MIT](LICENSE-MIT). All extensions (hardware layers, media bridge, call features, UI, security, tests) are Apache 2.0. See [NOTICE](NOTICE) for full attribution.

## Next steps

- **[Quick start](#get-it-running-in-2-minutes-desktop)** on your laptop first.
- **[SETUP_GUIDE.md](docs/SETUP_GUIDE.md)** for hardware flashing.
- **[ARCHITECTURE.md](docs/ARCHITECTURE.md)** to understand the design.
- **[PHONE_COMPATIBILITY.md](docs/PHONE_COMPATIBILITY.md)** to check your phone model.

## Companion projects

- [pocket-dial-handset](https://github.com/GlomarGadaffi/pocket-dial-handset) — ESP32-S3 push-to-talk SIP handset that registers to pocket-dial
- [BarGabriel/SipServer](https://github.com/BarGabriel/SipServer/) — the original MIT-licensed SIP server (see [LICENSE-MIT](LICENSE-MIT))

---

**Questions?** Open an issue or check [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).
