# Flashing pocket-dial Firmware

This guide covers building and flashing pocket-dial firmware onto an ESP32-S3
board, and updating it afterward over-the-air (OTA).

> **First flash must be over USB.** The dual-OTA partition layout (see
> [OTA.md](OTA.md)) differs from any single-`factory` image, so the very first
> install of an OTA-capable build has to go on over the USB/serial port. After
> that, you can update wirelessly from the dashboard.

---

## 1. Which firmware for which board

All targets build for **ESP32-S3** with **ESP-IDF v5.2.1**. Pick the transport
with `-D SIP_TRANSPORT=<transport>`:

| Board | `SIP_TRANSPORT` | Verified app size |
|-------|-----------------|-------------------|
| Guition JC3248W535 (3.5" touch display) | `display` | ~1.50 MB (1,571,568 B) |
| Generic ESP32-S3 / SoftAP | `wifi` | ~1.29 MB (1,288,464 B) |
| Waveshare ESP32-S3-ETH / LilyGO T-ETH (W5500) | `eth` | ~0.95 MB (949,984 B) |

Every image fits comfortably in the 6 MB `ota_0` / `ota_1` slots.

---

## 2. Build

```bash
. $IDF_PATH/export.sh            # set up the ESP-IDF v5.2.1 environment
idf.py set-target esp32s3
idf.py -D SIP_TRANSPORT=display build     # or wifi / eth
```

The build produces, under `build/`:

- `bootloader/bootloader.bin`
- `partition_table/partition-table.bin`
- `SipServer.bin` (the application)
- `flash_args` (the exact offsets, used by `idf.py flash`)

---

## 3. First-time flash (USB)

### Option 0 — from the browser, no toolchain

Open **<https://glomargadaffi.github.io/pocket-dial/flasher/>** in Chrome, Edge, or
Opera on a desktop, plug the board in over USB, pick the variant (Ethernet /
display / Wi-Fi) and click **Flash board**. It pulls the images from the GitHub
Release you choose and writes them from your machine; nothing is uploaded. It
also takes locally built `.bin` files under *Flash your own build*. Details in
[flasher/README.md](flasher/README.md).

Connect the board over USB and identify the serial port:

- **Linux:** `/dev/ttyUSB0` or `/dev/ttyACM0`
- **macOS:** `/dev/cu.usbserial-*` or `/dev/cu.usbmodem-*`
- **Windows:** `COM3` (check Device Manager → Ports)

### Option A — `idf.py` (recommended)

```bash
idf.py -p COM3 flash         # Windows
idf.py -p /dev/ttyUSB0 flash # Linux
```

This writes the bootloader, partition table, and app at the correct offsets and
can `monitor` the serial log with `idf.py -p COM3 monitor` (or `flash monitor`).

### Option B — `esptool` directly

```bash
esptool.py -p COM3 -b 460800 --chip esp32s3 write_flash \
  0x0      build/bootloader/bootloader.bin \
  0x8000   build/partition_table/partition-table.bin \
  0xf000   build/ota_data_initial.bin \
  0x20000  build/SipServer.bin
```

> The `ota_data_initial.bin` at `0xf000` points the bootloader at `ota_0`
> (offset `0x20000`) for the first boot. `idf.py flash` handles this for you.

### Migration note (single-`factory` → dual-OTA)

If the board previously ran a single-`factory` image, the partition layout
changes. The `nvs` partition stays at `0x9000`/`0x6000`, so saved Wi-Fi
credentials and the admin PIN *can* survive — but a full chip erase
(`esptool.py -p COM3 erase_flash`) wipes them. After a migration flash, expect
to re-onboard Wi-Fi and re-set the admin PIN.

---

## 4. Updating over-the-air (after the first USB flash)

Once a device is running an OTA-capable build, push new firmware without a
cable. OTA upload is gated by the admin session when a PIN is set, so log in
first (via `POST /api/admin/login`) to obtain the `pd_session` cookie.

```bash
# 1. (If a PIN is set) log in and capture the session cookie
curl -c cookies.txt -X POST \
     -H "Origin: http://192.168.4.1" -H "Host: 192.168.4.1" \
     -d "pin=YOUR_PIN" http://192.168.4.1/api/admin/login

# 2. Stream the new image to the inactive OTA slot
curl -b cookies.txt -X POST \
     -H "Origin: http://192.168.4.1" -H "Host: 192.168.4.1" \
     --data-binary @build/SipServer.bin \
     http://192.168.4.1/api/ota/upload

# 3. Reboot into the new image
curl -b cookies.txt -X POST \
     -H "Origin: http://192.168.4.1" -H "Host: 192.168.4.1" \
     http://192.168.4.1/api/ota/reboot
```

The bootloader brings the new image up in a *pending-verify* state. The firmware
confirms it automatically a few seconds after the SIP and HTTP servers come up
(see [OTA.md](OTA.md) §4); if the new image crashes on boot, the bootloader rolls
back to the previous slot — no bricking.

Check status any time:

```bash
curl http://192.168.4.1/api/ota/status
```

---

## 5. Troubleshooting

| Symptom | Fix |
|---------|-----|
| Port not found / permission denied | Install the CP210x/CH34x USB-UART driver; on Linux add yourself to the `dialout` group. |
| Flash fails / garbage output | Lower baud (`-b 115200`), or hold **BOOT** while connecting to force download mode. |
| Boots to old firmware after OTA | The new image failed verification and rolled back — check the serial log; rebuild and retry. |
| Colors look wrong on the display | Confirm the `display` transport build (`CONFIG_LV_COLOR_16_SWAP=y` is set in `sdkconfig.defaults`). |
| Wi-Fi creds / PIN lost after flashing | Expected after a full `erase_flash` or layout migration — re-onboard. |
