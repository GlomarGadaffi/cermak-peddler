# pocket-dial

A self-contained SIP PBX that runs on a single ESP32-S3 — no router, no SIP trunk, no cloud.
Point any SIP phone at it (hardware desk phone, ATA, or softphone) and start calling. The same
C++17 engine also builds as a desktop/server binary, so you can run the whole thing on your
laptop while you hack on it.

The device only brokers signaling — **RTP media flows peer-to-peer** between phones, so call
audio never bottlenecks on the microcontroller. Hobbyist-first, field-deployable.

## Quick start (desktop, no hardware)

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/SipServer --ip 127.0.0.1 --port 5060 --web 8080
```

Register two softphones to `127.0.0.1:5060`, call each other, then dial `777` for the echo test.
The web UI is at `http://127.0.0.1:8080`.

## Run it on hardware (ESP32-S3)

Builds with ESP-IDF v5.1 through v6.0.1. The `SIP_TRANSPORT` option picks the board and network path:

| `SIP_TRANSPORT` | Board / target |
|---|---|
| `eth` (default) | W5500 wired / PoE on ESP32-S3 (LilyGO T-ETH-ELITE or Waveshare) |
| `wifi` | generic ESP32-S3, brings up its own SoftAP |
| `display` | Guition JC3248W535 — AXS15231B touch display, LVGL UI |
| `lan8720` | classic ESP32 with internal EMAC (`set-target esp32`) |

```bash
idf.py set-target esp32s3
idf.py -D SIP_TRANSPORT=display build
idf.py -p <PORT> -D SIP_TRANSPORT=display flash monitor
```

Tight on RAM (classic ESP32)? Add `-D SIP_CONSTRAINED=1` with `sdkconfig.defaults.esp32_constrained`
and `partitions_4mb.csv`.

## What you get

- **SIP registrar + PBX** — extensions register and call each other; digest auth and call setup handled on-device.
- **Peer-to-peer RTP** — media goes phone↔phone, not through the MCU.
- **Star codes** — echo test, do-not-disturb, and other classic PBX star codes (see [docs/API.md](docs/API.md)).
- **Web UI + HTTP API** — manage extensions and DND from a browser or `POST /api/dnd`.
- **NVS provisioning** — inject credentials/config without rebuilding ([docs/PROVISIONING.md](docs/PROVISIONING.md)).
- **Dual-OTA firmware updates** ([docs/OTA.md](docs/OTA.md)).
- **Optional SSH sysop terminal** — a TUI console served over SSH (`-D PD_HOST_SSH=ON`, built on wolfSSH).
- **Runs on the desktop too** — same engine, GoogleTest-covered and CI-gated.

## Docs

- [SETUP_GUIDE.md](docs/SETUP_GUIDE.md) · [FLASHING.md](docs/FLASHING.md) · [HARDWARE.md](docs/HARDWARE.md) — getting running
- [PHONE_COMPATIBILITY.md](docs/PHONE_COMPATIBILITY.md) — phones people have actually registered
- [ARCHITECTURE.md](docs/ARCHITECTURE.md) · [RTP.md](docs/RTP.md) — how it works inside
- [PROVISIONING.md](docs/PROVISIONING.md) · [OTA.md](docs/OTA.md) — config and updates
- [SECURITY_AUDIT.md](docs/SECURITY_AUDIT.md) · [THREAT_MODEL.md](docs/THREAT_MODEL.md) — security posture
- [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) — when it doesn't

## Building and tests

The host build is the fast dev loop and what CI gates on:

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build/tests --output-on-failure
```

CI compiles the firmware across ESP-IDF versions × esp32/esp32s3 × wifi/eth and runs the full
GoogleTest suite plus the HTTP API smoke tests. Read [CONTRIBUTING_FIRMWARE.md](CONTRIBUTING_FIRMWARE.md)
before sending firmware PRs.

## License

MIT — see [LICENSE](LICENSE).

## Companion projects

- [pocket-dial-handset](https://github.com/GlomarGadaffi/pocket-dial-handset) — ESP32-S3 push-to-talk SIP handset that registers to pocket-dial
- [resiprocate](https://github.com/GlomarGadaffi/resiprocate) — the vendored SIP/ICE/TURN/STUN/RTP stack
