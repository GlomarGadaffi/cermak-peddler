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
 *   - Board: ESP32S3 Dev Module
 *   - PSRAM: OPI PSRAM (Mandatory for AXS15231B display)
 *   - Flash Size: 16MB (128Mb)
 *   - USB CDC On Boot: Enabled
 *   - Flash Mode: QIO 80MHz
 *
 * Required Libraries:
 *   - JC3248W535EN-Touch-LCD (by AudunKodehode)
 *   - Arduino_GFX_Library (by Moon On Our Nation - dependency)
 */

#include <WiFi.h>
#include <JC3248W535EN-Touch-LCD.h>
#include "SipServer.hpp"
#include "HttpServer.hpp"

// ── Wi-Fi AP Configuration ──────────────────────────────────────────────────
static constexpr const char* AP_SSID        = "esp32-sipserver";
static constexpr const char* AP_PASSWORD    = "";        // Open network
static constexpr int         AP_CHANNEL     = 1;
static constexpr int         AP_MAX_CLIENTS = 10;

// ── SIP & HTTP Ports ────────────────────────────────────────────────────────
static constexpr const char* SIP_BIND_IP    = "192.168.4.1";
static constexpr int         SIP_PORT       = 5060;
static constexpr int         HTTP_PORT      = 80;

// ── Globals ─────────────────────────────────────────────────────────────────
static SipServer*  server     = nullptr;
static HttpServer* httpServer = nullptr;
static JC3248W535EN* screenPtr = nullptr;
#define screen (*screenPtr)
static bool displayActive = false;

// ── Themes and UI State ─────────────────────────────────────────────────────
enum Theme {
    THEME_CGA_BLUE = 0,
    THEME_AMBER,
    THEME_GREEN,
    THEME_COUNT
};

static Theme currentTheme = THEME_CGA_BLUE;

// RGB Color Struct (using the library's setColor RGB args)
struct RGBColor {
    uint8_t r, g, b;
};

// Color palettes for the themes
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

// ── Log Handler Helper ──────────────────────────────────────────────────────
void addLogLine(const String& line) {
    Serial.println("[LOG] " + line);
    
    // Add to circular buffer
    logHistory[logIndex] = line;
    logIndex = (logIndex + 1) % MAX_LOG_LINES;
    if (totalLogLines < MAX_LOG_LINES) {
        totalLogLines++;
    }
    
    // If we are currently displaying the dashboard, update only the terminal area
    if (currentViewState == VIEW_DASHBOARD) {
        // Redraw console terminal frame and lines
        const Palette& p = PALETTES[currentTheme];
        
        // Terminal background clear
        screen.setColor(p.bg.r, p.bg.g, p.bg.b);
        screen.drawFillRect(12, 321, 296, 148);
        
        // Draw logs text
        screen.setColor(p.text.r, p.text.g, p.text.b);
        int printIdx = (logIndex - totalLogLines + MAX_LOG_LINES) % MAX_LOG_LINES;
        for (int i = 0; i < totalLogLines; i++) {
            screen.prt(logHistory[printIdx].c_str(), 18, 326 + (i * 20), 1);
            printIdx = (printIdx + 1) % MAX_LOG_LINES;
        }
    }
}

// ── UI Drawing Orchestrator ─────────────────────────────────────────────────
void redrawUI() {
    const Palette& p = PALETTES[currentTheme];
    screen.clear(p.bg.r, p.bg.g, p.bg.b);
    
    switch (currentViewState) {
        case VIEW_DASHBOARD:
            drawDashboard();
            break;
        case VIEW_WIFI_QR:
            drawWiFiQR();
            break;
        case VIEW_REBOOT_CONFIRM:
            drawRebootConfirm();
            break;
    }
}

