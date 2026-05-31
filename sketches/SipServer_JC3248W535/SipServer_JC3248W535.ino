/*
 * SipServer_JC3248W535 - Interactive SIP PBX for ESP32-S3 Smart Display
 *
 * Targets: Guition/Cheap-Black-Display JC3248W535 (ESP32-S3 + 3.5" IPS LCD + Cap Touch)
 *
 * Features:
 *   - Standalone Wi-Fi Access Point ("esp32-sipserver", open, 192.168.4.1)
 *   - SIP PBX Server on Port 5060
 *   - Retro CGA CRT Web Dashboard on Port 80
 *   - On-screen CGA CRT Dashboard with live status updates (IP, Uptime, Registrations, Calls)
 *   - Interactive capacitive touch screen:
 *       * [ SHOW WIFI QR ] - Shows a QR code for instant phone Wi-Fi connection
 *       * [ COLOR ]        - Toggles color theme: CGA Blue, Amber CRT, Green CRT
 *       * [ REBOOT ]       - Hardware reboot with confirmation prompt
 *   - Live scrolling console terminal on the bottom of the screen showing system logs
 *
 * Board Settings:
 *   - Board:           ESP32S3 Dev Module
 *   - PSRAM:           OPI PSRAM  (MANDATORY — the 320x480x16 canvas needs ~307 KB)
 *   - Flash Size:      16MB (128Mb)
 *   - Flash Mode:      QIO 80MHz
 *   - USB CDC On Boot: Enabled
 *
 * Required Libraries:
 *   - GFX Library for Arduino  (Arduino_GFX, by moononournation) >= 1.4.x
 *   - QRCode  (by ricmoo)  — for the Wi-Fi QR screen
 *
 * Display stack (Issue #40 fix):
 *   The previous JC3248W535EN-Touch-LCD library wraps Arduino_GFX internally but
 *   exposes no flush(). Replacing it with the raw Arduino_GFX + Arduino_Canvas +
 *   gfx->flush() pattern (proven in AXS15231B_CanvasTest) fixes the blank screen.
 *   Touch is replaced with a direct CST816S I2C poll (SDA 4, SCL 8, addr 0x15).
 */

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <qrcode.h>       // ricmoo/QRCode
#include <WiFi.h>
#include "SipServer.hpp"
#include "HttpServer.hpp"

// ── Bring-up diagnostics (Issue #40) ─────────────────────────────────────────
// The full app bootlooped with no serial output after the display migration,
// while the display-only bring-up sketch ran fine. These switches bisect the
// cause without re-architecting the sketch:
//   ENABLE_NETWORK 0       -> run display + touch ONLY (no WiFi/SIP/HTTP). If
//                             this renders & prints, the fault is in the network
//                             stack now running alongside an actively-DMAing panel.
//   DIAG_DISABLE_BROWNOUT 1-> turn off the brownout detector. If the bootloop
//                             stops, the reset is power-related (WiFi TX spikes +
//                             backlight + framebuffer DMA sagging the 3V3 rail).
// Default build is the normal full app with both diagnostics off.
#ifndef ENABLE_NETWORK
#define ENABLE_NETWORK 1
#endif
#ifndef DIAG_DISABLE_BROWNOUT
#define DIAG_DISABLE_BROWNOUT 0
#endif

#if DIAG_DISABLE_BROWNOUT
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#endif

// ── JC3248W535EN QSPI pin map ────────────────────────────────────────────────
#define TFT_CS   45
#define TFT_SCK  47
#define TFT_D0   21
#define TFT_D1   48
#define TFT_D2   40
#define TFT_D3   39
#define TFT_BL    1   // backlight; some revisions also use GPIO 19

// ── Proven display stack (Issue #40) ─────────────────────────────────────────
Arduino_DataBus *bus       = new Arduino_ESP32QSPI(TFT_CS, TFT_SCK, TFT_D0, TFT_D1, TFT_D2, TFT_D3);
Arduino_GFX    *raw_panel  = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED /*RST*/, 0 /*rotation*/, true /*IPS*/, 320, 480);
Arduino_GFX    *gfx        = new Arduino_Canvas(320, 480, raw_panel, 0, 0);

// ── CST816S capacitive touch (I2C) ───────────────────────────────────────────
#define TOUCH_SDA  4
#define TOUCH_SCL  8
#define CST816S_ADDR 0x15

