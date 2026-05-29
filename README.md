# pocket-dial
<!-- README.md: Issues #21 and #26 resolved. -->

> [!TIP]
> **TL;DR (Zero-Friction One-Line Installers)**
> * **Linux / macOS Bash**:
>   ```bash
>   sh -c 'set -eu; TAG=v1.0.0; SHA=423411c556378ab0725011a16df3d3fc8bb6f798af3bdc8263e48aee77ec3f5c; URL="https://github.com/GlomarGadaffi/pocket-dial/releases/download/$TAG/pocket-dial-$TAG.zip"; T=$(mktemp -d); trap "rm -rf $T" EXIT; curl -fsSL "$URL" -o "$T/pd.zip"; (command -v sha256sum >/dev/null && echo "$SHA  $T/pd.zip" | sha256sum -c -) || (command -v shasum >/dev/null && echo "$SHA  $T/pd.zip" | shasum -a 256 -c -) || { echo "checksum FAILED"; exit 1; }; unzip -q "$T/pd.zip" -d "$T"; cd "$T/pocket-dial-1.0.0" && chmod +x quickstart.sh && ./quickstart.sh'
>   ```
> * **Windows (CMD / PowerShell / Run Dialog)**:
>   ```cmd
>   powershell -c "& { $ErrorActionPreference='Stop'; $tag='v1.0.0'; $sha='423411c556378ab0725011a16df3d3fc8bb6f798af3bdc8263e48aee77ec3f5c'; $url=\"https://github.com/GlomarGadaffi/pocket-dial/releases/download/$tag/pocket-dial-$tag.zip\"; $tmp=Join-Path $env:TEMP ([guid]::NewGuid()); New-Item -ItemType Directory $tmp | Out-Null; $zip=Join-Path $tmp 'pd.zip'; Invoke-WebRequest $url -OutFile $zip; if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $sha) { throw 'checksum mismatch' }; Expand-Archive $zip -DestinationPath $tmp; Set-Location (Join-Path $tmp 'pocket-dial-1.0.0'); .\quickstart.bat }"
>   ```
> * **ESP32 / CYD (Arduino)**: Open `sketches/SipServer/SipServer.ino` in Arduino IDE, hit **Upload**, connect to the `esp32-sipserver` AP, and open `http://192.168.4.1/`!
> 
> That's it! Running the one-liner installer automatically downloads, extracts, compiles the C++17 core, launches the server, and fires up the dashboard in your default browser.

A C++17 SIP registrar and stateless proxy designed for flat, trusted local networks. It runs on desktop environments (Windows/Linux) and embedded targets (ESP32-S3 via Wi-Fi or W5500 PoE Ethernet) and includes a CGA CRT retro-styled web administration console.

