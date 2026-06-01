# ESP32 Pocket-Dial Firmware: Field Incident Playbook

This document serves as the authoritative production-grade field operation and incident playbook for the pocket-dial ESP32 / ESP32-S3 firmware. It is intended for field engineers, system administrators, and core firmware maintainers to diagnose, isolate, and recover devices suffering from field anomalies.

---

## 🛠 Quick Reference Matrix

Use this matrix for rapid triage based on visible device indicators and active diagnostic symptoms:

| Scenario / Symptom | Primary Indicators | Probable Root Cause | Instant Recovery Action |
| :--- | :--- | :--- | :--- |
| **Watchdog Reset** | • Boot loops with `TG0WDT_SYS_RST`<br>• Logs showing `Task watchdog got triggered` | • Thread starvation on Core 1<br>• Infinite loop in SIP message parser | • Increase TWDT timeout in sdkconfig<br>• Add `vTaskDelay` yields in parsing loops |
| **NVS / Credential Corruption** | • Core boot loops on `nvs_flash_init()` failure<br>• Constant boot loop back to factory SoftAP | • Flash sector wear-out<br>• Brownout mid-write (incomplete `nvs_commit`) | • Programmatic partition format on error<br>• Force sector erase with `esptool.py` |
| **SIP Engine Deadlock** | • SIP endpoints unregisterable (5060 dead)<br>• HTTP Dashboard running on Core 0 (8080/80 active) | • Recursive locking of `RequestsHandler::_mutex`<br>• Lock-order inversion in paging | • Trigger CPU crash dump / JTAG stack trace<br>• Hard power cycle or remote API restart |
| **OTA Failure / Rollback** | • Device boots old firmware after OTA update<br>• Bootloader prints `Rollback triggered...` | • Missing `esp_ota_mark_app_valid_cancel_rollback`<br>• Network dropout mid-stream | • Verify OTA validation timing<br>• Flash known working binary to active slot |

---

## 1. Watchdog Resets (Task & Hardware WDT)

ESP32 chips feature both a **Hardware Watchdog Timer (WDT)** in the Timer Group and a **Task Watchdog Timer (TWDT)** managed by FreeRTOS. A watchdog reset indicates that the CPU has been occupied continuously by a high-priority task without yielding control to the system idle tasks or lower-priority routines.

```
       ┌─────────────────────────────────────────────────────────┐
       │     SipServer Loop / UDP Receive Task (Priority 5)      │
       └────────────────────────────┬────────────────────────────┘
                                    │  Parses incoming SIP packet
                                    ▼
       ┌─────────────────────────────────────────────────────────┐
       │     Infinite Parsing / Processing Loop (No yields)      │
       └────────────────────────────┬────────────────────────────┘
                                    │  Fails to call vTaskDelay()
                                    ▼
       ┌─────────────────────────────────────────────────────────┐
       │      Idle Task Starved (CPU Core 0/1 pinned @ 100%)     │
       └────────────────────────────┬────────────────────────────┘
                                    │  WDT counter expires
                                    ▼
       ┌─────────────────────────────────────────────────────────┐
       │        Task Watchdog Triggered (Panic / Reboot)        │
       └─────────────────────────────────────────────────────────┘
```

### 🔍 Detection & Symptoms
1. **Serial Console Logs:** Look for the signature panic message:
   ```text
   E (12345) task_wdt: Task watchdog got triggered. The following tasks did not reset the watchdog in time:
   E (12345) task_wdt:  - IDLE1 (CPU 1)
   E (12345) task_wdt: Tasks currently running:
   E (12345) task_wdt: CPU 0: http_server_task
   E (12345) task_wdt: CPU 1: sip_server_task
   ```
2. **Reset Reason Check:** Upon reboot, the bootloader logs the reset reason. If `esp_reset_reason_t` is called, it returns `ESP_RST_WDT`.
3. **Register Stack Dumps:**
   ```text
   Guru Meditation Error: Core  1 panic'ed (Interrupt wdt timeout on CPU1).
   Core  1 register dump:
   PC      : 0x400d54c8  PS      : 0x00060034  A0      : 0x400d5a1c  A1      : 0x3ffd5480
   ```

### 🔬 Diagnosis Procedure
To map raw hex addresses (like `PC : 0x400d54c8`) back to the specific line of C++ code causing the deadlock or lockup, use the ESP-IDF toolchain's backtrace decoder:

```powershell
# For Standard ESP32 (Xtensa)
xtensa-esp32-elf-addr2line -pfia -e build/SipServer.elf 0x400d54c8 0x400d5a1c

# For ESP32-S3 (Xtensa-S3)
xtensa-esp32s3-elf-addr2line -pfia -e build/SipServer.elf 0x400d54c8 0x400d5a1c
```

> [!NOTE]
> Ensure that the `.elf` binary used for decoding matches the exact compiler build running on the target device; otherwise, line offsets will be misaligned.

### 🛡️ Recovery & Prevention
* **Insert Cooperative Yields:** Ensure that every high-priority loop, especially inside `UdpServer::receiveLoop` or the SIP engine's `RequestsHandler::tick()`, yields control.
  ```cpp
  // Force a task block to allow IDLE task execution and watchdog feeding
  vTaskDelay(pdMS_TO_TICKS(1)); 
  ```