static bool cst816s_read(uint16_t& tx, uint16_t& ty)
{
    Wire.beginTransmission(CST816S_ADDR);
    Wire.write(0x01);  // start at GestureID register
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint8_t)CST816S_ADDR, (uint8_t)6) < 6) return false;
    Wire.read();                         // gesture ID
    uint8_t fingers = Wire.read();       // finger count
    uint8_t xh = Wire.read();
    uint8_t xl = Wire.read();
    uint8_t yh = Wire.read();
    uint8_t yl = Wire.read();
    if (fingers == 0 || fingers > 5) return false;
    tx = ((uint16_t)(xh & 0x0F) << 8) | xl;
    ty = ((uint16_t)(yh & 0x0F) << 8) | yl;
    return true;
}

// ── GfxWrapper — drop-in replacement for JC3248W535EN screen.* calls ─────────
// Keeps every draw-function call site identical; only this wrapper changes.
struct GfxWrapper {
    uint16_t _color = 0xFFFF;

    bool begin() { return gfx->begin(); }

    void clear(uint8_t r, uint8_t g, uint8_t b) {
        gfx->fillScreen(gfx->color565(r, g, b));
    }
    void setColor(uint8_t r, uint8_t g, uint8_t b) {
        _color = gfx->color565(r, g, b);
    }
    void drawFillRect(int16_t x, int16_t y, int16_t w, int16_t h) {
        gfx->fillRect(x, y, w, h, _color);
    }
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h) {
        gfx->drawRect(x, y, w, h, _color);
    }
    void prt(const char* s, int16_t x, int16_t y, uint8_t scale) {
        gfx->setTextSize(scale);
        gfx->setTextColor(_color);
        gfx->setCursor(x, y);
        gfx->print(s);
    }
    void prt(const String& s, int16_t x, int16_t y, uint8_t scale) {
        prt(s.c_str(), x, y, scale);
    }
    bool getTouchPoint(uint16_t& x, uint16_t& y) {
        return cst816s_read(x, y);
    }
    // drawQRCode: compatible signature with JC3248W535EN library
    void drawQRCode(const char* text, int16_t x, int16_t y, uint8_t scale,
                    uint8_t bgR, uint8_t bgG, uint8_t bgB,
                    uint8_t fgR, uint8_t fgG, uint8_t fgB)
    {
        QRCode qrcode;
        uint8_t qrcodeData[qrcode_getBufferSize(5)];  // version 5 — fits 64 byte strings
        qrcode_initText(&qrcode, qrcodeData, 5, ECC_LOW, text);
        uint16_t bg = gfx->color565(bgR, bgG, bgB);
        uint16_t fg = gfx->color565(fgR, fgG, fgB);
        gfx->fillRect(x, y, qrcode.size * scale, qrcode.size * scale, bg);
        for (uint8_t qy = 0; qy < qrcode.size; qy++)
            for (uint8_t qx = 0; qx < qrcode.size; qx++)
                if (qrcode_getModule(&qrcode, qx, qy))
                    gfx->fillRect(x + qx * scale, y + qy * scale, scale, scale, fg);
    }
};

// ── Wi-Fi AP Configuration ──────────────────────────────────────────────────
static constexpr const char* AP_SSID        = "esp32-sipserver";
static constexpr const char* AP_PASSWORD    = "";        // Open network
static constexpr int         AP_CHANNEL     = 1;
static constexpr int         AP_MAX_CLIENTS = 10;

// ── SIP & HTTP Ports ────────────────────────────────────────────────────────
static constexpr const char* SIP_BIND_IP    = "192.168.4.1";
static constexpr int         SIP_PORT       = 5060;
static constexpr int         HTTP_PORT      = 80;

// ── Battery monitor (Issue #46) ──────────────────────────────────────────────
static constexpr int    BATTERY_ADC_PIN  = 5;
static constexpr float  BATTERY_DIVIDER  = 2.0f;
static constexpr unsigned long BATTERY_READ_INTERVAL_MS = 10000;
static float         batteryVolts   = 0.0f;
static int           batteryPercent = -1;
static unsigned long lastBatteryRead = 0;

