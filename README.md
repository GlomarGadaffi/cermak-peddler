# pocket-dial

A premium, compact SIP registrar and stateless proxy in C++17 — small enough to run on an ESP32-S3, robust enough to handle extension-to-extension calling between real softphones on a local network, and packaged with a stunning, cross-platform **CGA CRT retro console web dashboard**.

Forked and embedded-hardened from [BarGabriel/SipServer](https://github.com/BarGabriel/SipServer).

---

## 🌟 Key Features

* **Dual-Service Engine:** Standard SIP signaling server (UDP 5060) and self-contained TCP web administration dashboard (TCP 8080 or port 80).
* **CGA CRT Retro Dashboard:** A gorgeous, authentic retro blue `#0000AA` console layout featuring:
  * Twinkling starfield background with a **3D rotating wireframe tesseract (hypercube)**.
  * Phosphor glow text-shadows, scanlines, curvature vignette, and subtle screen flicker animations.
  * Interactive terminal shell (`C:/SipServer>`) with command history (↑↓) and classic commands (`help`, `dir`, `registered`, `sessions`, `uptime`, `oracle`, `ver`, `ascii`).
  * Live system status monitoring (online extensions, call sessions, packet counters).
  * WiFi Network Manager modal for scanning and connecting networks dynamically on embedded targets.
* **System Oracle:** A rotating bar displaying witty, charming retro computing, compiler, silicon, and network routing quotes.
* **Out-of-the-Box LAN Connectivity:** Binds to `0.0.0.0` by default to listen on all interfaces. It dynamically queries the host routing table to resolve the active LAN IP and automatically substitutes it in `Via` and `Contact` SIP headers so remote devices can register plug-and-play.
* **Three Targets, One Codebase:**
  * **ESP32-S3 + W5500 Ethernet (PoE)** — Single-cable PoE calling appliance.
  * **ESP32-S3 + Wi-Fi SoftAP** — Broad-casts its own ad-hoc `esp32-sipserver` AP.
  * **Desktop (Linux/Windows)** — For instant local server hosting and testing.

---

## 🛠️ Visual & Retro Console Design

The web dashboard is fully embedded within the C++ binary inside [index_html.h](src/Helpers/index_html.h) as a raw string literal (split into chunks for maximum compiler compatibility). 

* **The Tesseract Canvas:** Uses vanilla HTML5 Canvas to execute full 3D vector rotation, projecting a 4D hypercube tesseract inside a pulsing orbital halo, matching the geeky retro-hacker aesthetic perfectly.
* **Interactive ASCII Art:** Typing `ascii` in the console prints a custom classic telephone bell matching the *pocket-dial* name.
* **CGA Palette:** Adheres strictly to the classic 16-color IBM CGA/EGA graphics color palette.

---

## 🚦 SIP Signaling & Architecture

SIP signaling coordinates VoIP call setups. Once connected, audio (RTP media) flows peer-to-peer directly between phones. This design enables a constrained MCU to host the signaling registrar without getting bogged down in heavy audio packet relaying.

```
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

### Supported SIP Methods & Responses

| Method / Response | Constant | Description |
|---|---|---|
| `REGISTER` | `SipMessageTypes::REGISTER` | Adds/updates client in memory. Responds `200 OK`. `expires=0` unregisters. |
| `INVITE` | `SipMessageTypes::INVITE` | Initiates call. Requires SDP body. Responds `404` if callee offline. |
| `ACK` | `SipMessageTypes::ACK` | Confirms final transaction handshake. |
| `BYE` | `SipMessageTypes::BYE` | Terminates active calling sessions. |
| `CANCEL` | `SipMessageTypes::CANCEL` | Cancels pending/ringing INVITE setups. |
| `SIP/2.0 100 Trying` | `SipMessageTypes::TRYING` | Sent to caller to acknowledge processing. |
| `SIP/2.0 180 Ringing` | `SipMessageTypes::RINGING` | Sent to caller while callee's device is ringing. |
| `SIP/2.0 200 OK` | `SipMessageTypes::OK` | Acknowledges transaction success (e.g. call accepted). |

---

## ⚡ Quickstart (Desktop)

Compile and run the server instantly on your local computer to test with softphones (e.g. MicroSIP, Linphone, Zoiper).

### Windows (MSVC)
From a Visual Studio Developer Command Prompt:
```bash
mkdir build && cd build
cmake ..
msbuild SipServer.sln
# Run (defaults to bind 0.0.0.0, SIP port 5060, HTTP port 8080)
.\Release\SipServer.exe
```

### Linux / macOS
```bash
mkdir build && cd build
cmake ..
make
./SipServer
```

On boot, the server outputs local binding information and active LAN routes:
```text
SIP server started on 0.0.0.0:5060 (LAN IP: 192.168.12.116)
CGA CRT Dashboard: http://localhost:8080/
Remote LAN Access:  http://192.168.12.116:8080/
Press ENTER to stop...
```

### Softphone Configuration:
1. **SSID/LAN:** Ensure the softphones are on the same local network as the server.
2. **SIP Domain/Proxy:** Specify the server's LAN IP (e.g. `192.168.12.116`).
3. **Port:** `5060` (UDP).
4. **Extension:** Any number (e.g., `101`, `102`).
5. **Password:** Leave blank (authentication is bypassed for flat trusted LAN environments).
6. **Open Dashboard:** Navigate to `http://192.168.12.116:8080/` in your browser.

---

## 🔌 Embedded ESP32-S3 Targets

The repository supports ESP-IDF and Arduino entry points.

### ESP-IDF (PoE W5500 Ethernet or Wi-Fi AP)
Requires [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/).

```bash
idf.py set-target esp32s3
# For Ethernet (PoE) Target
idf.py build flash monitor
# For Wi-Fi Access Point Target
idf.py -D SIP_TRANSPORT=wifi build flash monitor
```

### Arduino IDE
Two Arduino sketches are provided in the root directory:
* `SipServerETH.ino` — Native W5500 Ethernet variant. Requires ESP32 Arduino Core ≥ 3.0.
* `SipServer.ino` — Wi-Fi SoftAP variant. Open, select **ESP32S3 Dev Module**, and upload.

---

## 📁 Project Structure

```
pocket-dial/
├── CMakeLists.txt              # Dual-mode desktop CMake or ESP-IDF build
├── SipServer.ino               # Arduino sketch entry — Wi-Fi AP
├── SipServerETH.ino            # Arduino sketch entry — PoE W5500 Ethernet
├── main.cpp                    # Desktop entry point
├── main/
│   ├── CMakeLists.txt          # ESP-IDF component register
│   ├── esp_main.cpp            # ESP-IDF Wi-Fi AP entry
│   └── esp_main_eth.cpp        # ESP-IDF W5500 Ethernet entry
├── src/
│   ├── Helpers/
│   │   ├── cxxopts.hpp         # Desktop CLI parser
│   │   ├── IDGen.hpp           # Thread-safe ID generator
│   │   ├── IPHelper.hpp        # Routing-table LAN IP auto-resolution [NEW]
│   │   ├── HttpServer.{hpp,cpp}# TCP HTTP & JSON API server
│   │   ├── index_html.h        # 48KB CGA CRT web console string literal [NEW]
│   │   └── UdpServer.{hpp,cpp} # POSIX/Winsock/lwIP UDP abstraction
│   └── SIP/
│       ├── SipServer.{hpp,cpp} # Core controller
│       ├── RequestsHandler.{hpp,cpp} # Method dispatcher & dynamic IP resolver
│       ├── Session.{hpp,cpp}   # Call state machine
│       ├── SipClient.{hpp,cpp} # Extensions list
│       ├── SipMessage.{hpp,cpp} # SIP packet parser
│       ├── SipSdpMessage.{hpp,cpp} # SDP body parser
│       └── SipMessageFactory.{hpp,cpp} # Packet classifier
```

---

## 🔒 Security Specifications
This signaling server is lightweight and designed exclusively for **flat, trusted LANs**:
* **No Authentication:** It is designed for maximum speed and simplicity. It registers any extension without a password challenge. **Do not expose ports 5060 or 8080 to the public Internet.**
* **Peer-to-Peer Media:** Audio packets flow directly between softphones, eliminating server load.

---

## 📝 Credits & Licensing

Special thanks to **Bar Gabriel** for creating the original [BarGabriel/SipServer](https://github.com/BarGabriel/SipServer) architecture. This fork incorporates cross-platform abstractions, robust Winsock/POSIX porting, dynamic LAN routing, the CGA CRT visual dashboard, and multi-core ESP32-S3 firmware targeting.

Distributed under the **MIT License**. See [LICENSE](LICENSE) for details.
