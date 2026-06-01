# pocket-dial

A self-contained, enterprise-capable SIP PBX on a $10 microcontroller. Flash an ESP32-S3, connect softphones or physical IP phones to its network, and make direct VoIP calls instantly. No routers, no upstream trunks, no complex servers — the registrar, proxy, DHCP server, HTTP management server, and access point all run concurrently on a single dual-core chip.

Now with **native touch display support** and **W5500 wired-Ethernet capability**:
* **Guition JC3248W535EN HMI Target**: Features a native **ESP-IDF v5.3 + LVGL 8.3** application driving a 3.5" IPS capacitive touchscreen with a retro CGA CRT-style switchboard dashboard — displaying live uptime, battery stats, registration counts, active sessions, and a real-time scrollable system console log directly on the panel.
* **Wired-Ethernet & PoE Support**: Pre-configured transport targets for Waveshare ESP32-S3-ETH and LilyGO T-ETH boards, bridging pocket-dial to professional wired and Power-over-Ethernet network segments.
* **SIP Virtual Extensions**: Features an inline **SIP Echo Test (`777`)** with zero-media-processing SDP loopback, and a **Parallel Broadcast / All-Page Intercom (`999`)** with target-URI rewriting and dynamic SIP auto-answer injection.
* **Strict VoIP Interoperability**: Resolves early media loopback loops and ringing hangs on professional devices (like Yealink IP phones) by automatically stripping caller SDPs from all provisional `180 Ringing` responses via `clearBody()`, while enforcing PCMU/PCMA G.711 codec compliance.

---

## Developer Roadmap & Issue Tracker

> [!NOTE]
> We maintain an active architectural roadmap and issue tracker documenting concurrency challenges, task pinning, and socket-blocking mitigations. Review our planned performance updates in [ISSUES.md](ISSUES.md).

---

## Table of Contents

