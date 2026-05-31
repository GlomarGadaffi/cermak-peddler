# pocket-dial

A self-contained SIP PBX on a $10 board. Flash an ESP32-S3, connect two softphones to its SoftAP, and place a call. No router, no upstream, no infrastructure — the registrar, the proxy, the DHCP server, and the access point all run on the chip.

Now with **smart display support**: the [JC3248W535EN](#jc3248w535en-smart-display) variant adds a 3.5" IPS touchscreen running a retro CGA-style switchboard dashboard — live call status, Wi-Fi QR join, and hardware reboot, all on the panel.

Written in C++17. Also builds for **Linux** and **Windows**, which is useful for iterating on the SIP logic without a flash cycle — but the embedded target is the point.

## What it's for

* Ad-hoc voice between endpoints with zero infrastructure — lab, field, festival, CTF, air-gapped site.
* Embedded VoIP / SIP behavior research without standing up Asterisk or FreeSWITCH.
* A complete, demonstrable, end-to-end SIP stack you can hold in one hand.

\---

## Table of Contents

* [Features](#features)
* [How It Works](#how-it-works)
* [Architecture](#architecture)
* [Project Structure](#project-structure)
* [Supported SIP Methods \& Responses](#supported-sip-methods--responses)
* [Building](#building)

  * [ESP32-S3 (ESP-IDF)](#esp32-s3-esp-idf)
  * [ESP32-S3 (Arduino IDE)](#esp32-s3-arduino-ide)
  * [JC3248W535EN Smart Display](#jc3248w535en-smart-display)
  * [Desktop (Linux)](#desktop-linux)
  * [Desktop (Windows)](#desktop-windows)
* [Usage](#usage)

  * [ESP32-S3 Mode](#esp32-s3-mode)
  * [JC3248W535EN Mode](#jc3248w535en-mode)
  * [Desktop Mode](#desktop-mode)
* [Call Flow](#call-flow)
* [API Reference](#api-reference)
* [Configuration](#configuration)
* [License](#license)
* [Credits](#credits)

\---

## Features

* **Standalone AP Mode (ESP32-S3)** — On power-up the board brings up an open Wi-Fi access point (`esp32-sipserver`), runs a DHCP server, and binds the SIP registrar to `192.168.4.1:5060`. Nothing else required: connect, register, call.
* **Smart Display Dashboard (JC3248W535EN)** — On the Guition 3.5" IPS touchscreen variant, the board renders a retro CGA CRT-style switchboard directly on the 320×480 panel: live uptime, station count, registered extensions, active sessions, and a scrolling console log. Tap to show a Wi-Fi QR code for instant phone connection, cycle color themes (CGA Blue / Amber CRT / Green Phosphor), or trigger a hardware reboot with confirmation.
* **SIP User Registration** — Clients send `REGISTER`; the server responds `200 OK` and maintains an in-memory registry of active extensions. Re-registration refreshes the client's address (NAT rebind safe).
* **Full Call Signaling** — Proxies `INVITE`, `TRYING`, `RINGING`, `200 OK`, `ACK`, `BYE`, `CANCEL`, `486 Busy Here`, `480 Temporarily Unavailable`, and `487 Request Terminated` between registered endpoints.
* **SDP Media Negotiation** — Parses `application/sdp` bodies to extract RTP port information, letting endpoints establish direct peer-to-peer media streams. Signaling goes through the board; audio does not.
* **Session State Machine** — Tracks per-call state (`Invited → Connected → Bye`) with cleanup of orphaned sessions on cancel, busy, or unavailable.
* **Headless Fallback** — If the display driver fails to initialize on a display-equipped board, the SIP server continues operating in headless mode. The `displayActive` flag gates all UI code so the PBX never crashes due to a panel issue.
* **Cross-Platform Core** — A single codebase compiles against POSIX sockets (Linux), Winsock2 (Windows), and lwIP (ESP32-S3 via ESP-IDF and Arduino IDE).

\---

## How It Works

The device boots as its own network. There is no DHCP lease to wait on, no SIP provider to register against, and no internet path in the call. Two phones associate to the SoftAP, register their extensions with the on-board registrar, and the proxy brokers the `INVITE`/`OK`/`ACK` handshake between them. Media (RTP) flows directly phone-to-phone over the local Wi-Fi segment.

The result is a closed voice cell — everything needed to place a call lives on the microcontroller, and nothing leaves it.

\---

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
│  └───────────┘    └──────────────────┘    │ handlers │  │
│                                           │          │  │
│                   ┌──────────────────┐    │ Manages: │  │
│                   │    SipClient     │◀───│ \_clients │  │
│                   │ (number + addr)  │    │ \_sessions│  │
│                   └──────────────────┘    └──────────┘  │
│                                                         │
│                   ┌──────────────────┐                   │
│                   │     Session      │                   │
│                   │ (callID, state,  │                   │
│                   │  src, dest, RTP) │                   │
│                   └──────────────────┘                   │
└─────────────────────────────────────────────────────────┘
```

**Data flow:**

1. `UdpServer` receives a raw UDP datagram and invokes `SipServer::onNewMessage`.
2. `SipMessageFactory` inspects the payload: if it contains `application/sdp`, it creates a `SipSdpMessage`; otherwise a plain `SipMessage`.
3. The parsed message is dispatched to `RequestsHandler::handle`, which looks up the message type in a `std::unordered\_map` of handler functions.
4. The appropriate handler (`onInvite`, `onRegister`, `onBye`, …) processes the request, mutates headers as needed, and calls `endHandle` to route the response back through `UdpServer::send`.

\---

## Project Structure

```
pocket-dial/
├── CMakeLists.txt              # Dual-mode build: desktop (CMake) or ESP-IDF
├── main.cpp                    # Desktop entry point (cxxopts CLI)
├── main/
│   ├── CMakeLists.txt          # ESP-IDF component registration
│   └── esp_main.cpp            # ESP32-S3 entry point (SoftAP + FreeRTOS)
├── sketches/
│   ├── SipServer/              # Arduino: generic ESP32-S3 Wi-Fi SoftAP
│   ├── SipServerETH/           # Arduino: Waveshare ESP32-S3-ETH (W5500)
│   ├── SipServer_T_ETH_Lite_W5500/   # Arduino: LilyGO T-ETH-Lite W5500
│   ├── SipServer_T_POE_Pro_LAN8720/  # Arduino: LilyGO T-PoE-Pro LAN8720
│   ├── SipServer_JC3248W535/   # Arduino: Guition 3.5" IPS Smart Display ⬅ NEW
│   ├── MinimalTest/            # Diagnostic: PSRAM / heap / buffer test
│   └── PinFuzzer/              # Diagnostic: GPIO / QSPI / I2C pin probe
├── src/
│   ├── Helpers/
│   │   ├── cxxopts.hpp         # CLI argument parser (desktop only)
│   │   ├── IDGen.hpp           # Thread-safe random alphanumeric ID generator
│   │   ├── UdpServer.hpp       # UDP socket abstraction (header)
│   │   └── UdpServer.cpp       # UDP socket abstraction (implementation)
│   └── SIP/
│       ├── SipMessageTypes.h   # SIP method & response string constants
│       ├── SipMessage.hpp      # Base SIP message parser (header)
│       ├── SipMessage.cpp      # Base SIP message parser (implementation)
│       ├── SipSdpMessage.hpp   # SDP-aware SIP message (header)
│       ├── SipSdpMessage.cpp   # SDP body parser (implementation)
│       ├── SipMessageFactory.hpp  # Factory: SipMessage vs SipSdpMessage
│       ├── SipMessageFactory.cpp
│       ├── SipClient.hpp       # Registered client model (number + address)
│       ├── SipClient.cpp
│       ├── Session.hpp         # Call session state machine (header)
│       ├── Session.cpp         # Call session state machine (implementation)
│       ├── RequestsHandler.hpp # SIP method dispatch & client/session mgmt
│       ├── RequestsHandler.cpp
│       ├── SipServer.hpp       # Top-level server orchestrator (header)
│       └── SipServer.cpp       # Top-level server orchestrator (implementation)
├── CHANGELOG.md
├── LICENSE                     # MIT
└── README.md                   # ← You are here
```

\---

## Supported SIP Methods \& Responses

|Type|Constant|Handler|Description|
|-|-|-|-|
|`REGISTER`|`SipMessageTypes::REGISTER`|`onRegister`|Adds/updates a client in the registry. Responds `200 OK`.|
|`INVITE`|`SipMessageTypes::INVITE`|`onInvite`|Initiates a call. Creates a `Session`. Forwards to callee.|
|`ACK`|`SipMessageTypes::ACK`|`onAck`|Acknowledges a final response. Cleans up failed sessions.|
|`BYE`|`SipMessageTypes::BYE`|`onBye`|Terminates an active call. Forwards to the other party.|
|`CANCEL`|`SipMessageTypes::CANCEL`|`onCancel`|Cancels a pending INVITE.|
|`SIP/2.0 100 Trying`|`SipMessageTypes::TRYING`|`onTrying`|Provisional: call is being routed.|
|`SIP/2.0 180 Ringing`|`SipMessageTypes::RINGING`|`onRinging`|Provisional: destination phone is ringing.|
|`SIP/2.0 200 OK`|`SipMessageTypes::OK`|`onOk`|Success. On INVITE OK, extracts SDP and completes session.|
|`SIP/2.0 404 Not Found`|`SipMessageTypes::NOT\_FOUND`|—|Sent when destination number is not registered.|
|`SIP/2.0 480 Temporarily Unavailable`|`SipMessageTypes::UNAVAILABLE`|`onUnavailable`|Destination unavailable.|
|`SIP/2.0 486 Busy Here`|`SipMessageTypes::BUSY`|`onBusy`|Destination is busy.|
|`SIP/2.0 487 Request Terminated`|`SipMessageTypes::REQUEST\_TERMINATED`|`onReqTerminated`|INVITE was cancelled before completion.|

\---

## Building

The repository builds two ways from one tree. The root `CMakeLists.txt` auto-detects the `IDF\_PATH` environment variable: when present, it delegates to ESP-IDF's component build system; when absent, it builds the standard desktop executable.

### ESP32-S3 (ESP-IDF)

The primary target. Requires [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) installed and sourced.

```
# Set your target chip
idf.py set-target esp32s3

# (Optional) Adjust settings via menuconfig
idf.py menuconfig

# Build, flash, and monitor serial output
idf.py build flash monitor
```

### ESP32-S3 (Arduino IDE)

If you prefer Arduino over ESP-IDF:

1. Install the **ESP32 board package** via Boards Manager (search `esp32` by Espressif).
2. Open `sketches/SipServer/SipServer.ino`.
3. Select your board: **Tools → Board → ESP32S3 Dev Module** (or your specific LilyGO variant).
4. Select your port: **Tools → Port → COMx**.
5. Click **Upload**.

The sketch uses Arduino's `WiFi.h` to start the SoftAP, then instantiates `SipServer` directly in `setup()`. Each sketch directory includes its own copies of the SIP source files for self-contained compilation.

> **Note:** The Arduino sketches and the ESP-IDF `esp_main.cpp` are independent entry points into the same SIP engine. Use whichever toolchain you prefer — runtime behavior is identical.

### JC3248W535EN Smart Display

For the **Guition JC3248W535EN** (3.5" IPS touchscreen, ESP32-S3, AXS15231B QSPI display controller):

1. Install the **ESP32 board package** via Boards Manager.
2. Install the following libraries via Library Manager:
   * **JC3248W535EN-Touch-LCD** by AudunKodehode
   * **Arduino_GFX Library** by Moon On Our Nation (installed as dependency)
3. Open `sketches/SipServer_JC3248W535/SipServer_JC3248W535.ino`.
4. Configure **Tools** with these **mandatory** settings:

   | Setting | Value |
   |---------|-------|
   | Board | ESP32S3 Dev Module |
   | PSRAM | **OPI PSRAM** |
   | Flash Mode | **QIO 80MHz** |
   | Flash Size | 16MB (128Mb) |
   | USB CDC On Boot | Enabled |

5. Select your port and click **Upload**.

> **⚠️ PSRAM = OPI and Flash Mode = QIO are mandatory.** The AXS15231B display driver allocates a 307.2 KB frame buffer in PSRAM during initialization. Without OPI PSRAM enabled, the board will panic on boot.

#### Pin Mapping

| Function | GPIO | Notes |
|----------|-------|-------|
| QSPI PCLK | 47 | Display clock |
| QSPI DATA0–3 | 21, 48, 40, 39 | QSPI data bus |
| QSPI CS | 45 | Chip select |
| Touch SDA | 4 | I2C data |
| Touch SCL | 8 | I2C clock |
| Touch INT | 11 | Interrupt |
| Touch RST | 12 | Reset |
| Touch Addr | — | `0x3B` |
| Backlight | 1 | Active HIGH |
| Battery ADC | 5 | Voltage divider |

#### Diagnostic Sketches

Two additional sketches are provided for hardware bring-up and debugging:

* **`sketches/MinimalTest/`** — PSRAM detection, heap reporting, and frame buffer allocation test. Flash this first to verify your board settings are correct.
* **`sketches/PinFuzzer/`** — Scans GPIO pins to verify QSPI data lines, I2C touch bus, and backlight control. Useful for confirming pin assignments on unknown board revisions.

### Desktop (Linux)

For developing the SIP logic without flashing hardware.

```
mkdir build && cd build
cmake ..
make
```

### Desktop (Windows)

From a Visual Studio Developer Command Prompt:

```
mkdir build && cd build
cmake ..
msbuild SipServer.sln
```

\---

## Usage

### ESP32-S3 Mode

No configuration required. On power-up the board will:

1. Initialize Non-Volatile Storage (NVS) for Wi-Fi calibration data.
2. Start a **Wi-Fi Soft Access Point**:

   * **SSID:** `esp32-sipserver`
   * **Password:** *(none — open network)*
   * **Channel:** 1
   * **Max clients:** 10
3. Run a **DHCP server** that assigns addresses to connected devices.
4. Bind the SIP server to **`192.168.4.1:5060`** in a dedicated FreeRTOS task (8 KB stack).

To place a call:

1. Flash the firmware to your ESP32-S3 board.
2. On two phones (or laptops), connect to the `esp32-sipserver` Wi-Fi network.
3. In a SIP softphone on each, configure:

   * **Domain / Outbound Proxy:** `192.168.4.1`
   * **Port:** `5060`
   * **Transport:** UDP
4. Register an extension on each, then dial the other.

### JC3248W535EN Mode

Behavior is identical to [ESP32-S3 Mode](#esp32-s3-mode) above, with the addition of the on-screen dashboard. After boot you will see:

* **Status Panel** — Server IP, SIP port, uptime counter, connected stations, registered extensions, and active call sessions — all updating live.
* **[ TAP FOR WI-FI QR CODE ]** — Shows a full-screen QR code that phones can scan to instantly join the `esp32-sipserver` network.
* **[ COLOR ]** — Cycles through three CRT themes: CGA Blue, Amber Phosphor, Green Phosphor.
* **[ REBOOT ]** — Hardware reset with a YES/NO confirmation dialog.
* **Live Console** — Scrolling log at the bottom of the screen showing connection events, SIP registrations, and call state changes in real time.

If the display fails to initialize, the board enters **headless mode** — the SIP server and web dashboard continue running normally, and all logs are still available via Serial and the HTTP dashboard at `http://192.168.4.1`.

### Desktop Mode

```
# Required: --ip specifies the interface to bind
./SipServer --ip=192.168.1.100

# Optional: --port overrides the default SIP port (5060)
./SipServer --ip=192.168.1.100 --port=5080

# Show help
./SipServer --help
```

|Option|Required|Default|Description|
|-|-|-|-|
|`--ip`, `-i`|Yes|—|IP address to bind the UDP listener to|
|`--port`, `-p`|No|`5060`|UDP port to listen on|
|`--help`, `-h`|No|—|Print usage information|

\---

## Call Flow

### Successful Call

```
Caller (1001)          SipServer           Callee (1002)
    │                      │                      │
    │── REGISTER ─────────▶│                      │
    │◀──── 200 OK ─────────│                      │
    │                      │                      │
    │                      │◀──── REGISTER ───────│
    │                      │──────── 200 OK ─────▶│
    │                      │                      │
    │── INVITE (SDP) ─────▶│                      │
    │                      │── INVITE (SDP) ─────▶│
    │                      │                      │
    │                      │◀──── 180 Ringing ────│
    │◀──── 180 Ringing ────│                      │
    │                      │                      │
    │                      │◀──── 200 OK (SDP) ───│
    │◀──── 200 OK (SDP) ───│                      │
    │                      │                      │
    │── ACK ──────────────▶│                      │
    │                      │── ACK ──────────────▶│
    │                      │                      │
    │◀═══════════ RTP Media (direct P2P) ═══════▶│
    │                      │                      │
    │── BYE ──────────────▶│                      │
    │                      │── BYE ──────────────▶│
    │                      │◀──── 200 OK ─────────│
    │◀──── 200 OK ─────────│                      │
    │                      │                      │
```

### Cancelled Call

```
Caller (1001)          SipServer           Callee (1002)
    │                      │                      │
    │── INVITE ───────────▶│── INVITE ───────────▶│
    │                      │◀──── 180 Ringing ────│
    │◀──── 180 Ringing ────│                      │
    │                      │                      │
    │── CANCEL ───────────▶│── CANCEL ───────────▶│
    │                      │◀──── 200 OK ─────────│
    │◀──── 200 OK ─────────│                      │
    │                      │◀── 487 Req Term ─────│
    │◀── 487 Req Term ─────│                      │
    │── ACK ──────────────▶│── ACK ──────────────▶│
    │                      │                      │
```

### Destination Not Found

```
Caller (1001)          SipServer
    │                      │
    │── INVITE ───────────▶│
    │◀── 404 Not Found ────│  (callee not registered)
    │                      │
```

\---

## API Reference

### Core Classes

#### `SipServer`

Top-level orchestrator. Binds a UDP socket and wires the message factory to the request handler.

```cpp
SipServer(std::string ip, int port = 5060);
```

#### `UdpServer`

Platform-abstracted UDP socket with a threaded receive loop.

```cpp
UdpServer(std::string ip, int port, OnNewMessageEvent event);
void startReceive();            // Spawns receiver thread
int send(sockaddr\_in addr, const std::string\& buffer);
```

* `OnNewMessageEvent` = `std::function<void(std::string, sockaddr\_in)>`
* Buffer size: 2048 bytes
* Platforms: Linux (POSIX), Windows (Winsock2), ESP32 (lwIP)

#### `SipMessageFactory`

Creates the correct message subtype based on payload content.

```cpp
std::optional<std::shared\_ptr<SipMessage>> createMessage(std::string message, sockaddr\_in src);
```

Returns `SipSdpMessage` if the body contains `application/sdp`, otherwise `SipMessage`. Returns `std::nullopt` on parse failure.

#### `SipMessage`

Parses and stores SIP header fields from raw datagram text.

|Getter|Returns|
|-|-|
|`getType()`|Request method or response line (e.g. `"INVITE"`, `"SIP/2.0 200 OK"`)|
|`getHeader()`|Full first line of the SIP message|
|`getVia()`|`Via` header value|
|`getFrom()` / `getFromNumber()`|`From` header / extracted extension number|
|`getTo()` / `getToNumber()`|`To` header / extracted extension number|
|`getCallID()`|`Call-ID` header|
|`getCSeq()`|`CSeq` header|
|`getContact()` / `getContactNumber()`|`Contact` header / extracted extension|
|`getSource()`|`sockaddr\_in` of the sending endpoint|
|`toString()`|Reconstructed SIP message with any mutations applied|

All setters (`setHeader`, `setVia`, `setTo`, …) perform in-place replacement on the raw message string, keeping `toString()` consistent.

#### `SipSdpMessage` (extends `SipMessage`)

Additionally parses the SDP body.

|Getter|Returns|
|-|-|
|`getVersion()`|SDP `v=` line|
|`getOriginator()`|SDP `o=` line|
|`getSessionName()`|SDP `s=` line|
|`getConnectionInformation()`|SDP `c=` line|
|`getTime()`|SDP `t=` line|
|`getMedia()`|SDP `m=` line|
|`getRtpPort()`|Extracted RTP port from `m=` line|

#### `SipClient`

Represents a registered SIP endpoint.

```cpp
SipClient(std::string number, sockaddr\_in address);
std::string getNumber() const;
sockaddr\_in getAddress() const;
```

#### `Session`

Tracks the lifecycle of a single call.

```cpp
Session(std::string callID, std::shared\_ptr<SipClient> src, uint32\_t srcRtpPort);
```

**States:** `Invited` → `Connected` | `Busy` | `Unavailable` | `Cancel` | `Bye`

#### `RequestsHandler`

Central dispatch. Maintains the client registry and active session table.

```cpp
RequestsHandler(std::string serverIp, int serverPort, OnHandledEvent onHandledEvent);
void handle(std::shared\_ptr<SipMessage> request);
```

#### `IDGen`

Thread-safe random alphanumeric string generator using `std::mt19937`.

```cpp
static std::string GenerateID(int len);  // e.g. IDGen::GenerateID(9)
```

\---

## Configuration

### ESP32-S3 (ESP-IDF)

Compile-time constants in `main/esp_main.cpp`:

|Define|Default|Description|
|-|-|-|
|`EXAMPLE_ESP_WIFI_SSID`|`"esp32-sipserver"`|Wi-Fi network name|
|`EXAMPLE_ESP_WIFI_PASS`|`""`|Wi-Fi password (empty = open)|
|`EXAMPLE_ESP_WIFI_CHANNEL`|`1`|Wi-Fi channel|
|`EXAMPLE_MAX_STA_CONN`|`10`|Max simultaneous Wi-Fi clients|

The SIP server always binds to `192.168.4.1:5060` (the default ESP-IDF SoftAP gateway address).

### JC3248W535EN

Compile-time constants at the top of `sketches/SipServer_JC3248W535/SipServer_JC3248W535.ino`:

|Constant|Default|Description|
|-|-|-|
|`AP_SSID`|`"esp32-sipserver"`|Wi-Fi network name|
|`AP_PASSWORD`|`""`|Wi-Fi password (empty = open)|
|`AP_CHANNEL`|`1`|Wi-Fi channel|
|`AP_MAX_CLIENTS`|`10`|Max simultaneous Wi-Fi clients|
|`SIP_BIND_IP`|`"192.168.4.1"`|SIP server bind address|
|`SIP_PORT`|`5060`|SIP server port|
|`HTTP_PORT`|`80`|Web dashboard port|

### Desktop

All configuration is via command-line arguments. See [Usage → Desktop Mode](#desktop-mode).

\---

## License

MIT — see [LICENSE](LICENSE).

\---

## Credits

The SIP/SDP parsing and dispatch architecture is inherited from the original [SipServer](https://github.com/BarGabriel/SipServer) by **Bar Gabriel** — the desktop server this project grew out of. The embedded target, SoftAP bring-up, lwIP integration, and Arduino entry point are new here.

The JC3248W535EN display integration uses the [JC3248W535EN-Touch-LCD](https://github.com/AudunKodehode/JC3248W535EN-Touch-LCD) library by **AudunKodehode**, built on top of [Arduino_GFX](https://github.com/moononournation/Arduino_GFX) by **Moon On Our Nation**. Hardware reference from [NorthernMan54/JC3248W535EN](https://github.com/NorthernMan54/JC3248W535EN).

Thank you, Bar, for the clean foundation for lightweight VoIP development.