Forked and extended from [BarGabriel/SipServer](https://github.com/BarGabriel/SipServer).

---

## Features

### 1. SIP Signaling & Call Setup
* **User Registration**: Supports the `REGISTER` transaction. Saves registered endpoints dynamically in memory. Unregistration is handled via `expires=0`. The registrar does not perform credential validation — any username/password is accepted. This is a deliberate scoping decision for flat, trusted LANs, not a security feature; see [Security Specifications](#security-specifications) before deploying.
* **Stateless Proxy Routing**: Routes standard call setups via `INVITE`, `TRYING`, `RINGING`, `OK`, `ACK`, `CANCEL`, `BYE`, and error states (`486 Busy Here`, `480 Temporarily Unavailable`, `404 Not Found`).
* **Direct RTP Flow**: Media (audio) flows peer-to-peer directly between endpoints, keeping the signaling server lightweight and suitable for constrained hardware.

### 2. LAN & Auto-Routing Abstraction
* Binds to `0.0.0.0` by default to listen on all local interfaces.
* Resolves active LAN routes at startup to determine the primary host interface IP and automatically substitutes it within SIP headers (`Via` and `Contact`) for out-of-the-box local network communication.

### 3. Web Administration Console
* Hosted on port `8080` (desktop) or `80` (embedded).
* Retro CGA-styled interface (`#0000AA` palette) featuring:
  * **Interactive Terminal**: A CLI terminal supporting commands such as `help`, `dir`, `registered`, `sessions`, `uptime`, `oracle`, `ver`, and `ascii`.
  * **Live Status Dashboard**: Grids displaying registered extensions, active session states, and diagnostic packet counters.
  * **WiFi Manager**: Onboard Wi-Fi setup module to scan and connect the device to local networks (on embedded platforms).

## Quickstart (Desktop)

You can download, configure, build, and run the server with a **single, zero-dependency one-liner installer**:

### Linux / macOS (Bash)
Open a terminal and run:
```bash
sh -c 'set -eu; TAG=v1.0.0; SHA=423411c556378ab0725011a16df3d3fc8bb6f798af3bdc8263e48aee77ec3f5c; URL="https://github.com/GlomarGadaffi/pocket-dial/releases/download/$TAG/pocket-dial-$TAG.zip"; T=$(mktemp -d); trap "rm -rf $T" EXIT; curl -fsSL "$URL" -o "$T/pd.zip"; (command -v sha256sum >/dev/null && echo "$SHA  $T/pd.zip" | sha256sum -c -) || (command -v shasum >/dev/null && echo "$SHA  $T/pd.zip" | shasum -a 256 -c -) || { echo "checksum FAILED"; exit 1; }; unzip -q "$T/pd.zip" -d "$T"; cd "$T/pocket-dial-1.0.0" && chmod +x quickstart.sh && ./quickstart.sh'
```

### Windows (CMD / PowerShell / Run Dialog)
Open a terminal or run dialog and paste:
```cmd
powershell -c "& { $ErrorActionPreference='Stop'; $tag='v1.0.0'; $sha='423411c556378ab0725011a16df3d3fc8bb6f798af3bdc8263e48aee77ec3f5c'; $url=\"https://github.com/GlomarGadaffi/pocket-dial/releases/download/$tag/pocket-dial-$tag.zip\"; $tmp=Join-Path $env:TEMP ([guid]::NewGuid()); New-Item -ItemType Directory $tmp | Out-Null; $zip=Join-Path $tmp 'pd.zip'; Invoke-WebRequest $url -OutFile $zip; if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $sha) { throw 'checksum mismatch' }; Expand-Archive $zip -DestinationPath $tmp; Set-Location (Join-Path $tmp 'pocket-dial-1.0.0'); .\quickstart.bat }"
```

---

## Local Development Execution (Inside Repository)

If you have cloned the repository locally:

### Windows (CMD)
Double-click `quickstart.bat` or run:
```cmd
quickstart.bat
```

### Linux / macOS (Bash)
```bash
chmod +x quickstart.sh && ./quickstart.sh
```
```bash
mkdir build
cd build
cmake ..
cmake --build . --config Release
# Run executable
.\Release\SipServer.exe
```

### Linux / macOS
```bash
mkdir build
cd build
cmake ..
make
./SipServer
```

On boot, the server outputs local interface bindings and network details:
```text
SIP server started on 0.0.0.0:5060 (LAN IP: 192.168.12.116)
CGA CRT Dashboard: http://localhost:8080/
Remote LAN Access:  http://192.168.12.116:8080/
Press ENTER to stop...
```

---

## Embedded Targets (ESP32-S3)

The project supports both ESP-IDF and Arduino IDE configurations.

### ESP-IDF (W5500 Ethernet or Wi-Fi AP)
Requires [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/).

```bash
idf.py set-target esp32s3
# For Ethernet (PoE W5500)
idf.py build flash monitor

# For Wi-Fi Access Point (SoftAP)
idf.py -D SIP_TRANSPORT=wifi build flash monitor
```

### Arduino IDE
Pre-configured sketches are available under the `sketches/` directory:
* `sketches/SipServer/SipServer.ino` — Wireless SoftAP variant (ESP32 / CYD).
* `sketches/SipServerETH/SipServerETH.ino` — Generic Wired W5500 SPI Ethernet variant (Waveshare S3-PoE).
* `sketches/SipServer_T_ETH_Lite_W5500/SipServer_T_ETH_Lite_W5500.ino` — LilyGO T-ETH-Lite ESP32-S3 W5500 board.
* `sketches/SipServer_T_POE_Pro_LAN8720/SipServer_T_POE_Pro_LAN8720.ino` — LilyGO T-POE-Pro LAN8720 board.

To compile in the Arduino IDE, set the board to **ESP32S3 Dev Module** and import the source files located under `src/Helpers` and `src/SIP` to the sketch.

---

## Project Structure

```text
pocket-dial/
├── CMakeLists.txt              # Multi-platform desktop CMake or ESP-IDF delegate
├── sketches/                   # Board-specific Arduino IDE sketches
│   ├── SipServer/              # Wireless SoftAP variant (ESP32 Wi-Fi / CYD)
│   │   └── SipServer.ino
│   ├── SipServerETH/           # Generic W5500 Ethernet variant (Waveshare PoE)
│   │   └── SipServerETH.ino
│   ├── SipServer_T_ETH_Lite_W5500/ # LilyGO T-ETH-Lite ESP32-S3 W5500
│   │   └── SipServer_T_ETH_Lite_W5500.ino
│   └── SipServer_T_POE_Pro_LAN8720/ # LilyGO T-POE-Pro LAN8720 (RMII)
│       └── SipServer_T_POE_Pro_LAN8720.ino
├── main.cpp                    # Desktop application entry point
├── main/
│   ├── CMakeLists.txt          # ESP-IDF component manifest
│   ├── esp_main.cpp            # ESP-IDF entry — Wi-Fi SoftAP
│   └── esp_main_eth.cpp        # ESP-IDF entry — W5500 Ethernet
├── src/
│   ├── Helpers/
│   │   ├── cxxopts.hpp         # Desktop command-line parser
│   │   ├── IDGen.hpp           # Thread-safe random ID generator
│   │   ├── IPHelper.hpp        # Dynamic LAN IP resolution utility
│   │   ├── HttpServer.hpp/cpp  # Web administration TCP server and API endpoints
│   │   ├── index_html.h        # Self-contained administration dashboard HTML carrier
│   │   └── UdpServer.hpp/cpp   # Network socket wrapper (POSIX, Winsock, and lwIP)
│   └── SIP/
│       ├── SipServer.hpp/cpp   # Core server orchestrator
│       ├── RequestsHandler.hpp/cpp # SIP transaction dispatcher and state controller
│       ├── Session.hpp/cpp     # Call session state machine
│       ├── SipClient.hpp/cpp   # Registered endpoint profiles
│       ├── SipMessage.hpp/cpp  # Base SIP message parser
│       ├── SipMessageTypes.h   # SIP method and status line constants
│       ├── SipSdpMessage.hpp/cpp # Derived SDP body parser
│       └── SipMessageFactory.hpp/cpp # SIP message factory and classifier
```

---

## System Architecture

```text
                       ┌──────────────────────┐
                       │      SipServer       │
                       │ (top-level orchestr.)│
                       └─────────┬────────────┘
                                 │ owns
             ┌───────────────────┼───────────────────┐
             ▼                   ▼                   ▼
      ┌────────────┐    ┌──────────────────┐  ┌──────────────────┐
      │ UdpServer  │    │ SipMessageFactory│  │ RequestsHandler  │
      │            │    │                  │  │                  │
      │ recv/send  │    │ parses raw UDP   │  │ method dispatch  │
      │ threaded   │    │ → SipMessage or  │  │ session state    │
      │ loop       │    │   SipSdpMessage  │  │ client registry  │
      └─────┬──────┘    └──────────────────┘  └────────┬─────────┘
            │                                          │
            │  raw bytes in/out                        │  manages
            ▼                                          ▼
       network (Wi-Fi or W5500)              ┌──────────────────────┐
                                             │  Session  /  Client  │
                                             └──────────────────────┘
```

---

## Security Specifications

> ⚠️ **This server performs no authentication and is intended for flat, trusted local networks only. Do NOT expose SIP (UDP 5060) or the administration console (TCP 8080/80) directly to the public Internet.**

The server is designed for flat, private networks:
* **No Authentication**: The registrar does not validate credentials — any username/password combination is accepted. This removes provisioning overhead and keeps the codebase lightweight, which suits constrained hardware on a trusted LAN. It is a known limitation, not a hardening feature: anyone who can reach the SIP port can register as any extension.
* **Decentralized Media**: Voice packets flow peer-to-peer, keeping memory footprint low.

---

## License

Distributed under the **MIT License**. See [LICENSE](LICENSE) for details.
