# Pocket-Dial ESP32 Firmware: Hardware Configuration Guide

This document defines the official hardware specifications, pin mappings, wiring diagrams, and Bills of Materials (BOM) for the supported microcontrollers and smart display boards running the **pocket-dial** firmware.

---

## 1. Supported Hardware Boards Overview

The **pocket-dial** firmware is designed to compile across multiple hardware targets. It supports two primary product configurations:
1. **Interactive Smart Displays**: Running an on-screen LVGL UI with real-time log scrolling, status fields, and a captive portal.
2. **Headless PoE Signalling Servers**: Compact, high-throughput, power-over-ethernet SIP servers designed for high packet-processing reliability.

Because developers deploy these on different target setups depending on hardware availability, the configuration is managed via preprocessor pin-defines and target SDK configs.

---

## 2. Guition JC3248W535 Smart Display (3.5" Black LCD)

The Guition JC3248W535 is an all-in-one Smart Display powered by an ESP32-S3. It includes a 3.5" high-resolution screen, a capacitive touchscreen, and a built-in battery ADC voltage divider.

```
       ┌────────────────────────────────────────────────────────┐
       │                                                        │
       │                   Guition JC3248W535                   │
       │                   320 x 480 QSPI TFT                   │
       │                      (AXS15231B)                       │
       │                                                        │
       └────────────────────────────────────────────────────────┘
          │ TFT SCK=47                │ I2C SDA=4
          │ TFT CS=45                 │ I2C SCL=8
          │ QSPI TFT Lines            │ I2C INT=11
          │ [21, 48, 40, 39]          │ I2C RST=12
          ▼                           ▼
      ┌────────────────────────────────────────────────────────┐
      │                      ESP32-S3R8                        │
      │        (8MB Octal PSRAM + 16MB QSPI Flash Chip)        │
      └────────────────────────────────────────────────────────┘
                       │ GPIO 5 (ADC1_CH4)
                       ▼
                 [ Battery divider ]
```

### Pin Assignments
* **Display Interface**: QSPI (Quad SPI) Panel Bus
* **Touch Controller**: AXS15231B via standard I2C Bus 0

| Hardware Pin Name | ESP32-S3 GPIO | Signal Type | Description |
| :--- | :---: | :---: | :--- |
| `TFT_CS` | **GPIO 45** | Output | Display SPI Chip Select |
| `TFT_SCK` | **GPIO 47** | Output | Display SPI Clock |
| `TFT_D0` | **GPIO 21** | Output / IO | QSPI Data Line 0 |
| `TFT_D1` | **GPIO 48** | Output / IO | QSPI Data Line 1 |
| `TFT_D2` | **GPIO 40** | Output / IO | QSPI Data Line 2 |
| `TFT_D3` | **GPIO 39** | Output / IO | QSPI Data Line 3 |
| `TFT_BL` | **GPIO 1** | Output | LED Backlight Control (High = Backlight ON) |
| `TOUCH_SDA` | **GPIO 4** | Bidirectional | Touch Controller I2C SDA (Pull-up Required) |
| `TOUCH_SCL` | **GPIO 8** | Output | Touch Controller I2C SCL (Pull-up Required) |
| `TOUCH_RST` | **GPIO 12** | Output | Touch Controller Reset Pin |
| `TOUCH_INT` | **GPIO 11** | Input | Touch Controller Interrupt Pin |
| `BATTERY_ADC` | **GPIO 5** | Analog Input | Voltage Divider ADC channel (Simulated or ADC1_CH4) |

> [!TIP]
> **PSRAM Configuration**: This board uses an ESP32-S3R8 chip containing 8MB of Octal PSRAM. In `sdkconfig`, PSRAM **must** be enabled in Octal mode at 80MHz, and LVGL double-frame buffers (320 * 480 * 2 bytes = 307.2 KB each) must be allocated in `MALLOC_CAP_SPIRAM` to prevent internal SRAM exhaustion.

---

## 3. Waveshare ESP32-S3-ETH + PoE Module

An industrial, ultra-compact ESP32-S3 board with a built-in PoE module and a high-performance W5500 SPI Ethernet controller.

```
       ┌──────────────────┐
       │     RJ45 PoE     │
       └────────┬─────────┘
                │
                ▼
       ┌──────────────────┐             ┌──────────────────┐
       │   W5500 MAC/PHY  │<─── SPI ───>│    ESP32-S3R8    │
       └──────────────────┘             └──────────────────┘
         SCLK: GPIO 12                    SCLK: SPI2_SCLK
         MISO: GPIO 13                    MISO: SPI2_MISO
         MOSI: GPIO 11                    MOSI: SPI2_MOSI
         CS:   GPIO 10                    CS:   SPI2_CS
         INT:  GPIO 14                    INT:  GPIO Pin Interrupt
```

### Pin Assignments
* **Ethernet Controller**: Wiznet W5500 MAC/PHY
* **SPI Host Device**: `SPI2_HOST` (FSPI)
* **Bus Speed**: 36 MHz (Supports up to 80 MHz)

| Signal Name | ESP32-S3 GPIO | Bus Signal | Description |
| :--- | :---: | :---: | :--- |
| `W5500_SCLK_GPIO` | **GPIO 12** | SPI2 SCLK | SPI Serial Clock |
| `W5500_MISO_GPIO` | **GPIO 13** | SPI2 MISO | SPI Master Input Slave Output |
| `W5500_MOSI_GPIO` | **GPIO 11** | SPI2 MOSI | SPI Master Output Slave Input |
| `W5500_CS_GPIO` | **GPIO 10** | SPI2 CS | SPI Chip Select (Active Low) |
| `W5500_INT_GPIO` | **GPIO 14** | Input | W5500 Hardware Interrupt Pin |
| `W5500_RST_GPIO` | **-1 (Unused)** | Reset | Not wired; reset handled via SPI soft commands |