// ── Globals ──────────────────────────────────────────────────────────────────
static SipServer*  server     = nullptr;
static HttpServer* httpServer = nullptr;
static GfxWrapper  screenObj;
static GfxWrapper& screen = screenObj;
static bool displayActive = false;

// ── Themes and UI State ─────────────────────────────────────────────────────
enum Theme {
    THEME_CGA_BLUE = 0,
    THEME_AMBER,
    THEME_GREEN,
    THEME_COUNT
};

static Theme currentTheme = THEME_CGA_BLUE;

struct RGBColor { uint8_t r, g, b; };

struct Palette {
    RGBColor bg;
    RGBColor text;
    RGBColor border;
    RGBColor highlight;
    RGBColor btnActive;
    RGBColor alert;
};

static const Palette PALETTES[THEME_COUNT] = {
    // 0: CGA Blue
    {
        {0, 0, 120},     // Deep Blue bg
        {255, 255, 255}, // White text
        {85, 255, 255},  // Cyan border
        {255, 255, 85},  // Yellow highlight
        {85, 255, 85},   // Green button
        {255, 85, 85}    // Red alert
    },
    // 1: Amber CRT
    {
        {0, 0, 0},       // Black bg
        {255, 160, 0},   // Amber text
        {255, 110, 0},   // Darker amber border
        {255, 210, 0},   // Light amber highlight
        {255, 180, 0},   // Amber button
        {255, 50, 0}     // Red-orange alert
    },
    // 2: Green Phosphor
    {
        {0, 0, 0},       // Black bg
        {50, 255, 50},   // Bright green text
        {0, 200, 0},     // Green border
        {180, 255, 180}, // Pale green highlight
        {0, 255, 0},     // Green button
        {255, 50, 50}    // Red alert
    }
};

// UI View States
enum ViewState {
    VIEW_DASHBOARD = 0,
    VIEW_WIFI_QR,
    VIEW_REBOOT_CONFIRM
};

static ViewState currentViewState = VIEW_DASHBOARD;

// Circular Buffer for Live Console Logs
static constexpr int MAX_LOG_LINES = 7;
static String logHistory[MAX_LOG_LINES];
static int logIndex = 0;
static int totalLogLines = 0;

// Tracking state for change detection
static int prevStations = -1;
static int prevExtensions = -1;
static int prevSessions = -1;
static unsigned long prevUptimeSec = 0;

// Forward declarations
void addLogLine(const String& line);
void redrawUI();
void drawDashboard();
void drawWiFiQR();
void drawRebootConfirm();
void handleTouch(uint16_t x, uint16_t y);
void readBattery();

// ── Log Handler Helper ───────────────────────────────────────────────────────
void addLogLine(const String& line) {
    Serial.println("[LOG] " + line);

    logHistory[logIndex] = line;
    logIndex = (logIndex + 1) % MAX_LOG_LINES;
    if (totalLogLines < MAX_LOG_LINES) totalLogLines++;

    if (currentViewState == VIEW_DASHBOARD && displayActive) {
        const Palette& p = PALETTES[currentTheme];

        // Redraw only the console terminal area (no full-screen flush needed)
        screen.setColor(p.bg.r, p.bg.g, p.bg.b);
        screen.drawFillRect(12, 321, 296, 148);

        screen.setColor(p.text.r, p.text.g, p.text.b);
        int printIdx = (logIndex - totalLogLines + MAX_LOG_LINES) % MAX_LOG_LINES;
        for (int i = 0; i < totalLogLines; i++) {
            screen.prt(logHistory[printIdx].c_str(), 18, 326 + (i * 20), 1);
            printIdx = (printIdx + 1) % MAX_LOG_LINES;
        }
        gfx->flush();  // push log-area update to panel
    }
}

// ── Battery monitor (Issue #46) ──────────────────────────────────────────────
void readBattery() {
    uint32_t mv = analogReadMilliVolts(BATTERY_ADC_PIN);
    batteryVolts = (mv * BATTERY_DIVIDER) / 1000.0f;

    float pct = (batteryVolts - 3.30f) / (4.20f - 3.30f) * 100.0f;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    batteryPercent = (int)(pct + 0.5f);

    Serial.printf("[BATT] %.2f V (%d%%)\n", batteryVolts, batteryPercent);
}

