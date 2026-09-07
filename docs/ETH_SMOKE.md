# W5500 Ethernet bring-up & smoke test

End-to-end "is this board alive and on the network?" runbook for the wired-Ethernet
(`SIP_TRANSPORT=eth`) firmware. Written against the **LilyGO T-ETH-ELITE S3** (the default
`eth` board) but applies to any W5500 board — just pick the pin map with `PD_ETH_BOARD`
(see [HARDWARE_SELECTION.md](HARDWARE_SELECTION.md)).

Unlike the Wi-Fi/display SoftAP builds, a wired board joins **your existing LAN** and pulls
a DHCP lease, so your dev machine reaches it over the normal network — you do **not** have
to drop your Wi-Fi to talk to it. That makes this smoke loop much simpler than the display
board's captive-AP dance.

> Reusable scripts live in `.smoke/`: `capture.py` (bounded serial boot capture) and
> `sip_probe.py` (UDP SIP REGISTER/OPTIONS probe). Both use only `pyserial`/stdlib, which
> ship in the ESP-IDF Python env.

> [!IMPORTANT]
> **This runbook assumes a freshly flashed, unprovisioned board** — no admin PIN, registrar
> in the default `open` mode. That is the state §3 leaves you in, and it is the state the
> HTTP checks below are written against. On a **provisioned** board (an admin PIN exists)
> two things change and steps 5–6 read differently:
> * The HTTP listener is **dark by default** — `curl` gets *connection refused*, not a
>   status code (see step 6 and [THREAT_MODEL.md §5.5](THREAT_MODEL.md)).
> * Mutating API calls need a session **and** an `X-CSRF` header
>   ([API.md §2.1](API.md)) — the read-only checks here do not.
>
> The `eth` build has **no SoftAP**, so SoftAP WPA2 (`ap_secure`) is not a factor on these
> boards. The registrar admission mode is.

---

## 0. Prerequisites

- ESP-IDF env sourced in the shell (this machine: `. 'C:\esp\v6.0.1\esp-idf\export.ps1'`).
- Board powered + connected:
  - **USB-C** to the dev machine (for flashing + serial), **and**
  - **RJ45** into a switch/router on the same LAN as your machine (or PoE — the Elite is
    802.3af Class 0; a PoE switch powers it without USB, but you still want USB for serial).
- **PoE + USB at the same time is safe on the T-ETH-ELITE *with the OTG SW set to OFF*.**
  Per the board schematic, a P-FET (AO3401A) automatically disconnects USB VBUS from the
  5 V rail when PoE power is present and blocks back-feed toward the USB host; the OTG
  switch is the only path that drives 5 V *out* of the USB-C port (it exists to power OTG
  peripherals from PoE). So: OTG SW **OFF** whenever a computer is on the USB port —
  matching LilyGO's own instruction. Use an active **802.3af/at** source only; never a
  passive PoE injector.
- Target already set to `esp32s3` (the Elite/Waveshare are S3 parts).

## 1. Find the serial port

Plug in over USB-C and identify the new port.

```powershell
# PowerShell — list COM ports before/after plugging in to spot the new one
[System.IO.Ports.SerialPort]::GetPortNames()
# or, with IDF env sourced:
python -m serial.tools.list_ports -v
```

The Elite enumerates as the ESP32-S3 native USB-CDC (or a CH343/CP210x UART, depending on
the OTG-switch position). Note the port as `COMx` below. If it doesn't appear for flashing,
hold **BOOT**, tap **RST**, release BOOT to force download mode (and check the OTG switch).

## 2. Build

```powershell
# Default = LilyGO T-ETH-ELITE S3 pin map
idf.py -B build_eth_elite -D SIP_TRANSPORT=eth build
# Waveshare instead:  -D SIP_TRANSPORT=eth -D PD_ETH_BOARD=waveshare
```

CMake prints the selected map at configure time:
`SipServer transport: W5500 Ethernet — board: elite`.

## 3. Flash

```powershell
idf.py -B build_eth_elite -p COMx -D SIP_TRANSPORT=eth flash
```

(Plain `flash` preserves NVS — it only writes bootloader/partition-table/ota_data/app.)

## 4. Capture the boot log → confirm link + IP

