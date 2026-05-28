/*
 * SipServer - Arduino Sketch for ESP32-S3
 *
 * Boots a Wi-Fi SoftAP ("esp32-sipserver", open, no password)
 * and starts the SIP registrar/proxy on 192.168.4.1:5060.
 * Also starts a retro CGA CRT web dashboard on port 80.
 *
 * Board:  ESP32-S3 Dev Module (or any LilyGO ESP32-S3 variant)
 * IDE:    Arduino IDE 2.x  /  PlatformIO
 *
 * Required Arduino libraries:  (none — uses built-in WiFi)
 *
 * IMPORTANT: In the Arduino IDE, go to
 *   Sketch → Add File…  and add every .cpp from src/Helpers/ and src/SIP/
 *   OR use the "src/" folder convention described in the README.
 */

#include <WiFi.h>
#include "src/SIP/SipServer.hpp"
#include "src/Helpers/HttpServer.hpp"

// ── Wi-Fi SoftAP Configuration ──────────────────────────────────────────────
static constexpr const char* AP_SSID        = "esp32-sipserver";
static constexpr const char* AP_PASSWORD    = "";        // open network
static constexpr int         AP_CHANNEL     = 1;
static constexpr int         AP_MAX_CLIENTS = 10;

// ── SIP Server Configuration ────────────────────────────────────────────────
static constexpr const char* SIP_BIND_IP    = "192.168.4.1";
static constexpr int         SIP_PORT       = 5060;

// ── HTTP Dashboard Configuration ────────────────────────────────────────────
static constexpr int         HTTP_PORT      = 80;

// ── Globals ─────────────────────────────────────────────────────────────────
static SipServer*  server     = nullptr;
static HttpServer* httpServer = nullptr;

void setup()
{
    Serial.begin(115200);
    while (!Serial) { delay(10); }

    Serial.println();
    Serial.println("╔══════════════════════════════════════╗");
    Serial.println("║       ESP32-S3  SIP Server           ║");
    Serial.println("║         Retro-Console Edition        ║");
    Serial.println("╚══════════════════════════════════════╝");
    Serial.println();

    // ── Start Wi-Fi Access Point ────────────────────────────────────────────
    Serial.printf("[WiFi] Starting SoftAP  SSID: %s  Channel: %d\n", AP_SSID, AP_CHANNEL);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CLIENTS);

    // Give the DHCP server a moment to settle
    delay(500);

    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("[WiFi] AP ready.  Gateway IP: %s\n", apIP.toString().c_str());
    Serial.printf("[WiFi] Max stations: %d\n", AP_MAX_CLIENTS);
    Serial.println();

    // ── Start SIP Server ────────────────────────────────────────────────────
    Serial.printf("[SIP]  Binding to %s:%d ...\n", SIP_BIND_IP, SIP_PORT);

    try {
        server = new SipServer(std::string(SIP_BIND_IP), SIP_PORT);
        Serial.println("[SIP]  Server is RUNNING.");
    } catch (const std::exception& e) {
        Serial.printf("[SIP]  FATAL: %s\n", e.what());
        Serial.println("[SIP]  Server failed to start. Halting.");
        while (true) { delay(1000); }
    }

    // ── Start HTTP Dashboard ────────────────────────────────────────────────
    Serial.printf("[HTTP] Starting CGA CRT Dashboard on port %d ...\n", HTTP_PORT);

    try {
        httpServer = new HttpServer(std::string(SIP_BIND_IP), HTTP_PORT, server->getHandler());
        httpServer->start();
        Serial.printf("[HTTP] Dashboard RUNNING at http://%s:%d/\n", SIP_BIND_IP, HTTP_PORT);
    } catch (const std::exception& e) {
        Serial.printf("[HTTP] FATAL: %s\n", e.what());
        Serial.println("[HTTP] Dashboard failed to start.");
    }

    Serial.println();
    Serial.printf("Ready. Connect to Wi-Fi and open http://%s/ for the retro dashboard\n", SIP_BIND_IP);
    Serial.println("────────────────────────────────────────────────────────────────────");
}

void loop()
{
    // Print station count every 30 seconds for visibility
    static unsigned long lastPrint = 0;
    unsigned long now = millis();

    if (now - lastPrint >= 30000) {
        lastPrint = now;
        Serial.printf("[WiFi] Connected stations: %d\n", WiFi.softAPgetStationNum());
    }

    delay(100);
}
