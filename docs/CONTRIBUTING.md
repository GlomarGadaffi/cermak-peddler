# Pocket-Dial ESP32 Firmware: Contributing & Build Guide

Welcome! This guide assists contributors in setting up the development environment, compiling the source files, flashing targets, and executing local functional validation testing.

---

## 1. Development Toolchain Prerequisites

The **pocket-dial** firmware builds with **ESP-IDF**. That is the only supported
firmware toolchain: it is what CI builds on every pull request, what the release
workflow ships, and what the browser flasher serves.

> [!NOTE]
> An Arduino/`.ino` sketch path used to live in `sketches/`. It was removed: it had
> been broken since June 2026, nothing in CI compiled it, and nobody noticed. See
> issues #41 and #143 for the history.

### ESP-IDF Environment Setup
1. Install **ESP-IDF v6.0 or newer**. `main/CMakeLists.txt` hard-fails at configure
   time below v6.0 (components such as `esp_driver_ledc` were split out of the
   monolithic `driver` component after v5.2, so older toolchains cannot build this
   project at all). CI pins **v6.0.1**. Follow the [Espressif Installation Guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html).
2. Configure environmental paths (Windows PowerShell example):
   ```powershell
   . $HOME\esp\esp-idf\export.ps1
   ```
3. Verify your compiler installation:
   ```bash
   xtensa-esp32s3-elf-gcc --version
   ```

---

## 2. Compilation & Flashing Instructions

### Compiling & flashing via the ESP-IDF CLI
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
   * **Password**: Leave blank — the registrar's **default** mode is open, so any
     extension registers without a credential. (SIP digest authentication exists and is
     runtime-selectable via the `open` / `learn` / `secure` registrar modes; in `secure`
     mode you must supply the extension's provisioned password here. See
     [LEARN_MODE.md](LEARN_MODE.md) and [THREAT_MODEL.md](THREAT_MODEL.md) §9.)
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