---

## 4. LilyGO T-ETH-Lite W5500 (ESP32-S3 Target)

A widely used dev board featuring an integrated SD/TF card slot, 16MB flash, and 8MB PSRAM, coupled with a W5500 SPI Ethernet controller.

### Pin Assignments
* **Ethernet Controller**: Wiznet W5500 MAC/PHY
* **SPI Host Device**: Default hardware SPI

| Signal Name | ESP32-S3 GPIO | Description |
| :--- | :---: | :--- |
| `W5500_SCLK` | **GPIO 10** | SPI Clock |
| `W5500_MISO` | **GPIO 11** | SPI MISO |
| `W5500_MOSI` | **GPIO 12** | SPI MOSI |
| `W5500_CS` | **GPIO 9** | SPI Chip Select (CS) |
| `W5500_INT` | **GPIO 13** | Hardware Interrupt Pin |
| `W5500_RST` | **GPIO 14** | Hardware Reset Pin |

---

## 5. LilyGO T-POE-Pro LAN8720 (ESP32-WROVER-E Target)

A legacy ESP32-WROVER-E board with Power-over-Ethernet (PoE) and RMII-based LAN8720 Ethernet. It leverages the native ESP32 internal MAC.

```
       ┌──────────────────┐
       │     RJ45 PoE     │
       └────────┬─────────┘
                │
                ▼
       ┌──────────────────┐             ┌──────────────────┐
       │    LAN8720 PHY   │<─── RMII ──>│  ESP32 Internal  │
       └──────────────────┘             │  Ethernet MAC    │
         MDC:  GPIO 23                  └──────────────────┘
         MDIO: GPIO 18
         CLK:  GPIO 17 (OUT)
         RST:  GPIO 5 (Active Low)
```

### Pin Assignments
* **Ethernet PHY**: LAN8720 (RMII Mode)
* **PHY Address**: `0`
* **Reference Clock Mode**: `ETH_CLOCK_GPIO17_OUT` (50MHz clock generated by ESP32)

| Signal Name | ESP32 GPIO | RMII Signal | Description |
| :--- | :---: | :---: | :--- |
| `ETH_MDC_PIN` | **GPIO 23** | MDC | Management Data Clock |
| `ETH_MDIO_PIN` | **GPIO 18** | MDIO | Management Data Input/Output |
| `ETH_POWER_PIN` | **GPIO 5** | PHY_RST | PHY Active-low Reset and Power enable |
| `ETH_CLK_MODE` | **GPIO 17** | CLK_OUT | 50MHz Reference Clock output |

---

## 6. Generic Standalone AP (Wi-Fi Only)

For developers lacking Ethernet or smart displays, the firmware compiles onto any standard ESP32 / ESP32-S3 Development Board (e.g., ESP32-WROOM-32D, ESP32-S3-DevKitC-1).

* **Wi-Fi Mode**: SoftAP
* **Broadcast SSID**: `esp32-sipserver`
* **Network Password**: Open (WIFI_AUTH_OPEN) or configurable via NVS.
* **Fallback IP Gateway**: `192.168.4.1`

---

## 7. Bill of Materials (BOM) Estimates

The following table provides estimated BOM specifications for assembling a physical **pocket-dial** station.

| Component Name | Description | Manufacturer Part Number | Quantity | Est. Unit Price (USD) |
| :--- | :--- | :--- | :---: | :---: |
| **Guition Smart Display** | ESP32-S3 3.5" black display board (320x480) with case | Guition JC3248W535 | 1 | $14.50 |
| **PoE Ethernet Module** | ESP32-S3 core board with W5500 SPI Ethernet and PoE RJ45 | Waveshare ESP32-S3-ETH | 1 | $18.90 |
| **Ethernet Dev Board** | ESP32-S3 LilyGO board with W5500 SPI Ethernet | LilyGO T-ETH-Lite S3 | 1 | $15.50 |
| **RMII Ethernet Board** | ESP32-Wrover board with LAN8720 RMII Ethernet | LilyGO T-POE-Pro | 1 | $19.90 |
| **Battery Divider Resistors** | 100kΩ 1% and 220kΩ 1% resistors for battery ADC divider | Generic | 1 set | $0.10 |
| **USB-C Data Cable** | High-speed data cable for flashing and serial monitoring | Generic | 1 | $2.00 |

---

## 8. Assembly & Wiring Guidelines

### A. Touch I2C Pull-Up Requirements
On boards where the touch controller (AXS15231B) is connected, ensure that **4.7kΩ pull-up resistors** are soldered between:
* `TOUCH_SDA` (GPIO 4) and 3.3V power rails.
* `TOUCH_SCL` (GPIO 8) and 3.3V power rails.
Many JC3248W535 display clones omit these, resulting in I2C timeouts and immediate panel initialization crashes.

### B. High-Frequency SPI Ethernet Routing
If breadboarding the Waveshare or LilyGO W5500 SPI interfaces:
* Keep SPI line paths **shorter than 5 cm** to prevent clock skew.
* Ground lines should be bundled alongside high-speed clock signals (`SCK` and `MOSI`) to shield against EMI.
* High-speed SPI clock lines running at 36 MHz will experience significant crosstalk if unshielded or loosely jumpered.
