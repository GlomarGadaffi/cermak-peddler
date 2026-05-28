# pocket-dial

> [!TIP]
> **TL;DR (Extra Super Lazy Quickstart)**
> * **Windows**: Just double-click `quickstart.bat`
> * **Linux / macOS**: Run `chmod +x quickstart.sh && ./quickstart.sh`
> * **ESP32 / CYD (Arduino)**: Open `SipServer.ino` in Arduino IDE, hit **Upload**, connect to the `esp32-sipserver` AP, and open `http://192.168.4.1/`!
> 
> That's it. This will automatically configure, compile, run the server, and open/show the retro CRT dashboard.

A C++17 SIP registrar and stateless proxy designed for flat, trusted local networks. It runs on desktop environments (Windows/Linux) and embedded targets (ESP32-S3 via Wi-Fi or W5500 PoE Ethernet) and includes a CGA CRT retro-styled web administration console.

Forked and extended from [BarGabriel/SipServer](https://github.com/BarGabriel/SipServer).

---

## Features

### 1. SIP Signaling & Call Setup
* **User Registration**: Supports the `REGISTER` transaction. Saves registered endpoints dynamically in memory. Unregistration is handled via `expires=0`. Bypasses password validation for simplicity in flat trusted LAN environments.
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

---

## Quickstart (Desktop)

The repository provides zero-dependency automated quickstart scripts that configure CMake, compile the executable, launch the SIP server, and automatically open the retro console dashboard in your default browser.

### Windows
Double-click `quickstart.bat` or run it from the command line:
```cmd
quickstart.bat
```

### Linux / macOS
Run the bash script:
```bash
./quickstart.sh
```

---

## Manual Build (Desktop Fallback)

If you prefer to compile manually, follow the standard CMake workflow:

### Windows (MSVC)
Open a Visual Studio Developer Command Prompt:
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
Two pre-configured sketches are available in the root directory:
* `SipServerETH.ino` — Wired W5500 Ethernet variant (Requires ESP32 Arduino Core 3.x).
* `SipServer.ino` — Wireless SoftAP variant.

To compile in the Arduino IDE, set the board to **ESP32S3 Dev Module** and import the source files located under `src/Helpers` and `src/SIP` to the sketch.

---

## Project Structure

```text
pocket-dial/
├── CMakeLists.txt              # Multi-platform desktop CMake or ESP-IDF delegate
├── SipServer.ino               # Arduino IDE entry — Wi-Fi SoftAP
├── SipServerETH.ino            # Arduino IDE entry — W5500 Ethernet
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
│       ├── SipMessageHeaders.h # SIP standard header field name constants
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

The server is designed for flat, private networks:
* **No Authentication**: The registrar does not validate user credentials. **Do not expose SIP (UDP 5060) or administration (TCP 8080/80) ports directly to the public Internet.**
* **Decentralized Media**: Voice packets flow peer-to-peer, keeping memory footprint low.

---

## License

Distributed under the **MIT License**. See [LICENSE](LICENSE) for details.