void drawBatteryHeader() {
    if (!displayActive || batteryPercent < 0) return;
    const Palette& p = PALETTES[currentTheme];
    screen.setColor(p.border.r, p.border.g, p.border.b);
    screen.drawFillRect(232, 4, 86, 24);
    char b[24];
    sprintf(b, "%.1fV %d%%", batteryVolts, batteryPercent);
    screen.setColor(p.bg.r, p.bg.g, p.bg.b);
    screen.prt(b, 234, 10, 1);
}

// ── UI Drawing Orchestrator ──────────────────────────────────────────────────
void redrawUI() {
    const Palette& p = PALETTES[currentTheme];
    screen.clear(p.bg.r, p.bg.g, p.bg.b);

    switch (currentViewState) {
        case VIEW_DASHBOARD:    drawDashboard();    break;
        case VIEW_WIFI_QR:      drawWiFiQR();       break;
        case VIEW_REBOOT_CONFIRM: drawRebootConfirm(); break;
    }

    gfx->flush();  // push the completed frame to the panel
}

// Draw Dashboard Screen
void drawDashboard() {
    const Palette& p = PALETTES[currentTheme];

    // 1. Header Bar
    screen.setColor(p.border.r, p.border.g, p.border.b);
    screen.drawFillRect(0, 0, 320, 32);
    screen.setColor(p.bg.r, p.bg.g, p.bg.b);
    screen.prt("══ POCKET-DIAL SWITCHBOARD ══", 10, 10, 1);
    drawBatteryHeader();

    // 2. Status Box
    screen.setColor(p.border.r, p.border.g, p.border.b);
    screen.drawRect(10, 42, 300, 150);
    screen.drawRect(11, 43, 298, 148);

    screen.setColor(p.highlight.r, p.highlight.g, p.highlight.b);
    screen.prt("☼ SYSTEM STATUS", 20, 50, 1);

    screen.setColor(p.text.r, p.text.g, p.text.b);
    screen.prt("Server IP : 192.168.4.1", 20, 72, 1);
    screen.prt("SIP Port  : 5060", 20, 92, 1);

    char buf[64];
    unsigned long uptimeMs = millis();
    unsigned long hrs = uptimeMs / 3600000;
    unsigned long mins = (uptimeMs % 3600000) / 60000;
    unsigned long secs = (uptimeMs % 60000) / 1000;
    sprintf(buf, "Uptime    : %02lu:%02lu:%02lu", hrs, mins, secs);
    screen.prt(buf, 20, 112, 1);

    int stations = WiFi.softAPgetStationNum();
    sprintf(buf, "Stations  : %d connected", stations);
    screen.prt(buf, 20, 132, 1);

    int clients = server ? server->getHandler().getClientCount() : 0;
    sprintf(buf, "Extensions: %d active", clients);
    screen.prt(buf, 20, 152, 1);

    int sessions = server ? server->getHandler().getSessionCount() : 0;
    sprintf(buf, "Sessions  : %d active", sessions);
    screen.prt(buf, 20, 172, 1);

    // 3. Interactive Buttons Area
    screen.setColor(p.btnActive.r, p.btnActive.g, p.btnActive.b);
    screen.drawFillRect(15, 202, 290, 36);
    screen.setColor(0, 0, 0);
    screen.prt("[ TAP FOR WI-FI QR CODE ]", 45, 212, 1);

    screen.setColor(p.highlight.r, p.highlight.g, p.highlight.b);
    screen.drawFillRect(15, 248, 140, 32);
    screen.setColor(0, 0, 0);
    screen.prt("[ COLOR ]", 45, 258, 1);

    screen.setColor(p.alert.r, p.alert.g, p.alert.b);
    screen.drawFillRect(165, 248, 140, 32);
    screen.setColor(255, 255, 255);
    screen.prt("[ REBOOT ]", 195, 258, 1);

    // 4. Console Logs Box
    screen.setColor(p.border.r, p.border.g, p.border.b);
    screen.drawRect(10, 295, 300, 178);
    screen.drawRect(11, 296, 298, 176);
    screen.drawFillRect(15, 298, 290, 20);
    screen.setColor(p.bg.r, p.bg.g, p.bg.b);
    screen.prt("═══ Live Console Logs ═══", 45, 302, 1);

    screen.setColor(p.text.r, p.text.g, p.text.b);
    int printIdx = (logIndex - totalLogLines + MAX_LOG_LINES) % MAX_LOG_LINES;
    for (int i = 0; i < totalLogLines; i++) {
        screen.prt(logHistory[printIdx].c_str(), 18, 326 + (i * 20), 1);
        printIdx = (printIdx + 1) % MAX_LOG_LINES;
    }
}

