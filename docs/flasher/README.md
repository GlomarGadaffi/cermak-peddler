# Browser flasher

`index.html` is a single, dependency-free page that installs pocket-dial onto
an ESP32-S3 board over USB, using the [Web Serial API][webserial] to drive
[esptool-js][esptool-js] entirely inside the browser. It is served from GitHub
Pages at:

**<https://glomargadaffi.github.io/pocket-dial/flasher/>**

Nothing is uploaded anywhere. The page fetches firmware images from this
repository's GitHub Releases and writes them to the board from the user's own
machine.

## Requirements

* **Chrome, Edge, or Opera on desktop.** Web Serial exists nowhere else — not
  Firefox, not Safari, not any mobile browser. The page detects this and shows
  an explanation instead of a broken UI.
* **HTTPS or `localhost`.** Web Serial requires a secure context. GitHub Pages
  is HTTPS; for local testing serve over `http://localhost`.
* **No other program holding the serial port.** `idf.py monitor`, PuTTY, the
  Arduino IDE, and VS Code serial terminals all take exclusive ownership of a
  COM port on Windows. The page says so up front and translates the resulting
  error into that advice.

## How it works

1. **Connect.** `navigator.serial.requestPort()` → `Transport` → `ESPLoader`.
   `loader.main()` syncs with the ROM bootloader, identifies the chip, uploads
   the flasher stub, and raises the baud rate to 921600. Everything afterwards
   runs on that one connection.

2. **Report what's installed.** The page reads `esp_app_desc_t` back out of
   both OTA slots (app partition + `0x20`, i.e. `0x20020` for `ota_0` and
   `0x620020` for `ota_1`) and `otadata` at `0xf000` to say which slot is
   live. That yields the project name (`SipServer`) and version of whatever is
   on the board. It does **not** identify the variant — the firmware doesn't
   stamp its transport into the descriptor — so the user always picks the
   board. Every published build is ESP32-S3, so a non-S3 chip gets a warning
   pointing at the from-source `lan8720` build.

3. **Fetch releases.** One call to
   `https://api.github.com/repos/GlomarGadaffi/pocket-dial/releases?per_page=15`
   populates a picker; drafts are hidden and the newest non-pre-release is
   selected by default. Rate limiting (60 unauthenticated requests per hour
   per IP), an empty release list, and network errors are all explained
   states that steer the user to the local-file panel.

4. **Resolve the images.** Two release layouts are understood:

   * **`manifest.json`** (preferred; written by `.github/workflows/release.yml`):

     ```json
     {
       "version": "v1.4.0",
       "variants": {
         "esp32s3-eth": {
           "name": "esp32s3-eth",
           "flash": { "size": "16MB", "mode": "dio", "freq": "80m" },
           "parts": [
             { "offset": 0,      "file": "bootloader-esp32s3-eth.bin",       "role": "bootloader",      "size": 20000 },
             { "offset": 32768,  "file": "partition-table-esp32s3-eth.bin",  "role": "partition-table", "size": 3072 },
             { "offset": 61440,  "file": "ota_data_initial-esp32s3-eth.bin", "role": "ota-data",        "size": 8192 },
             { "offset": 131072, "file": "SipServer-esp32s3-eth.bin",        "role": "app",             "size": 950000 }
           ]
         }
       }
     }
     ```

     Offsets are decimal. `role` is what lets *App only* mode pick its two
     parts without hardcoding `0xf000`/`0x20000` in the page.

   * **Bare naming convention** (v1.2.0 and earlier): `bootloader-<id>.bin`,
     `partition-table-<id>.bin`, `SipServer-<id>.bin`, with offsets and flash
     mode taken from `flasher_args-<id>.json` when it's attached. Those
     releases ship no `ota_data_initial.bin`; the page synthesises it (8 KB of
     `0xFF`, which tells the bootloader to boot `ota_0`). The hand-cut
     `v1.3.0-pre-alpha` names are matched too.

   Variant ids (`esp32s3-eth`, `esp32s3-display`, `esp32s3-wifi`) are the
   `<chip>-<transport>` tag the release workflow uses; `VARIANTS` in
   `index.html` and the matrix in `release.yml` must stay in sync.

5. **Flash.** Every image is downloaded *before* the first byte is written.
   `writeFlash` runs with `eraseAll: false` by default — that is what preserves
   NVS (saved Wi-Fi, admin PIN, extensions) — and `compress: true`. An
   *Erase the whole chip first* checkbox flips `eraseAll` on for the
   single-`factory` → dual-OTA migration described in `FLASHING.md`.

6. **Reset.** `loader.after("hard_reset", usingUsbOtg)`, choosing the USB-OTG
   reset path when `transport.getPid()` matches `loader.USB_JTAG_SERIAL_PID`
   (native USB-Serial-JTAG) and DTR/RTS otherwise (USB-UART bridge).

## Flash modes

| Mode | Writes | When |
| ---- | ------ | ---- |
| **Full flash** | bootloader `0x0`, partition table `0x8000`, otadata `0xf000`, app `0x20000` | First install, and after any partition-table or bootloader change |
| **App only** | otadata `0xf000`, app `0x20000` | Upgrading firmware already on the board — the same regions the OTA path touches |

There is also a **Flash your own build** panel: a file input per region with
an editable offset, for people building locally who don't want to install a
toolchain's flasher.

## Local development

```powershell
python -m http.server 8000
# then open http://localhost:8000/docs/flasher/
```

esptool-js is imported as an ES module from jsDelivr at a **pinned exact
version** (`0.6.1`). Bump it deliberately: `FlashOptions.fileArray[].data` is
a `Uint8Array` in 0.6.x, and the API has changed shape across releases.

`docs/.nojekyll` disables Jekyll for the Pages build so nothing in the
JavaScript is mistaken for Liquid templating.

[webserial]: https://developer.mozilla.org/docs/Web/API/Web_Serial_API
[esptool-js]: https://github.com/espressif/esptool-js
