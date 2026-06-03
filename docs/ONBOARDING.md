# Pocket-Dial Firmware Onboarding Guide
### ⏱ Zero-to-Flashing in Under 30 Minutes

Welcome to the pocket-dial ESP32 firmware project! This guide will take you from a clean machine to a fully compiled and flashed firmware, with a functioning HTTP dashboard running on your target development board.

---

## 📋 Prerequisites & Tools

Before you begin, install the required toolchain and platform dependencies based on your operating system:

### 1. Hardware Drivers
Ensure you have installed the correct USB-to-UART bridge drivers for your target board:
* **Silicon Labs CP210x:** [Download CP210x VCP Drivers](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers) (Common on standard ESP32 DevKits)
* **WCH CH340 / CH341:** [Download CH340 Drivers](http://www.wch-ic.com/downloads/CH341SER_EXE.html) (Common on low-cost S3 boards, displays, and clone devkits)

### 2. ESP-IDF Toolchain (v5.1.2 or v5.2.1)
The firmware is fully certified and compiled against **ESP-IDF v5.1.2** and **v5.2.1**.
* **Windows installation:** Download and run the [ESP-IDF Offline Installer](https://dl.espressif.com/dl/esp-idf/) (Select version `5.1.2` or `5.2.1`).
* **Linux/macOS installation:** Follow the [Espressif Standard Shell Setup Guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/linux-macos-setup.html).

---

## 🚀 Step 1: Environment Activation

To load the correct compilers (`xtensa-esp32-elf-gcc`, `xtensa-esp32s3-elf-g++`), build utilities (`cmake`, `ninja`), and target tools (`esptool.py`, `idf.py`) into your current shell session:

### 💻 Windows (PowerShell)
```powershell
# Open PowerShell and run the export script generated during installer setup
. ~\esp\esp-idf\export.ps1
```

### 🐧 Linux / 🍏 macOS
```bash
# Source the export script in your active terminal
. $HOME/esp/esp-idf/export.sh
```

> [!TIP]
> If activation is successful, running `idf.py --version` should output `ESP-IDF v5.1.x` or `ESP-IDF v5.2.x`.

---

## 🛠️ Step 2: Clone & Configure

1. **Clone the repository and submodules:**
   ```bash
   git clone --recursive https://github.com/GlomarGadaffi/pocket-dial.git
   cd pocket-dial
   ```

2. **Select your target chip architecture:**
   The firmware supports multiple physical boards. Tell the build system which chip you are compiling for:
   ```bash
   # For Standard ESP32 devboards (e.g. NodeMCU, Waveshare PoE)
   idf.py set-target esp32

   # For ESP32-S3 boards (e.g. Guition AXS15231B Display, LilyGO T-ETH-Lite)
   idf.py set-target esp32s3
   ```

---

## ⚡ Step 3: Select Your Hardware Transport Layer

The firmware is designed with a highly modular physical layer. Define your transport configuration at compile-time by passing the `-D SIP_TRANSPORT` parameter to `idf.py`:

```
                           ┌───────────────────────────┐
                           │      Build Entrypoint     │
                           └─────────────┬─────────────┘
                                         │
                 ┌───────────────────────┼───────────────────────┐
                 ▼                       ▼                       ▼
      -D SIP_TRANSPORT=wifi    -D SIP_TRANSPORT=eth    -D SIP_TRANSPORT=display
                 │                       │                       │
                 ▼                       ▼                       ▼
    ┌─────────────────────────┐  ┌─────────────────────────┐  ┌─────────────────────────┐
    │     Wi-Fi SoftAP Mode   │  │   W5500 PoE Ethernet    │  │   AXS15231B Touch screen │
    │     (esp_main.cpp)      │  │   (esp_main_eth.cpp)    │  │ (esp_main_display.cpp)  │
    └─────────────────────────┘  └─────────────────────────┘  └─────────────────────────┘
```

| Transport Parameter | Target Board Description | Primary Entrypoint File |
| :--- | :--- | :--- |
| `wifi` | Broad support. Starts a Standalone Access Point named `esp32-sipserver` | `main/esp_main.cpp` |
| `eth` *(Default)* | W5500 / LAN8720 Ethernet chips (e.g. Waveshare PoE, T-POE-Pro) | `main/esp_main_eth.cpp` |
| `display` | Guition AXS15231B 2.4" LCD, pins graphics to Core 1 and SIP on Core 0 | `main/esp_main_display.cpp` |

Choose **ONE** of the following compilation commands matching your physical hardware setup:

```bash
# Option A: Build for Standalone Wi-Fi (Any standard ESP32 or S3 Board)
idf.py -D SIP_TRANSPORT=wifi build

# Option B: Build for Ethernet (PoE / Wired W5500 setup)
idf.py -D SIP_TRANSPORT=eth build

# Option C: Build for Guition Touch Display (Requires AXS15231B with LVGL)
idf.py -D SIP_TRANSPORT=display build
```

---

## 🔒 Step 4: Security Customizations (Issues #54-#59)

Before compiling, configure security settings according to your environment's posture:

### 1. Closed Mode vs. Open Mode Authentication (Issue #56)
By default, the SIP engine starts in **Open Mode** to facilitate rapid bench-testing and hobbyist setups. If you are deploying the firmware in a production or shared corporate network environment, you must switch the system to **Closed Mode** to block unauthorized traffic.
* **To Enable Closed Mode:** Open `src/SIP/RequestsHandler.hpp` and comment out or remove the following line:
  ```cpp
  // Comment this line to switch from default-open to a secure closed registrar:
  // #define POCKETDIAL_OPEN_REGISTRAR
  ```
* **Effect:** When undefined, any incoming `REGISTER` or `INVITE` transaction from an unrecognized endpoint or with non-matching credentials will be blocked immediately with a secure `403 Forbidden` response code.

### 2. Address of Record (AOR) Sanitization Safeguard (Issue #55)
The engine whitelists and sanitizes SIP Address of Record (AOR) segments during inbound packet parsing. 
* Only alphanumeric characters and the characters `.`, `-`, `_`, `+` are allowed.
* Malformed injection strings or script probes are instantly intercepted and rejected with a `400 Bad Request` packet, preventing potential parsing or configuration hijacking attempts.

### 3. Distributed Scanner Denial-of-Service Defense (Issue #58)
The system prevents flash/memory exhaustion exploits by network scanners via two mechanisms:
* Old rate-limit tracking buckets (inactive for >60 seconds) are automatically swept from memory during the tick process.
* The number of concurrent tracking source IPs is restricted to a hard cap of `MAX_BUCKETS = 256`. Scanning packets exceeding this limit are silently dropped to safeguard device stability.

---

## 🔌 Step 5: Flash the Board

Connect your development board using a high-quality USB-C/micro-USB cable. Identify the serial port assigned to your device:
* **Windows:** Open Device Manager and check `Ports (COM & LPT)` (e.g., `COM3`, `COM12`).
* **Linux:** Run `ls /dev/ttyUSB*` or `ls /dev/ttyACM*` (e.g., `/dev/ttyUSB0`).
* **macOS:** Run `ls /dev/cu.usbserial*` or `ls /dev/cu.usbmodem*`.

Flash the compiled firmware binaries and partition tables directly to the flash memory of your board, and start the serial monitor:

```bash
# Replace 'COM3' or '/dev/ttyUSB0' with your actual port address
idf.py -p COM3 flash monitor
```

> [!IMPORTANT]
> If the flashing utility times out while trying to connect, hold down the physical **BOOT / FLASH** button on the development board, press and release the **EN / RST** button, then release the BOOT button to force the chip into ROM bootloader mode before running the command.

---

## 🧪 Step 6: Manual HTTP API Verification

Once flashing completes, the device boots and the serial monitor will show logging status. If running in **Wi-Fi SoftAP** or **Display** mode:

1. **Connect to the Network:** On your laptop or phone, connect to the open Wi-Fi network named **`esp32-sipserver`** (no password required).
2. **Retrieve Server IP:** The SoftAP gateway IP defaults to **`192.168.4.1`**.

You can now manually query and control the firmware using standard terminal commands or by navigating your browser to `http://192.168.4.1/`.

### 1. Get Live System Status (Uptime, Registered Clients, and Sessions)
Retrieves system metrics and registered SIP extensions.

#### Command
```bash
curl -s http://192.168.4.1/api/status
```

#### Expected JSON Output
```json
{
  "ip": "192.168.4.1",
  "port": 5060,
  "httpPort": 80,
  "uptime": 45,
  "packetsProcessed": 12,
  "packetsDropped": 0,
  "clients": [
    {
      "number": "100",
      "address": "192.168.4.2:5060"
    }
  ],
  "sessions": []
}
```

---

### 2. Scan Local Wi-Fi Networks
Triggers an active network scan and returns reachable SSID lists, their signal strength, and encryption types.

#### Command
```bash
curl -s http://192.168.4.1/api/wifi/scan
```

#### Expected JSON Output
```json
{
  "networks": [
    {
      "ssid": "HQ-Office-WiFi",
      "rssi": -65,
      "encryption": "WPA2"
    },
    {
      "ssid": "Guest-Zone",
      "rssi": -82,
      "encryption": "WPA2"
    }
  ]
}
```

---

### 3. Connect Device to a Client Wi-Fi Network
Saves credentials into the `"storage"` NVS namespace, changes operating mode to station, and initiates an automatic system restart.

#### Command
```bash
# For Windows PowerShell:
Invoke-RestMethod -Method Post -Uri "http://192.168.4.1/api/wifi/connect" -Body "ssid=HQ-Office-WiFi&password=MySecurePassword"

# For Linux/macOS Shell:
curl -X POST \
     -H "Content-Type: application/x-www-form-urlencoded" \
     -d "ssid=HQ-Office-WiFi&password=MySecurePassword" \
     http://192.168.4.1/api/wifi/connect
```

#### Expected Response
```json
{
  "status": "ok",
  "message": "WiFi credentials saved. Rebooting to Station Mode..."
}
```

---

### 4. Force Disconnect an Active Call Extension (CSRF-Protected)
Sends a disconnect command to terminate the session of a rogue or deadlocked extension. Because this is a state-changing POST endpoint, it requires **Same-Origin protection**. You must supply matching `Origin` and `Host` headers.

#### Command
```bash
curl -X POST \
     -H "Origin: http://192.168.4.1" \
     -H "Host: 192.168.4.1" \
     -H "Content-Type: application/x-www-form-urlencoded" \
     -d "extension=100" \
     http://192.168.4.1/api/kill
```

#### Expected Response
```json
{
  "status": "ok",
  "disconnected": "100"
}
```

---

## ⌨️ IDF Monitor Shortcuts

When viewing the console logs inside `idf.py monitor`, use these keyboard shortcuts to manage your session:

* **`Ctrl + ]`**: Exit the serial monitor and return to your terminal shell.
* **`Ctrl + T` then `Ctrl + R`**: Toggle the RST pin of the board to trigger a hardware reboot.
* **`Ctrl + T` then `Ctrl + L`**: Toggle hex logging output (displays raw serial packets).
* **`Ctrl + T` then `Ctrl + H`**: Display help menu showing all console monitor controls.