`idf.py monitor` is interactive and hard to bound; use the capture script for a hands-off
window instead. Connect the Ethernet cable **before** (or during) the capture.

```powershell
python .smoke\capture.py COMx 25 .smoke\boot_eth_elite.txt
```

In the captured log, confirm three things in order:

1. **Pin map compiled in** (proves the right board was selected):
   ```
   W5500 board: LilyGO T-ETH-ELITE S3 — SCLK=48 MISO=47 MOSI=21 CS=45 INT=14 RST=-1 @ 40 MHz
   ```
2. **Link up** (PHY sees the cable):
   ```
   SipServerETH: Ethernet link UP
   ```
3. **DHCP lease** (the part you need for the next steps):
   ```
   SipServerETH: IP:      192.168.x.y
   SipServerETH: Gateway: 192.168.x.1
   ```

Record the IP as `<BOARD_IP>`.

Two optional lines are worth noting if you see them:

* `[boot] applied flash-time cfgseed` — the board carries a `cfgseed` record written by the
  browser flasher and has just applied it to NVS. On an `eth` board the seed's AP/STA fields
  are meaningless, but `regMode` is not: a board seeded `regMode=2` comes up with the SIP
  registrar in **`secure`** mode. Read step 5 accordingly. The line's **absence is normal**
  — a board flashed before `cfgseed` existed, or one whose seed generation was already
  applied, prints nothing.
* An NVS init error early in the log — the firmware erases and re-inits NVS on
  `ESP_ERR_NVS_NO_FREE_PAGES` / `NEW_VERSION_FOUND`, which also clears `cfgseed_gen` and so
  re-arms the flash-time seed on the next boot (`DeviceConfig::applyFlashSeed()`).

## 5. SIP smoke — the signaling stack is alive

From the dev machine (same LAN), poke the registrar. **Any** SIP status line back
(`200 OK`, `401 Unauthorized`, `403 Forbidden`) means the UDP receiver + parser + handler
are all working — that's a pass.

```powershell
python .smoke\sip_probe.py <BOARD_IP> 5060
# -> [probe] RESPONSE to REGISTER ...: SIP/2.0 401 Unauthorized
# -> [probe] RESULT: ALIVE   (exit 0)
```

> [!NOTE]
> **`401` is a pass here, and on a `secure` board it is the only answer you will get.** In
> the default `open` registrar mode a `401` back is just the probe's unauthenticated
> REGISTER being answered. If the board was seeded (or configured) into `secure` mode, every
> `REGISTER` is digest-challenged — the probe can never reach `200 OK`, and that is still a
> pass for *this* test: it proves the stack parses and answers. It does **not** prove a real
> handset can register. Check the mode with `curl.exe -s http://<BOARD_IP>/api/registrar` —
> a read-only `GET` needing no `X-CSRF`, but session-gated once an admin PIN exists, so it
> answers without a cookie only on the unprovisioned board this runbook assumes.

## 6. HTTP dashboard smoke — the management surface is up

The eth build serves the HTTP dashboard on **port 80**.

```powershell
curl.exe -s -o NUL -w "HTTP %{http_code}\n" http://<BOARD_IP>/
# -> HTTP 200   (or 401 if the dashboard requires admin auth — still 'alive')
```