// Draw Dashboard Screen
void drawDashboard() {
    const Palette& p = PALETTES[currentTheme];
    
    // 1. Header Bar
    screen.setColor(p.border.r, p.border.g, p.border.b);
    screen.drawFillRect(0, 0, 320, 32);
    screen.setColor(p.bg.r, p.bg.g, p.bg.b); // Text stands out against block header
    screen.prt("══ POCKET-DIAL SWITCHBOARD ══", 10, 10, 1);
    
    // 2. Status Box
    screen.setColor(p.border.r, p.border.g, p.border.b);
    // Draw Box Borders (Double line effect via offset)
    screen.drawRect(10, 42, 300, 150);
    screen.drawRect(11, 43, 298, 148);
    
    screen.setColor(p.highlight.r, p.highlight.g, p.highlight.b);
    screen.prt("☼ SYSTEM STATUS", 20, 50, 1);
    
    // Status text values will be updated by active loop, draw labels now
    screen.setColor(p.text.r, p.text.g, p.text.b);
    screen.prt("Server IP : 192.168.4.1", 20, 72, 1);
    screen.prt("SIP Port  : 5060", 20, 92, 1);
    
    // Interactive state lines (drawn dynamically)
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
    // Button 1: [ SHOW WI-FI QR ]
    screen.setColor(p.btnActive.r, p.btnActive.g, p.btnActive.b);
    screen.drawFillRect(15, 202, 290, 36);
    screen.setColor(0, 0, 0); // High contrast dark text on active green btn
    screen.prt("[ TAP FOR WI-FI QR CODE ]", 45, 212, 1);
    
    // Button 2: [ COLOR ]
    screen.setColor(p.highlight.r, p.highlight.g, p.highlight.b);
    screen.drawFillRect(15, 248, 140, 32);
    screen.setColor(0, 0, 0);
    screen.prt("[ COLOR ]", 45, 258, 1);
    
    // Button 3: [ REBOOT ]
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
    
    // Draw logs text lines
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
    
    // Header
    screen.setColor(p.border.r, p.border.g, p.border.b);
    screen.drawFillRect(0, 0, 320, 32);
    screen.setColor(p.bg.r, p.bg.g, p.bg.b);
    screen.prt("═══ Wi-Fi Quick Join QR ═══", 20, 10, 1);
    
    // Draw QR code background wrapper
    screen.setColor(255, 255, 255);
    screen.drawFillRect(30, 60, 260, 260);
    
    // Generate and draw the QR code for open network connection
    // WIFI:S:<SSID>;T:<nopass>;P:<PASSWORD>;;
    String qrString = "WIFI:S:" + String(AP_SSID) + ";T:nopass;;";
    
    // drawQRCode(text, x, y, scale, bgR, bgG, bgB, fgR, fgG, fgB)
    screen.drawQRCode(qrString.c_str(), 45, 75, 5, 255, 255, 255, 0, 0, 0);
    
    // Helper Text
    screen.setColor(p.highlight.r, p.highlight.g, p.highlight.b);
    screen.prt("Scan with your phone to", 35, 340, 1);
    screen.prt("connect to SoftAP automatically.", 12, 360, 1);
    
    screen.setColor(p.text.r, p.text.g, p.text.b);
    screen.prt("SSID: " + String(AP_SSID), 30, 395, 1);
    screen.prt("Pass: (None - Open)", 30, 415, 1);
    
    // Close Button Tap Indicator
    screen.setColor(p.btnActive.r, p.btnActive.g, p.btnActive.b);
    screen.drawFillRect(15, 440, 290, 32);
    screen.setColor(0, 0, 0);
    screen.prt("[ TAP ANYWHERE TO CLOSE ]", 45, 450, 1);
}

// Draw Hardware Reboot Confirmation Screen
void drawRebootConfirm() {
    const Palette& p = PALETTES[currentTheme];
    
    // Draw Box
    screen.setColor(p.alert.r, p.alert.g, p.alert.b);
    screen.drawRect(20, 120, 280, 220);
    screen.drawRect(21, 121, 278, 218);
    screen.drawFillRect(25, 123, 270, 40);
    
    // Header Text
    screen.setColor(255, 255, 255);
    screen.prt("☣ SYSTEM WARNING ☣", 45, 133, 1);
    
    screen.setColor(p.text.r, p.text.g, p.text.b);
    screen.prt("Are you sure you want to", 35, 190, 1);
    screen.prt("reboot the SIP PBX board?", 35, 210, 1);
    
    // YES Button
    screen.setColor(p.alert.r, p.alert.g, p.alert.b);
    screen.drawFillRect(40, 260, 100, 40);
    screen.setColor(255, 255, 255);
    screen.prt("YES", 75, 272, 1);
    
    // NO Button
    screen.setColor(p.border.r, p.border.g, p.border.b);
    screen.drawFillRect(180, 260, 100, 40);
    screen.setColor(0, 0, 0);
    screen.prt("NO", 215, 272, 1);
}