// Draw Wi-Fi AP QR Code Screen
void drawWiFiQR() {
    const Palette& p = PALETTES[currentTheme];

    screen.setColor(p.border.r, p.border.g, p.border.b);
    screen.drawFillRect(0, 0, 320, 32);
    screen.setColor(p.bg.r, p.bg.g, p.bg.b);
    screen.prt("═══ Wi-Fi Quick Join QR ═══", 20, 10, 1);

    screen.setColor(255, 255, 255);
    screen.drawFillRect(30, 60, 260, 260);

    String qrString = "WIFI:S:" + String(AP_SSID) + ";T:nopass;;";
    screen.drawQRCode(qrString.c_str(), 45, 75, 5, 255, 255, 255, 0, 0, 0);

    screen.setColor(p.highlight.r, p.highlight.g, p.highlight.b);
    screen.prt("Scan with your phone to", 35, 340, 1);
    screen.prt("connect to SoftAP automatically.", 12, 360, 1);

    screen.setColor(p.text.r, p.text.g, p.text.b);
    screen.prt("SSID: " + String(AP_SSID), 30, 395, 1);
    screen.prt("Pass: (None - Open)", 30, 415, 1);

    screen.setColor(p.btnActive.r, p.btnActive.g, p.btnActive.b);
    screen.drawFillRect(15, 440, 290, 32);
    screen.setColor(0, 0, 0);
    screen.prt("[ TAP ANYWHERE TO CLOSE ]", 45, 450, 1);
}

// Draw Hardware Reboot Confirmation Screen
void drawRebootConfirm() {
    const Palette& p = PALETTES[currentTheme];

    screen.setColor(p.alert.r, p.alert.g, p.alert.b);
    screen.drawRect(20, 120, 280, 220);
    screen.drawRect(21, 121, 278, 218);
    screen.drawFillRect(25, 123, 270, 40);

    screen.setColor(255, 255, 255);
    screen.prt("☣ SYSTEM WARNING ☣", 45, 133, 1);

    screen.setColor(p.text.r, p.text.g, p.text.b);
    screen.prt("Are you sure you want to", 35, 190, 1);
    screen.prt("reboot the SIP PBX board?", 35, 210, 1);

    screen.setColor(p.alert.r, p.alert.g, p.alert.b);
    screen.drawFillRect(40, 260, 100, 40);
    screen.setColor(255, 255, 255);
    screen.prt("YES", 75, 272, 1);

    screen.setColor(p.border.r, p.border.g, p.border.b);
    screen.drawFillRect(180, 260, 100, 40);
    screen.setColor(0, 0, 0);
    screen.prt("NO", 215, 272, 1);
}