* **Adjust WDT Parameters:** If complex multi-party paging or intense network scanning requires more overhead, increase the task watchdog timer duration inside `sdkconfig` via:
  ```text
  CONFIG_ESP_TASK_WDT_TIMEOUT_S=15
  ```

---

## 2. NVS & Credential Corruption

The device uses Non-Volatile Storage (NVS) inside the `"storage"` namespace to save Wi-Fi SSID, passphrases, and modes. Incomplete flash operations during sudden power interruptions (brownouts) or flashing sector wear-out can lead to a corrupted partition.

```
                    ┌────────────────────────────┐
                    │      Device Power-On       │
                    └─────────────┬──────────────┘
                                  │
                                  ▼
                    ┌────────────────────────────┐
                    │     nvs_flash_init()       │
                    └─────────────┬──────────────┘
                                  │
               ┌──────────────────┴──────────────────┐
        Success│                                     │Error (Pages Corrupted)
               ▼                                     ▼
┌────────────────────────────┐         ┌────────────────────────────┐
│   Load saved Wi-Fi Mode    │         │     nvs_flash_erase()      │
│   and register SIP client  │         └─────────────┬──────────────┘
└────────────────────────────┘                       │ Re-init
                                                     ▼
                                       ┌────────────────────────────┐
                                       │     nvs_flash_init()       │
                                       └─────────────┬──────────────┘
                                                     │
                                                     ▼
                                       ┌────────────────────────────┐
                                       │    Load Default Standalone │
                                       │    SoftAP Mode Configuration│
                                       └────────────────────────────┘
```

### 🔍 Detection & Symptoms
1. **Crash Loops:** The firmware crashes and restarts indefinitely at boot-time with error logs from `app_main`:
   ```text
   E (450) app_main: NVS Initialization Failed: ESP_ERR_NVS_NO_FREE_PAGES (0x110d)
   ```
2. **Loss of Connection Parameters:** The device boots into the default Standalone AP (`esp32-sipserver`), failing to connect to the previously configured local Station Wi-Fi network, despite no user configuration changes.

### 🔬 Diagnosis Procedure
* Monitor serial output during device initialization.
* Look for errors associated with NVS key retrieval, specifically `nvs_open` or `nvs_get_str` returning `ESP_ERR_NVS_NOT_FOUND` (0x1102).

### 🛡️ Recovery & Prevention
To resolve a hard NVS corruption or partition block lockup, execute an explicit NVS partition erase using `esptool.py`.

#### Step 1: Locate the NVS Partition Address
Check your partition table (usually at `0x8000`). By default, the NVS partition is allocated at offset `0x9000` with a size of `0x6000` (24KB).

#### Step 2: Manually Erase the NVS Sector
```bash
# Clean NVS sector ONLY (retains core application and partition table)
esptool.py -p COM3 -b 460800 erase_region 0x9000 0x6000
```

> [!WARNING]
> If `erase_region` does not resolve the loop, the entire flash chip must be cleared to eliminate persistent bad partition tables. Use the following command with caution, as it will wipe all active firmware partitions:
> ```bash
> esptool.py -p COM3 erase_flash
> ```

#### Programmatic Fallback (C++ Safeguard)
Verify that `app_main` implements the standard Espressif auto-erase safeguard (present in `esp_main.cpp`):
```cpp
esp_err_t ret = nvs_flash_init();
if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
}
ESP_ERROR_CHECK(ret);
```

---

## 3. SIP Engine Deadlocks

The post-refactor `RequestsHandler` manages a concurrent snapshot-based dashboard structure. An architectural deadlock occurs if recursive operations or lock-order inversion locks `RequestsHandler::_mutex` permanently, starving SIP execution.

### 🔍 Detection & Symptoms
1. **Partial Responsiveness:** The HTTP dashboard is fully active on Core 0 (`http://192.168.4.1:80` returns the CGA CRT interface instantly), but the metrics showing active sessions, registered extensions, and processed packet counts remain frozen.
2. **SIP Protocol Silent Drop:** Active SIP phone endpoints show `No Registration` or timeout with status `408 Request Timeout` on port `5060` (UDP).
3. **Task Monitor Frozen:** Serial console logs do not show `UdpServer` or `RequestsHandler::tick` processing notices.

### 🔬 Diagnosis Procedure
If JTAG or an ESP-Prog adapter is connected, attach GDB to the target chip to view running backtraces:

```text
(gdb) thread apply all bt
...
Thread 2 (sip_server_task):
#0  vPortPlaceOnEventList (pxEventList=0x3ffd5a20, xTicksToWait=4294967295) at tasks.c:3120
#1  xQueueSemaphoreTake (xQueue=0x3ffd5a10, xTicksToWait=4294967295) at queue.c:1530
#2  std::mutex::lock (this=0x3ffd31b4) at mutex.cpp:45
#3  RequestsHandler::handle (this=0x3ffd3110, request=...) at RequestsHandler.cpp:115
```