> [!IMPORTANT]
> **A refused connection here is not necessarily a dead board.** Once an admin PIN is
> provisioned, the HTTP listener is closed except inside a bounded open window (default
> 600 s) — `curl` fails to connect at all rather than returning a status
> ([API.md §0](API.md)). On a freshly flashed board this cannot happen: unprovisioned
> devices listen unconditionally. If you hit it on a board that has been used before, either
> re-open the window (dial `*4887` from the registered admin extension) or erase NVS and
> re-run the smoke loop from step 3 — see
> [TROUBLESHOOTING.md](TROUBLESHOOTING.md#the-dashboard-went-dark-after-i-set-a-pin).

Optionally run the shared HTTP smoke suite against it (takes a bare `IP` or `host:port`,
defaults to the device AP if omitted — so pass the board's LAN IP explicitly):

```bash
tests/http/test_api.sh <BOARD_IP>
```

> [!WARNING]
> **`test_api.sh` provisions a PIN** (`1234`, in its last suite) and leaves it set. After it
> runs, the board is provisioned: the HTTP plane goes dark on the next window expiry and
> every mutating call needs an `X-CSRF` header. Run it only on a scratch board you are
> willing to erase, and factory-reset or `erase_region 0x9000 0x6000` afterwards. The suite
> covers the CSRF gate itself (`TC-AUTH-07a` expects `403` for cookie-without-token,
> `TC-AUTH-07b` expects `200` with it), so a `403` in that row is a pass, not a fault.

---

## Pass criteria

| # | Check | Pass signal |
|---|---|---|
| 1 | Correct pin map | `W5500 board: LilyGO T-ETH-ELITE S3 …` in boot log |
| 2 | PHY link | `Ethernet link UP` |
| 3 | DHCP | `IP: 192.168.x.y` |
| 4 | SIP stack | `sip_probe.py` prints `RESULT: ALIVE` (any SIP status line — `401` included) |
| 5 | HTTP dashboard | `curl` returns `200` (or `401`) on port 80. *Connection refused* is a fail **only** on an unprovisioned board — otherwise it is the dark-by-default gate |

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| No `Ethernet link UP` at all | Cable/switch dead, or wrong SPI pin map — recheck the `W5500 board:` line matches your hardware. On a breadboard, keep SPI leads <5 cm (W5500 runs 36 MHz); see [HARDWARE.md §9B](HARDWARE.md). |
| Link UP but no `IP:` | No DHCP server on that LAN segment, or the lease is slow. Set `USE_STATIC_IP 1` (+ the `STATIC_IP/GATEWAY/NETMASK` defines) at the top of `main/esp_main_eth.cpp` and reflash to bypass DHCP. |
| `sip_probe.py` → `NO RESPONSE` but board has an IP | Wrong IP, a firewall on the dev machine, or you're not on the same subnet. Ping `<BOARD_IP>` first. |
| Board never enumerates for flashing | OTG-switch position / native-USB; BOOT-hold + RST tap to enter download mode. |
| Wrong board's pins compiled | You passed (or defaulted) the wrong `PD_ETH_BOARD`. Rebuild; the `W5500 board:` boot line is the ground truth. |
| `curl` says **connection refused** on port 80, but SIP answers | Not a fault: the board is provisioned and the dark-by-default HTTP window has expired ([API.md §0](API.md)). Reopen with `*4887` from the registered admin extension, or wipe NVS (`esptool.py -p COMx erase_region 0x9000 0x6000`) to return it to the unprovisioned state this runbook assumes. |
| `403 {"error":"missing or invalid CSRF token"}` from a script | A provisioned board requires the per-session `X-CSRF` header on mutating calls. Capture `"csrf"` from the `POST /api/admin/login` response ([API.md §2.1](API.md), worked example in [OTA.md §3.2](OTA.md)). Read-only checks in this runbook are unaffected. |
| `429` on `/api/admin/login` | Brute-force lockout: 5 failures per client, 20 aggregate, cooldown doubling from 60 s to ~16 min and cleared only by a correct PIN ([THREAT_MODEL.md §5.2](THREAT_MODEL.md)). Wait it out. |
| A real handset gets `401` forever, though `sip_probe.py` passes | The registrar is in `secure` mode and this phone has no digest secret. (It was not seeded at flash time — `cfgseed`'s `regMode` never reaches the registrar, [#151](https://github.com/GlomarGadaffi/pocket-dial/issues/151) — so look for a dashboard or API change instead.) Check `GET /api/registrar`; recovery is in [TROUBLESHOOTING.md](TROUBLESHOOTING.md#all-phones-stopped-registering-at-once). |
| Settings you cleared come back after a reboot | The `cfgseed` partition re-applies at boot whenever `cfgseed_gen` is missing — which a factory reset or an NVS erase deliberately makes true. Erase the seed too: `esptool.py -p COMx erase_region 0xFFF000 0x1000` (16 MB layout only). |

**Related:** [HARDWARE.md §5](HARDWARE.md) (Elite pinout) · [HARDWARE_SELECTION.md](HARDWARE_SELECTION.md) · [TROUBLESHOOTING.md](TROUBLESHOOTING.md) · [API.md](API.md) · [THREAT_MODEL.md](THREAT_MODEL.md)