// ── Capacitive Touch Coordinator ─────────────────────────────────────────────
void handleTouch(uint16_t x, uint16_t y) {
    Serial.printf("[TOUCH] Pressed at X: %d, Y: %d\n", x, y);

    if (currentViewState == VIEW_WIFI_QR) {
        currentViewState = VIEW_DASHBOARD;
        redrawUI();
        delay(200);
        return;
    }

    if (currentViewState == VIEW_REBOOT_CONFIRM) {
        if (x >= 40 && x <= 140 && y >= 260 && y <= 300) {
            screen.clear(0, 0, 0);
            screen.setColor(255, 50, 50);
            screen.prt("Rebooting PBX...", 65, 220, 2);
            gfx->flush();
            Serial.println("[SYSTEM] Hard resetting board!");
            delay(1000);
            ESP.restart();
        } else if (x >= 180 && x <= 280 && y >= 260 && y <= 300) {
            currentViewState = VIEW_DASHBOARD;
            redrawUI();
            delay(200);
        }
        return;
    }

    if (currentViewState == VIEW_DASHBOARD) {
        // [ SHOW WI-FI QR ] — Y: 202..238
        if (x >= 15 && x <= 305 && y >= 202 && y <= 238) {
            currentViewState = VIEW_WIFI_QR;
            redrawUI();
            delay(200);
            return;
        }
        // [ COLOR ] — X: 15..155, Y: 248..280
        if (x >= 15 && x <= 155 && y >= 248 && y <= 280) {
            currentTheme = (Theme)((currentTheme + 1) % THEME_COUNT);
            redrawUI();
            delay(200);
            return;
        }
        // [ REBOOT ] — X: 165..305, Y: 248..280
        if (x >= 165 && x <= 305 && y >= 248 && y <= 280) {
            currentViewState = VIEW_REBOOT_CONFIRM;
            redrawUI();
            delay(200);
            return;
        }
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup()
{
#if DIAG_DISABLE_BROWNOUT
    // Must be the very first thing: kill the brownout detector before the
    // display/WiFi current spikes can trip it. Diagnostic only — if this
    // "fixes" the bootloop, the real problem is power, not code.
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif

    Serial.begin(115200);
    // Native USB CDC takes ~1-2s to enumerate; in a fast bootloop the banner
    // is printed before the host opens the port and is lost. Wait for the host
    // (bounded) so early diagnostics actually reach the monitor.
    unsigned long t0 = millis();
    while (!Serial && (millis() - t0) < 3000) { delay(10); }
    delay(200);

    Serial.println();
    Serial.println("╔══════════════════════════════════════╗");
    Serial.println("║    JC3248W535 Smart SIP Switchboard  ║");
    Serial.println("║        Retro-Console Edition         ║");
    Serial.println("╚══════════════════════════════════════╝");
    Serial.printf("[BOOT] reset reason: %d  (1=POWERON 8=TG1WDT 12=SW_CPU 15=BROWNOUT)\n",
                  (int)esp_reset_reason());
    Serial.printf("[BUILD] ENABLE_NETWORK=%d DIAG_DISABLE_BROWNOUT=%d\n",
                  ENABLE_NETWORK, DIAG_DISABLE_BROWNOUT);
    Serial.println();

    // Backlight — drive both pins; different board revisions use one or the other
    pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
    pinMode(19, OUTPUT);     digitalWrite(19, HIGH);

    Serial.printf("[SYSTEM] PSRAM found: %s\n", psramFound() ? "YES" : "NO");
    Serial.printf("[SYSTEM] Free PSRAM: %d KB\n", (int)(ESP.getFreePsram() / 1024));
    Serial.printf("[SYSTEM] Free Heap: %d KB\n",  (int)(ESP.getFreeHeap() / 1024));
    if (!psramFound())
        Serial.println("[SYSTEM] WARNING: OPI PSRAM not detected. "
                       "The 307 KB canvas will fail. Set Tools > PSRAM = OPI PSRAM.");

    // ── 1. Initialise Touch I2C ─────────────────────────────────────────────
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    Serial.println("[TOUCH] CST816S I2C initialised (SDA=4, SCL=8, addr=0x15).");

    // ── 2. Initialise Display (Arduino_GFX stack) ───────────────────────────
    Serial.println("[GFX] Starting Arduino_GFX + Canvas stack (Issue #40 fix)...");
    displayActive = screen.begin();
    if (!displayActive) {
        Serial.println("[GFX] WARNING: gfx->begin() returned false! "
                       "Check OPI PSRAM setting and QSPI pin map. Running headless.");
        // Re-assert backlight in case begin() toggled it
        digitalWrite(TFT_BL, HIGH);
        digitalWrite(19, HIGH);
    } else {
        Serial.println("[GFX] begin() OK.");
    }

    for (int i = 0; i < MAX_LOG_LINES; i++) logHistory[i] = "";
    currentViewState = VIEW_DASHBOARD;
    if (displayActive) redrawUI();

    addLogLine("Display Initialized.");
    addLogLine("AXS15231B LCD Active.");
    addLogLine("CST816S Touch Polling.");

#if ENABLE_NETWORK
    // ── 3. Start Wi-Fi Access Point ─────────────────────────────────────────
    addLogLine("Starting WiFi SoftAP...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CLIENTS);
    // Cap TX power: lowers the current spikes that can sag the 3V3 rail while
    // the panel is actively DMA-ing a framebuffer (a likely bootloop cause).
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    delay(500);

    char wifiLog[64];
    sprintf(wifiLog, "AP Ready. IP: %s", WiFi.softAPIP().toString().c_str());
    addLogLine(wifiLog);

    // ── 4. Start SIP PBX Server ─────────────────────────────────────────────
    sprintf(wifiLog, "Binding SIP to %s:%d...", SIP_BIND_IP, SIP_PORT);
    addLogLine(wifiLog);
    try {
        server = new SipServer(std::string(SIP_BIND_IP), SIP_PORT);
        addLogLine("SIP PBX Server is RUNNING.");
    } catch (const std::exception& e) {
        addLogLine("SIP ERROR: Failed to bind.");
        while (true) { delay(1000); }
    }

    // ── 5. Start HTTP Web Dashboard ─────────────────────────────────────────
    addLogLine("Starting Web server...");
    try {
        httpServer = new HttpServer(std::string(SIP_BIND_IP), HTTP_PORT, server->getHandler());
        httpServer->start();
        addLogLine("Web Dashboard is RUNNING.");
    } catch (const std::exception& e) {
        addLogLine("Web server failed to start.");
    }

    addLogLine("System ready for calls.");
#else
    addLogLine("NETWORK DISABLED (diag build).");
    addLogLine("Display + touch only.");
#endif
    Serial.println("──────────────────────────────────────────────────────────");
}

// ── Main Loop ─────────────────────────────────────────────────────────────────
void loop()
{
    // 1. Tick SIP Server logic
    if (server) server->getHandler().tick();

    // 2. Poll Capacitive Touch
    uint16_t touchX = 0, touchY = 0;
    if (screen.getTouchPoint(touchX, touchY))
        handleTouch(touchX, touchY);

    // 3. Periodic battery sample
    unsigned long now = millis();
    if (lastBatteryRead == 0 || now - lastBatteryRead >= BATTERY_READ_INTERVAL_MS) {
        lastBatteryRead = now;
        readBattery();
    }

    // 4. Wi-Fi station change detection
    int currentStations = 0;
#if ENABLE_NETWORK
    currentStations = WiFi.softAPgetStationNum();
    if (currentStations != prevStations) {
        if (prevStations != -1) {
            if (currentStations > prevStations)
                addLogLine("Station connected! Total: " + String(currentStations));
            else
                addLogLine("Station disconnected! Total: " + String(currentStations));
        }
        prevStations = currentStations;
    }

    // 5. SIP extension / session change detection
    if (server) {
        int currentExtensions = server->getHandler().getClientCount();
        if (currentExtensions != prevExtensions) {
            if (prevExtensions != -1) {
                addLogLine(currentExtensions > prevExtensions
                           ? "SIP extension registered!" : "SIP extension de-registered!");
            }
            prevExtensions = currentExtensions;
        }

        int currentSessions = server->getHandler().getSessionCount();
        if (currentSessions != prevSessions) {
            if (prevSessions != -1) {
                addLogLine(currentSessions > prevSessions
                           ? "VoIP Session established!" : "VoIP Session terminated.");
            }
            prevSessions = currentSessions;
        }
    }
#endif

    // 6. Partial status-area update every second
    if (currentViewState == VIEW_DASHBOARD && displayActive) {
        unsigned long uptimeSec = now / 1000;
        if (uptimeSec != prevUptimeSec) {
            prevUptimeSec = uptimeSec;
            const Palette& p = PALETTES[currentTheme];

            screen.setColor(p.bg.r, p.bg.g, p.bg.b);
            screen.drawFillRect(110, 105, 185, 80);

            screen.setColor(p.text.r, p.text.g, p.text.b);
            char buf[64];

            unsigned long hrs  = uptimeSec / 3600;
            unsigned long mins = (uptimeSec % 3600) / 60;
            unsigned long secs = uptimeSec % 60;
            sprintf(buf, "%02lu:%02lu:%02lu", hrs, mins, secs);
            screen.prt(buf, 110, 112, 1);

            sprintf(buf, "%d connected", currentStations);
            screen.prt(buf, 110, 132, 1);

            int clients = server ? server->getHandler().getClientCount() : 0;
            sprintf(buf, "%d active", clients);
            screen.prt(buf, 110, 152, 1);

            int sessions = server ? server->getHandler().getSessionCount() : 0;
            sprintf(buf, "%d active", sessions);
            screen.prt(buf, 110, 172, 1);

            drawBatteryHeader();
            gfx->flush();  // push status-area partial update to panel
        }
    }

    delay(30);
}