> [!IMPORTANT]
> The `std::mutex` provided by the ESP-IDF toolchain is a **non-recursive** mutex (wraps a standard FreeRTOS binary semaphore). If a function holding the lock attempts to call another member function that also requests the lock (e.g., calling `sweepExpired()` within another locked method without passing lock ownership), the task will immediately **self-deadlock**.

### 🛡️ Recovery & Prevention
1. **Emergency Remote Restart:** Since the HTTP task on Core 0 is unaffected, use the credentials api or fallback mode reboot api to trigger a remote software restart:
   ```bash
   curl -X POST http://192.168.4.1/api/wifi/mode_ap
   ```
2. **Locking Safeguard Best Practices:**
   * Never invoke external/callback functions while holding `_mutex`!
   * Copy necessary registrar state into local stack frames using `RegistrarSnapshot` copy structures before running extensive parsing or dispatch operations.
   * If a method needs to be called both internally (with lock held) and externally, split it into a public locked wrapper and a private unlocked implementation (typically suffixed with `_Unformatted` or `_NoLock`).

---

## 4. Over-The-Air (OTA) Failures & Rollbacks

OTA updates use dual application partitions (`ota_0` and `ota_1`) along with a dedicated `otadata` control partition. This configuration ensures that if a newly flashed firmware fails, the system automatically rolls back to the previous stable partition.

```
       ┌─────────────────────────────────────────────────────────┐
       │             Initialize OTA Update Sequence              │
       └────────────────────────────┬────────────────────────────┘
                                    │  Stream binary via Wi-Fi/ETH
                                    ▼
       ┌─────────────────────────────────────────────────────────┐
       │        Write Binary to Secondary Partition (ota_1)       │
       └────────────────────────────┬────────────────────────────┘
                                    │  Finish stream and verify hash
                                    ▼
       ┌─────────────────────────────────────────────────────────┐
       │     Set Boot Partition to ota_1 & Trigger reboot        │
       └────────────────────────────┬────────────────────────────┘
                                    │  esp_restart()
                                    ▼
       ┌─────────────────────────────────────────────────────────┐
       │              Bootloader Starts ota_1 app                │
       └────────────────────────────┬────────────────────────────┘
                                    │
                  ┌─────────────────┴─────────────────┐
        Success within 30s?                 Failure / Watchdog Crash?
                  ▼                                   ▼
┌───────────────────────────────────┐       ┌───────────────────────────────────┐
│  Call ota_mark_app_valid()        │       │ Bootloader detects crash / panic   │
│  State: ESP_OTA_IMG_VALID         │       │ State: ESP_OTA_IMG_INVALID        │
│  Firmware update committed!       │       │ Rollback boot partition to ota_0  │
└───────────────────────────────────┘       └───────────────────────────────────┘
```

### 🔍 Detection & Symptoms
1. **Automatic Rollovers:** After performing an OTA firmware update, the device boots but continues to display the old version string on the CGA HTTP dashboard.
2. **Boot Logs Rollback Signature:** The serial console displays the following bootloader notifications:
   ```text
   I (520) esp_image: Verifying image signature...
   I (545) esp_ota_ops: Partition ota_1 has pending rollback state.
   W (550) esp_ota_ops: Diagnostics failed or app crashed before validating. Rolling back...
   I (562) esp_ota_ops: Setting active partition to ota_0. Rebooting...
   ```

### 🔬 Diagnosis Procedure
* Connect to the serial port during boot.
* Run `esp_ota_get_state_partition()` to check the active state.
  * `ESP_OTA_IMG_NEW` (Flashed, unverified)
  * `ESP_OTA_IMG_PENDING_VERIFY` (Booted, awaiting validation)
  * `ESP_OTA_IMG_VALID` (Committed stable)
  * `ESP_OTA_IMG_INVALID` (Rollback candidate)

### 🛡️ Recovery & Prevention
To bypass rollback logic and manually recover a device stuck in a corrupted OTA state, use direct flash operations via `esptool.py` to restore partition state:

```powershell
# Step 1: Clear the OTA selection table (forces bootloader to return to factory partition)
esptool.py -p COM3 -b 460800 erase_region 0xd000 0x2000

# Step 2: Manually flash a verified stable SipServer binary directly to partition ota_0
esptool.py -p COM3 -b 460800 write_flash 0x10000 build/SipServer.bin
```

> [!IMPORTANT]
> The offset address of `ota_0` (here `0x10000`) must match the value configured in your custom partition layout `.csv` file. 

#### Verification API Call
Ensure your firmware contains the following safety pattern immediately after ensuring successful Wi-Fi connection and SIP initialization:
```cpp
#include "esp_ota_ops.h"

void verify_firmware_on_boot() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            // Check essential service states (SIP ports bound, Wi-Fi OK)
            if (g_sipServer != nullptr) {
                ESP_LOGI("OTA", "Self-diagnostics passed. Marking app as valid!");
                esp_ota_mark_app_valid_cancel_rollback();
            } else {
                ESP_LOGE("OTA", "Diagnostics failed. System will roll back on reboot.");
                esp_ota_mark_app_invalid_rollback_and_reboot();
            }
        }
    }
}
```
