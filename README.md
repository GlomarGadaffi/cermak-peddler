# pocket-dial

A self-contained SIP PBX that runs on a single ESP32-S3 — no router, no SIP trunk, no cloud needed. LAN extension-to-extension calls route peer-to-peer; the device only brokers signaling. The same C++17 engine compiles to ESP-IDF firmware and a host binary for desktop use and CI.

## Features

- **SIP registrar** — REGISTER / auth digest (RFC 3261), per-extension HA1 cache, open/secure/learn-mode policies
- **B2BUA call routing** — INVITE fork, ring groups (ring-all and hunt), call-forward (unconditional / busy / no-answer), DND
- **Hold / resume** — mid-dialog re-INVITE (RFC 3261 §12.2) and UPDATE (RFC 3311); `Held` session state, CDR talk-time preserved across hold
- **Call transfer** — blind (RFC 3515 REFER) and attended (cross-connect two live sessions)
- **Call parking** — virtual orbit extensions 700–709; park → P2P SDP-swap retrieve → ring-back on timeout
- **Paging zones** — extensions 980–989; intercom auto-answer fork to a configured zone member list
- **BLF / presence** — SUBSCRIBE / NOTIFY (RFC 6665) with RFC 4235 dialog-info XML; change detection on every packet
- **Session timers** — RFC 4028 Session-Expires negotiation; server-as-refresher; BYE on expiry
- **SIP transaction layer** — RFC 3261 §17 Timer A retransmit (exponential back-off) and Timer B timeout; per-Call-ID sweep on teardown
- **Virtual extensions** — `777` SDP loopback / echo test, `999` all-page broadcast, `440` server-sourced RTP tone stream
- **Register beep** — server-originated UAC INVITE plays a short beep on a newly registered phone
- **Blind-transfer guard** — `isDialogSourceAuthorized` rejects BYE / CANCEL from off-path addresses (issue #46)
- **AnchorClient interface** — plug in any external audio system (SIP trunk, recorder, AI pipeline) via the `AnchorClient` abstract class; `LoopbackAnchorClient` is the reference implementation and smoke-test
- **Adaptive jitter buffer** — `PlayoutBuffer` for AnchorClient audio paths (200 ms ceiling, comfort-noise underrun fill)
- **SSH sysop terminal** — ANSI TUI over SSH; screens for System Monitor, Network, PBX Config, Security, CDR
- **Dashboard HTTP API** — `getActiveClients`, `getActiveSessions`, `getParkedCalls`, CDR, DND, call-forwarding (thread-safe snapshot getters)
- **Zero heap in the packet hot path** — fixed pools for `SipClient`, `Session`, `SipMessage`, park slots, BLF subscriptions; exhaustion degrades to `503`, never crashes

## Hardware

| Transport | Board | Target |
|---|---|---|
| `eth` (default) | LilyGO T-ETH-Elite (W5500 wired/PoE) | ESP32-S3 |
| `wifi` | generic ESP32-S3 | ESP32-S3 |
| `display` | Guition JC3248W535 (AXS15231B touch) | ESP32-S3 |
| `lan8720` | classic ESP32 internal EMAC | ESP32 |

## Build

### Host (fastest dev loop, CI gate)

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build/tests -C Release --output-on-failure
```

### ESP-IDF firmware (v5.1 – v6.0.1)

```bash
idf.py set-target esp32s3
idf.py -D SIP_TRANSPORT=eth build
idf.py -p COM3 flash monitor
```

See `CLAUDE.md` for the full build matrix and flash workflow.

## Architecture

One C++17 SIP engine in `src/` compiled three ways (ESP-IDF, host, Arduino). Two hard invariants shape all engine code:

1. **Zero heap in the packet hot path.** Every `SipClient`, `Session`, and `SipMessage` is pre-allocated in fixed pools at boot (`src/SIP/PoolConfig.hpp`). Pool sizes are the device's hard concurrency limits.
2. **No blocking I/O under the registrar lock.** Responses are generated into a local outbox inside `_mutex`, then dispatched after release.

See `docs/ARCHITECTURE.md` for the full design.

## Extending

To bridge calls to an external audio system, implement `AnchorClient` (`src/SIP/AnchorClient.hpp`). The engine calls `makeCall()`, feeds audio via `writeAudio()`, and receives audio back through `AudioRxCallback`. `LoopbackAnchorClient` (`src/SIP/LoopbackAnchorClient.cpp`) is a working example that echoes audio back — useful as a smoke test and as a starting template.

## License

MIT — see `LICENSE`.
