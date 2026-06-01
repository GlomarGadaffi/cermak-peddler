# Pocket-Dial ESP32 Firmware: Contributing & Build Guide

Welcome! This guide assists contributors in setting up the development environment, compiling the source files, flashing targets, and executing local functional validation testing.

---

## 1. Development Toolchain Prerequisites

The **pocket-dial** firmware compiles under two build environments depending on your target configuration:
* **ESP-IDF Toolchain**: Preferred for production, smart-displays, and advanced multithreading builds.
* **Arduino Toolchain**: Used for rapid prototyping and flashing individual `.ino` sketch targets.

### A. ESP-IDF Environment Setup (Recommended)
1. Install **ESP-IDF v5.1** or newer (v5.1.x / v5.2.x are fully supported). Follow the [Espressif Installation Guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html).
2. Configure environmental paths (Windows PowerShell example):
   ```powershell
   . $HOME\esp\esp-idf\export.ps1
   ```
3. Verify your compiler installation:
   ```bash
   xtensa-esp32s3-elf-gcc --version
   ```

### B. Arduino IDE Toolchain
1. Download **Arduino IDE v2.x** or compile via `arduino-cli`.
2. Add the Espressif Board Manager URL under Preferences:
   ```text
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
3. Install **ESP32 by Espressif Systems (v3.0.0 or higher)**.
   > [!IMPORTANT]
   > ESP32 Arduino Core 3.x is required. It contains the native Wiznet W5500 SPI Ethernet and LAN8720 RMII drivers used in the sketches.

---

## 2. Compilation & Flashing Instructions

### A. Compiling via ESP-IDF CLI
1. Open your terminal in the root workspace directory.
2. Select your hardware chip target (`esp32s3` for displays and S3-ETH, `esp32` for legacy POE-Pro):
   ```bash
   idf.py set-target esp32s3
   ```
3. Open menuconfig to verify partition schemes, serial speeds, and octal PSRAM settings:
   ```bash
   idf.py menuconfig
   ```
4. Build the application:
   ```bash
   idf.py build
   ```
5. Flash the binary and launch the serial logger (replace `COM3` with your local port):
   ```bash
   idf.py -p COM3 flash monitor
   ```

### B. Compiling via Arduino IDE
1. Open the `.ino` sketch from the `sketches/` directory corresponding to your board (e.g., `sketches/SipServer_T_ETH_Lite_W5500/SipServer_T_ETH_Lite_W5500.ino`).
2. Select your board under **Tools > Board**:
   * For S3-ETH and T-ETH-Lite: Select **ESP32S3 Dev Module**.
   * For POE-Pro: Select **ESP32 Wrover Module**.
3. Enable **PSRAM** under **Tools > PSRAM > OPI PSRAM** (if applicable).
4. Select the **Huge APP (3MB No OTA/1MB SPIFFS)** or custom partition scheme.
5. Click **Upload** to compile and flash.

---

## 3. Local Testing & Validation Methodology

Once the target is flashed and running, follow these steps to test the local installation.

### A. Connecting Softphones
To test VoIP signaling, connect two software SIP clients (such as **MicroSIP**, **Linphone**, or **Zoiper**) to the target's IP address:

1. **Verify Server IP**: Read the serial output or on-screen display.
   * SoftAP Mode: Default gateway is `192.168.4.1`.
   * Station/DHCP Mode: Locate the leased IP (e.g. `192.168.1.145`).
2. **Configure Softphone Accounts**:
   * **Domain / Registrar**: `<device_ip>:5060` (e.g. `192.168.4.1:5060`)
   * **Username / Extension**: Set user 1 to `1001` and user 2 to `1002`.
   * **Password**: Leave blank (no authentication is enforced in the registrar).
   * **Protocol**: Set transport to **UDP**.
3. **Initiate Call**: Dial `1002` from `1001`. The status display should update instantly to show:
   * State: `Invited` (Ringing)
   * State: `Connected` (Active)
4. **Test Special Features**:
   * **Echo Test (`777`)**: Dial `777` from any extension. Speak into your mic; the server will mirror your RTP audio stream back to you.
   * **Intercom / Broadcast Paging (`999`)**: Dial `999` from an extension. The server forks the call to all other registered extensions, auto-answers their speakerphones, and streams audio to them simultaneously.

### B. Dashboard REST API Verification
Verify the Web dashboard and security barriers using `curl` or a web browser:

1. **Fetch Registrar Status**:
   ```bash
   curl -i http://192.168.4.1/api/status
   ```
   Verify that `packetsProcessed` increments with each registration or call.

2. **Trigger Same-Origin Rejection**:
   Attempt a simulated cross-origin POST attack to disconnect an extension:
   ```bash
   curl -i -X POST -H "Origin: http://malicious-site.com" -H "Host: 192.168.4.1" --data "extension=1001" http://192.168.4.1/api/kill
   ```
   Confirm that the server returns `403 Forbidden` with the cross-origin error payload.

3. **Verify Payload Capacity Protections**:
   Send an oversized request body (over 16KB) to simulate a buffer-flooding attack:
   ```bash
   curl -i -X POST -H "Content-Length: 20000" --data-binary @/path/to/large_file.txt http://192.168.4.1/api/wifi/connect
   ```
   Confirm that the connection is immediately severed or returns a `413 Payload Too Large` error.

---

## 4. Continuous Integration (CI) Checks

The **pocket-dial** repository enforces strict verification checks on every pull request (configured via `.github/workflows/ci.yml`):
* **Syntax Validation**: Ensures all source files are free of compilation warnings.
* **API Schema Audits**: Spawns a virtual container running the build and validates JSON responses against the API schemas defined in [docs/API.md](API.md).
* **Cross-Compilation Verification**: Verifies code builds for both `esp32` and `esp32s3` targets.
* **Firmware Policy Checks**: Scans for prohibited code patterns (like heap allocation or raw `strcpy` operations).