// ── Capacitive Touch Coordinator ───────────────────────────────────────────
void handleTouch(uint16_t x, uint16_t y) {
    Serial.printf("[TOUCH] Pressed at X: %d, Y: %d\n", x, y);
    
    if (currentViewState == VIEW_WIFI_QR) {
        // Any tap closes the QR overlay
        currentViewState = VIEW_DASHBOARD;
        redrawUI();
        delay(200); // Debounce
        return;
    }
    
    if (currentViewState == VIEW_REBOOT_CONFIRM) {
        // YES Button: X: 40..140, Y: 260..300
        if (x >= 40 && x <= 140 && y >= 260 && y <= 300) {
            screen.clear(0, 0, 0);
            screen.setColor(255, 50, 50);
            screen.prt("Rebooting PBX...", 65, 220, 2);
            Serial.println("[SYSTEM] Hard resetting board!");
            delay(1000);
            ESP.restart();
        }
        // NO Button: X: 180..280, Y: 260..300
        else if (x >= 180 && x <= 280 && y >= 260 && y <= 300) {
            currentViewState = VIEW_DASHBOARD;
            redrawUI();
            delay(200); // Debounce
        }
        return;
    }
    
    if (currentViewState == VIEW_DASHBOARD) {
        // Button 1: [ SHOW WI-FI QR ] -> X: 15..305, Y: 202..238
        if (x >= 15 && x <= 305 && y >= 202 && y <= 238) {
            currentViewState = VIEW_WIFI_QR;
            redrawUI();
            delay(200);
            return;
        }
        
        // Button 2: [ COLOR ] -> X: 15..155, Y: 248..280
        if (x >= 15 && x <= 155 && y >= 248 && y <= 280) {
            currentTheme = (Theme)((currentTheme + 1) % THEME_COUNT);
            redrawUI();
            delay(200);
            return;
        }
        
        // Button 3: [ REBOOT ] -> X: 165..305, Y: 248..280
        if (x >= 165 && x <= 305 && y >= 248 && y <= 280) {
            currentViewState = VIEW_REBOOT_CONFIRM;
            redrawUI();
            delay(200);
            return;
        }
    }
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    
    // Give CDC JTAG a small delay to connect
    delay(1000);

    Serial.println();
    Serial.println("╔══════════════════════════════════════╗");
    Serial.println("║    JC3248W535 Smart SIP Switchboard  ║");
    Serial.println("║        Retro-Console Edition         ║");
    Serial.println("╚══════════════════════════════════════╝");
    Serial.println();

    // Initialize Backlight pins (Pin 1 and Pin 19 for different board revisions)
    pinMode(1, OUTPUT);
    digitalWrite(1, HIGH);
    pinMode(19, OUTPUT);
    digitalWrite(19, HIGH);

    // Print PSRAM diagnostics
    Serial.printf("[SYSTEM] PSRAM found: %s\n", psramFound() ? "YES" : "NO");
    Serial.printf("[SYSTEM] Free PSRAM: %d KB\n", ESP.getFreePsram() / 1024);
    Serial.printf("[SYSTEM] Free Heap: %d KB\n", ESP.getFreeHeap() / 1024);

    // ── 1. Initialise the JC3248W535 Smart Display ──────────────────────────
    Serial.println("[GFX] Instantiating screen in safe setup context...");
    screenPtr = new JC3248W535EN();

    Serial.println("[GFX] Initialising screen...");
    displayActive = screen.begin();
    if (!displayActive) {
        Serial.println("[GFX] WARNING: Smart display initialization failed! Proceeding with headless operation.");
        // Re-force backlight HIGH in case library cleared it
        pinMode(1, OUTPUT);
        digitalWrite(1, HIGH);
        pinMode(19, OUTPUT);
        digitalWrite(19, HIGH);
    }
    
    // Initialize circular logs buffer
    for (int i = 0; i < MAX_LOG_LINES; i++) {
        logHistory[i] = "";
    }
    
    currentViewState = VIEW_DASHBOARD;
    if (displayActive) {
        redrawUI();
    }
    
    addLogLine("Display Initialized.");
    addLogLine("AXS15231B LCD Active.");
    addLogLine("CST816S Touch Polling.");
    
    // ── 2. Start Wi-Fi Access Point ─────────────────────────────────────────
    addLogLine("Starting WiFi SoftAP...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CLIENTS);

    delay(500);

    IPAddress apIP = WiFi.softAPIP();
    char wifiLog[64];
    sprintf(wifiLog, "AP Ready. IP: %s", apIP.toString().c_str());
    addLogLine(wifiLog);
    
    // ── 3. Start SIP PBX Server ─────────────────────────────────────────────
    sprintf(wifiLog, "Binding SIP to %s:%d...", SIP_BIND_IP, SIP_PORT);
    addLogLine(wifiLog);

    try {
        server = new SipServer(std::string(SIP_BIND_IP), SIP_PORT);
        addLogLine("SIP PBX Server is RUNNING.");
    } catch (const std::exception& e) {
        addLogLine("SIP ERROR: Failed to bind.");
        while (true) { delay(1000); }
    }

    // ── 4. Start HTTP Web Dashboard ─────────────────────────────────────────
    addLogLine("Starting Web server...");
    try {
        httpServer = new HttpServer(std::string(SIP_BIND_IP), HTTP_PORT, server->getHandler());
        httpServer->start();
        addLogLine("Web Dashboard is RUNNING.");
    } catch (const std::exception& e) {
        addLogLine("Web server failed to start.");
    }

    addLogLine("System ready for calls.");
    Serial.println("────────────────────────────────────────────────────────────────────");
}

// ── Main Loop ──────────────────────────────────────────────────────────────
void loop()
{
    // 1. Tick SIP Server logic
    if (server) {
        server->getHandler().tick();
    }

    // 2. Poll Capacitive Touch Screen
    uint16_t touchX = 0, touchY = 0;
    if (screen.getTouchPoint(touchX, touchY)) {
        // Touch detected
        handleTouch(touchX, touchY);
    }

    // 3. Status updates, change detection, and periodic logs
    unsigned long now = millis();
    
    // Check Wi-Fi Client connections
    int currentStations = WiFi.softAPgetStationNum();
    if (currentStations != prevStations) {
        if (prevStations != -1) {
            if (currentStations > prevStations) {
                addLogLine("Station connected! Total: " + String(currentStations));
            } else {
                addLogLine("Station disconnected! Total: " + String(currentStations));
            }
        }
        prevStations = currentStations;
    }

    // Check Registered SIP extensions
        int currentExtensions = server->getHandler().getClientCount();
        if (currentExtensions != prevExtensions) {
            if (prevExtensions != -1) {
                if (currentExtensions > prevExtensions) {
                    addLogLine("SIP extension registered!");
                } else {
                    addLogLine("SIP extension de-registered!");
                }
            }
            prevExtensions = currentExtensions;
        }

        // Check active call sessions
        int currentSessions = server->getHandler().getSessionCount();
        if (currentSessions != prevSessions) {
            if (prevSessions != -1) {
                if (currentSessions > prevSessions) {
                    addLogLine("VoIP Session established!");
                } else {
                    addLogLine("VoIP Session terminated.");
                }
            }
            prevSessions = currentSessions;
        }

    // Dynamic partial screen updates for status values
    if (currentViewState == VIEW_DASHBOARD) {
        const Palette& p = PALETTES[currentTheme];
        
        // Update Uptime every second
        unsigned long uptimeSec = now / 1000;
        if (uptimeSec != prevUptimeSec) {
            prevUptimeSec = uptimeSec;
            
            // Clear only the values column area
            screen.setColor(p.bg.r, p.bg.g, p.bg.b);
            screen.drawFillRect(110, 105, 185, 80); // Clear values area
            
            screen.setColor(p.text.r, p.text.g, p.text.b);
            char buf[64];
            
            // Redraw Uptime
            unsigned long hrs = uptimeSec / 3600;
            unsigned long mins = (uptimeSec % 3600) / 60;
            unsigned long secs = uptimeSec % 60;
            sprintf(buf, "%02lu:%02lu:%02lu", hrs, mins, secs);
            screen.prt(buf, 110, 112, 1);
            
            // Redraw Stations
            sprintf(buf, "%d connected", currentStations);
            screen.prt(buf, 110, 132, 1);
            
            // Redraw Extensions
            int clients = server ? server->getHandler().getClientCount() : 0;
            sprintf(buf, "%d active", clients);
            screen.prt(buf, 110, 152, 1);
            
            // Redraw Sessions
            int sessions = server ? server->getHandler().getSessionCount() : 0;
            sprintf(buf, "%d active", sessions);
            screen.prt(buf, 110, 172, 1);
        }
    }

    // Yield control / Small loop delay
    delay(30);
}