* [Features](#features)
* [How It Works](#how-it-works)
* [Architecture](#architecture)
* [Project Structure](#project-structure)
* [Supported SIP Methods \& Responses](#supported-sip-methods--responses)
* [Virtual Extensions (`777` & `999`)](#virtual-extensions-777--999)
* [VoIP Interoperability & SDP Stripping](#voip-interoperability--sdp-stripping)
* [Automated Testing & Remote Control](#automated-testing--remote-control)
* [Building](#building)
  * [ESP32-S3 (ESP-IDF v5.3)](#esp32-s3-esp-idf-v53)
  * [JC3248W535EN Smart Display](#jc3248w535en-smart-display)
  * [W5500 Ethernet & PoE](#w5500-ethernet--poe)
  * [ESP32-S3 (Arduino IDE)](#esp32-s3-arduino-ide)
  * [Desktop (Linux / Windows)](#desktop-mode)
* [API Reference](#api-reference)
* [Configuration](#configuration)
* [License](#license)

---

## Features

* **Standalone AP Mode (ESP32-S3)** — Spawns an open Wi-Fi access point (`esp32-sipserver`), runs an internal DHCP server, and binds the SIP registrar to `192.168.4.1:5060`. Connect, register, and call with zero external infrastructure.
* **W5500 Ethernet / PoE Transport** — Direct network interface driver support for wired RJ45 and Power-over-Ethernet environments, featuring DHCP or static IP fallbacks.
* **Captive Portal Wi-Fi Onboarding** — When unconfigured, spawns a secure setup SoftAP (`My-Ap`) and displays a join QR code on screen. Intercepts HTTP traffic via a background DNS redirection server (Port 53), leading clients to a retro-themed onboarding wizard at `192.168.4.1` to provision router credentials or toggle permanent Standalone AP mode.
* **Smart Touchscreen Dashboard** — Native ESP-IDF 5.3 + LVGL 8.3 CGA-style HMI. Double-buffered in external OPI PSRAM (307.2 KB 16-bit color frames). Tap to cycle phosphorus theme colors (CGA Blue / Amber CRT / Green Phosphor), reset via touch dialog, or monitor system console logs captured from standard stdout (`esp_log_set_vprintf`).
* **Active Keepalive OPTIONS & Pruning** — Periodically dispatches OPTIONS ping packets to all active clients. Automatically reaps dead bindings upon lease timeouts or silent periods to bound memory.
* **Robust Concurrency & Headless Fallback** — Dedicated, core-isolated FreeRTOS tasks. SIP signaling executes on Core 1, and HTTP/Web dashboard operations execute on Core 0. If the display panel fails to initialize, the server falls back to headless operation seamlessly.

---

## How It Works

The device boots as its own isolated telecommunication hub. Media (RTP) flows directly **peer-to-peer (P2P)** between the endpoints over the local wireless or wired segment. The board brokers signaling handshakes (`INVITE`, `200 OK`, `ACK`, `BYE`), allowing the microcontroller to coordinate high-bandwidth calls without processing or routing the audio packets themselves.

```
                  ┌──────────────────────┐
                  │  pocket-dial Server  │
                  │   (SIP Signaling)    │
                  └──────────────────────┘
                       ▲            ▲
             INVITE /  │            │  INVITE /
             200 OK    │            │  200 OK
                       ▼            ▼
             ┌───────────┐        ┌───────────┐
             │ Phone 102 │◀══════▶│ Phone 105 │
             │  (Caller) │   RTP  │  (Callee) │
             └───────────┘  Audio └───────────┘
```

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      SipServer                          │
│                                                         │
│  ┌───────────┐    ┌──────────────────┐    ┌──────────┐  │
│  │ UdpServer │───▶│ SipMessageFactory│───▶│Requests- │  │
│  │           │    │                  │    │Handler   │  │
│  │ (recv/    │    │ Parses raw UDP   │    │          │  │
│  │  send)    │◀───│ into SipMessage  │    │ Dispatch │  │
│  │           │    │ or SipSdpMessage │    │ table of │  │
│  │           │    └──────────────────┘    │ handlers │  │
│  └───────────┘                            │          │  │
│                                           │ Manages: │  │
│                   ┌──────────────────┐    │ _clients │  │
│                   │    SipClient     │◀───│_sessions │  │
│                   │ (number + addr)  │    └──────────┘  │
│                   └──────────────────┘          │       │
│                                                 ▼       │
│                   ┌──────────────────┐    ┌──────────┐  │
│                   │     Session      │◀───│ Outbox   │  │
│                   │ (callID, state,  │    │ Queue    │  │
│                   │  src, dest, P2P) │    │ (Lock-   │  │
│                   └──────────────────┘    │  free)   │  │
│                                           └──────────┘  │
└─────────────────────────────────────────────────────────┘
```

---

## Project Structure

```
pocket-dial/
├── CMakeLists.txt              # Dual-mode build: desktop (CMake) or ESP-IDF
├── ISSUES.md                   # Roadmap & Architectural Concurrency Issues
├── CHANGELOG.md                # Detailed release notes and issue fixes
├── LICENSE                     # MIT
├── README.md                   # Developer Manual (You are here)
├── main.cpp                    # Desktop entry point (cxxopts CLI)
├── main/
│   ├── CMakeLists.txt          # ESP-IDF component builder (wifi/eth/display targets)
│   ├── esp_main.cpp            # ESP32-S3 entry point (Wi-Fi AP transport)
│   ├── esp_main_eth.cpp        # ESP32-S3 entry point (W5500 Ethernet transport)
│   ├── esp_main_display.cpp    # ESP32-S3 entry point (Guition AXS15231B touch display target)
│   ├── drivers/                # Low-level QSPI panel and touch screen driver layer
│   │   ├── esp_lcd_axs15231b.c # High-performance esp_lcd driver
│   │   └── esp_lcd_axs15231b.h
│   ├── ui/                     # Interactive LVGL 8.3 switchboard dashboard
│   │   ├── ui.cpp              # CGA interface, touch press router, log terminal
│   │   └── ui.h
│   └── wifi/                   # DNS/HTTP redirect captive portal wizard
│       ├── DnsServer.cpp
│       └── DnsServer.hpp
├── sketches/                   # Self-contained Arduino IDE sketches
│   ├── SipServer/              # Generic ESP32-S3 Wi-Fi SoftAP
│   ├── SipServerETH/           # Waveshare ESP32-S3-ETH (W5500)
│   ├── SipServer_T_ETH_Lite_W5500/   # LilyGO T-ETH-Lite W5500
│   ├── SipServer_T_POE_Pro_LAN8720/  # LilyGO T-PoE-Pro LAN8720
│   ├── SipServer_JC3248W535/   # Guition 3.5" IPS Smart Display [DEPRECATED]
│   ├── MinimalTest/            # Diagnostic: PSRAM / heap allocation verification
│   └── PinFuzzer/              # Diagnostic: GPIO scanning tool
├── src/                        # Core SIP Engine (Cross-Platform C++)
│   ├── Helpers/
│   │   ├── IDGen.hpp           # Thread-safe alphanumeric generator
│   │   ├── UdpServer.cpp       # Threaded, platform-abstracted UDP socket
│   │   └── UdpServer.hpp
│   └── SIP/
│       ├── SipMessageTypes.h   # SIP method & status string definitions
│       ├── SipMessage.cpp      # O(n) index-walking header parser
│       ├── SipMessage.hpp
│       ├── SipSdpMessage.cpp   # SDP body extraction
│       ├── SipSdpMessage.hpp
│       ├── SipMessageFactory.cpp # RTTI-free dispatch factory
│       ├── SipMessageFactory.hpp
│       ├── SipClient.hpp       # Active registrar model
│       ├── SipClient.cpp
│       ├── Session.hpp         # Call state machine tracker
│       ├── Session.cpp
│       ├── RequestsHandler.cpp # Signaling logic & client/session registry
│       ├── RequestsHandler.hpp
│       ├── SipServer.cpp       # Orchestrator
│       └── SipServer.hpp
```

---

## Supported SIP Methods & Responses

| Method/Response | Enum Constant | Handler | Description |
|---|---|---|---|
| `REGISTER` | `SipMessageTypes::REGISTER` | `onRegister` | Adds or updates client binding in the registrar. |
| `INVITE` | `SipMessageTypes::INVITE` | `onInvite` | Initiates a call session. Intercepts `777` / `999`. |
| `ACK` | `SipMessageTypes::ACK` | `onAck` | Acknowledges session completion. |
| `BYE` | `SipMessageTypes::BYE` | `onBye` | Terminates an active call. |
| `CANCEL` | `SipMessageTypes::CANCEL` | `onCancel` | Aborts a pending call during ringing. |
| `100 Trying` | `SipMessageTypes::TRYING` | `onTrying` | Provisional: call is being routed. |
| `180 Ringing` | `SipMessageTypes::RINGING` | `onRinging` | Provisional: device alerting. Ringing SDP stripped. |
| `200 OK` | `SipMessageTypes::OK` | `onOk` | Final success. Closes signaling handshakes. |
| `404 Not Found` | `SipMessageTypes::NOT_FOUND` | — | Callee number is not currently registered. |
| `480 Temp Unavailable` | `SipMessageTypes::UNAVAILABLE` | `onUnavailable` | Callee is out of range or unregistered. |
| `486 Busy Here` | `SipMessageTypes::BUSY` | `onBusy` | Callee rejected the call. |
| `487 Request Terminated`| `SipMessageTypes::REQUEST_TERMINATED` | `onReqTerminated` | Call aborted via CANCEL. |

---

## Virtual Extensions (`777` & `999`)

pocket-dial includes two built-in virtual extensions to test network media paths and deploy emergency broadcast systems easily:

### 1. Echo Loopback Test (`777`)
* When an endpoint dials `777`, `RequestsHandler` intercepts the `INVITE` and immediately answers `200 OK` using **the caller's own SDP connection information**.
* This forces standard softphones and IP phones to open their RTP channels and stream their output packets back to their own receive ports.
* Resolves the need for server-side audio transcoding or DSP, producing a zero-latency hardware echo test.

### 2. Parallel Intercom / Emergency Broadcast (`999`)
* Dialing `999` invokes parallel forking. The server registers a broadcast session, saves the caller's `INVITE`, and cloning it, dispatches concurrent `INVITE` requests to **every registered extension** on the network (excluding the caller).
* **Auto-Answer Injection**: Injects VoIP-standard auto-answer headers into the outgoing forks:
  * `Call-Info: <sip:any>;answer-after=0`
  * `Alert-Info: info=alert-autoanswer`
  * `Alert-Info: answer-after=0`
  * `Alert-Info: intercom=true`
  * `P-Auto-Answer: normal`
* **Target-URI Rewriting**: Rewrites the Request-URI and `To` headers dynamically to use the target's unique extension rather than `999` (essential for phone-side SIP validation rules).
* **Race Connection**: The first device to answer with `200 OK` is connected to the caller. The server forwards its SDP and immediately fires off a `CANCEL` to all losing ringing targets so their alarms cease instantly. If an endpoint answers too late, the server sends an inline `BYE` to close its session cleanly.

---

## VoIP Interoperability & SDP Stripping

During testing with strict, professional SIP terminals (like Yealink IP phones), standard softphone behaviors can introduce early media loops or codec failures. pocket-dial applies two core signaling rules to guarantee perfect voice paths:

1. **Ringing SDP Stripper (`clearBody`)**: When forwarding a `180 Ringing` packet, any copy of the caller's SDP is erased via `ringing->clearBody()`. The body is removed and the `Content-Length` header is rewritten to `0`. This prevents phones from opening pre-handshake early media RTP loops, avoiding local echo loops and ensuring the call transitions cleanly to "Connected" upon answering.
2. **Strict Codec Enforcement (`enforceG711`)**: Strips non-G.711 audio configurations (like HD G.722 or Opus codecs) from forked `INVITE` and `200 OK` SDP packages, locking the media session to **PCMU / PCMA G.711** (`0 8 101`). This eliminates dead air and codec negotiation failures during auto-answers.

---

## Automated Testing & Remote Control

To facilitate automated signaling tests and validation in lab networks without physical button presses, the directory contains:

### `scratch/yealink_controller.py`
A comprehensive Python CLI tool that exploits the remote control **Action URI** endpoints on Yealink IP phones. It bypasses Web UI form handshakes and security limits by performing secure Basic Auth and anti-CSRF token-signed RSA/AES configuration provisioning uploads.

#### Basic Usage:
```bash
# Force the T29G phone (Extension 102 / IP 181) to dial the emergency broadcast
python scratch/yealink_controller.py 181 --dial 999

# Force the older model (Extension 105 / IP 205) to dial the echo loopback
python scratch/yealink_controller.py 205 --dial 777

# Send key press codes to simulate key actions (like toggling Speakerphone or Mute)
python scratch/yealink_controller.py both --key SPEAKER
python scratch/yealink_controller.py 181 --key CANCEL
```

---

## Building

### ESP32-S3 (ESP-IDF v5.3)
Required for professional firmware development. Installs and registers the device under native ESP-IDF structures.

```bash
# Set compilation target chip
idf.py set-target esp32s3

# (Optional) Open configuration menu
idf.py menuconfig

# Compile, flash to the board, and monitor serial output
idf.py build flash monitor
```

---

### JC3248W535EN Smart Display
To compile the high-performance native touchscreen switchboard UI:

```bash
# Sourced ESP-IDF environment required

# 1. Select the ESP32-S3 target chip
idf.py set-target esp32s3

# 2. Build using the display transport compile flag
idf.py -D SIP_TRANSPORT=display build

# 3. Flash and monitor (replace COM3 with your actual serial port)
idf.py -p COM3 -D SIP_TRANSPORT=display flash monitor
```
> [!IMPORTANT]
> The target allocates two 307.2 KB 16-bit double buffers in external RAM. The mandatory configurations (`FlashMode=qio` and `OPI PSRAM`) are automatically handled via `sdkconfig.defaults`.

---

### W5500 Ethernet & PoE
To build the server utilizing high-speed wired Ethernet (e.g. Waveshare ESP32-S3-ETH or LilyGO T-ETH-Lite):

```bash
# Build using the Ethernet transport compile flag
idf.py -D SIP_TRANSPORT=eth build

# Flash and monitor
idf.py -p COM3 -D SIP_TRANSPORT=eth flash monitor
```

---

### ESP32-S3 (Arduino IDE)
If compiling within an Arduino framework:

1. Open Boards Manager and install the **ESP32 board package** by Espressif.
2. Open `sketches/SipServer/SipServer.ino` (or target `SipServerETH.ino` for wired PoE W5500 connections).
3. Set your target board to **ESP32S3 Dev Module** (or specific board model).
4. Configure **Flash Mode = QIO 80MHz** and **PSRAM = OPI PSRAM** (mandatory for display sketches).
5. Click **Upload**.

---

### Desktop Mode

#### 1. Linux
Useful for debugging SIP transactions or testing softphone connections lock-free without flash waiting periods:
```bash
mkdir build && cd build
cmake ..
make
```

#### 2. Windows
Open a Visual Studio Developer Command Prompt:
```powershell
mkdir build && cd build
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" ..
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build . --config Release
```

#### Running the Desktop Binary:
The compiled executable requires the binding IP interface as an argument:
```bash
# Bind the SIP server to your local network interface IP
./build_desktop/Release/SipServer.exe --ip=192.168.12.225

# Optional: Override the default port (5060)
./build_desktop/Release/SipServer.exe --ip=192.168.12.225 --port=5080
```

---

## API Reference

### Core Classes

#### `SipServer`
Manages the lifetime of top-level sockets and binds the handler callbacks.
```cpp
SipServer(std::string ip, int port = 5060, int httpPort = 8080);
```

#### `UdpServer`
Handles socket execution loops, threading abstractions, and core task pinning.
```cpp
UdpServer(std::string ip, int port, OnNewMessageEvent event);
void startReceive(); // Spawns background thread
int send(sockaddr_in addr, const std::string& buffer);
```

#### `SipMessage` & `SipSdpMessage`
Parses headers and extracts SDP details through an O(n) non-mutating index-walking algorithm.
* **`clearBody()`**: Purges message bodies and rewrites content lengths.
* **`enforceG711()`**: Forces audio codecs list to `0 8 101`.
* **`addHeader(name, value)`**: Injects new SIP headers before the content boundary.

---

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
