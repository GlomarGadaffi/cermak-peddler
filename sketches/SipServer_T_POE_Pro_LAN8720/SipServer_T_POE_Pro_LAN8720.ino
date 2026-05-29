/*
 * SipServer_T_POE_Pro_LAN8720 - Arduino Sketch for LilyGO T-POE-Pro (LAN8720)
 *
 * Targets: LilyGO T-POE-Pro (ESP32-WROVER-E + LAN8720 RMII Ethernet + RS485 + POE)
 *          Board features: 16MB Flash, 8MB PSRAM, RS485 module, PoE support
 *
 * Initialises the LAN8720 RMII wired Ethernet interface, obtains an IP via
 * DHCP (or uses a static fallback), then starts the SIP registrar/proxy.
 *
 * Board:  ESP32 Dev Module / ESP32 Wrover Module (Arduino IDE 2.x)
 * Requires: ESP32 Arduino Core 3.x (supports native RMII eth)
 *
 * IMPORTANT – Pin mapping (LilyGO T-POE-Pro):
 *   - RESET: GPIO 5 (Active-low PHY Power/Reset)
 *   - MDC:   GPIO 23
 *   - MDIO:  GPIO 18
 *   - CLK:   GPIO 17 OUT (50MHz Reference Clock)
 *   - ADDR:  0 (PHY Address)
 */

#include <ETH.h>
#include "src/SIP/SipServer.hpp"
#include "src/Helpers/HttpServer.hpp"

// ── LAN8720 RMII Pin Mapping (LilyGO T-POE-Pro) ───────────────────────────
static constexpr int ETH_MDC_PIN   = 23;
static constexpr int ETH_MDIO_PIN  = 18;
static constexpr int ETH_POWER_PIN  = 5;   // Active-low RESET pin
static constexpr int ETH_PHY_ADDR   = 0;   // PHY Address (default is 0)
static constexpr eth_clock_mode_t ETH_CLK_MODE = ETH_CLOCK_GPIO17_OUT; 

// ── Static IP fallback (used when DHCP times out) ──────────────────────────
//    Set USE_STATIC_IP to true to skip DHCP entirely.
static constexpr bool USE_STATIC_IP = false;
static constexpr const char* STATIC_IP      = "192.168.1.200";
static constexpr const char* STATIC_GATEWAY = "192.168.1.1";
static constexpr const char* STATIC_SUBNET  = "255.255.255.0";
static constexpr const char* STATIC_DNS     = "8.8.8.8";

// ── SIP Server Configuration ───────────────────────────────────────────────
static constexpr int SIP_PORT = 5060;

// ── DHCP timeout (ms) before falling back to static config ─────────────────
static constexpr unsigned long DHCP_TIMEOUT_MS = 15000;

// ── Globals ────────────────────────────────────────────────────────────────
static constexpr int HTTP_PORT = 80;

static SipServer*  server     = nullptr;
static HttpServer* httpServer = nullptr;
static volatile bool ethConnected  = false;
static volatile bool ethHasIP      = false;

// ── Ethernet event handler ─────────────────────────────────────────────────
void onEthEvent(arduino_event_id_t event)
{
    switch (event)
    {
        case ARDUINO_EVENT_ETH_START:
            Serial.println("[ETH] Started");
            break;

        case ARDUINO_EVENT_ETH_CONNECTED:
            Serial.println("[ETH] Link UP");
            ethConnected = true;
            break;

        case ARDUINO_EVENT_ETH_GOT_IP:
            Serial.printf("[ETH] IP:      %s\n", ETH.localIP().toString().c_str());
            Serial.printf("[ETH] Gateway: %s\n", ETH.gatewayIP().toString().c_str());
            Serial.printf("[ETH] Subnet:  %s\n", ETH.subnetMask().toString().c_str());
            Serial.printf("[ETH] DNS:     %s\n", ETH.dnsIP().toString().c_str());
            Serial.printf("[ETH] Speed:   %d Mbps  %s\n",
                          ETH.linkSpeed(),
                          ETH.fullDuplex() ? "Full Duplex" : "Half Duplex");
            ethHasIP = true;
            break;

        case ARDUINO_EVENT_ETH_DISCONNECTED:
            Serial.println("[ETH] Link DOWN");
            ethConnected = false;
            ethHasIP     = false;
            break;

        default:
            break;
    }
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    while (!Serial) { delay(10); }

    Serial.println();
    Serial.println("╔══════════════════════════════════════════╗");
    Serial.println("║ LILYGO T-POE-Pro LAN8720 SipServer       ║");
    Serial.println("║         Retro-Console Edition        ║");
    Serial.println("╚══════════════════════════════════════════╝");
    Serial.println();

    // ── Register Ethernet events ────────────────────────────────────────
    Network.onEvent(onEthEvent);

    // ── Initialise LAN8720 RMII PHY ─────────────────────────────────────
    Serial.println("[ETH] Initialising LAN8720 over RMII ...");

    // WROVER-E board has native RMII Ethernet hardware MAC built into ESP32 chip
    if (!ETH.begin(ETH_PHY_LAN8720,
                   ETH_PHY_ADDR,
                   ETH_MDC_PIN,
                   ETH_MDIO_PIN,
                   ETH_POWER_PIN,
                   ETH_CLK_MODE))
    {
        Serial.println("[ETH] FATAL: ETH.begin() failed. Check cable / PoE switch.");
        while (true) { delay(1000); }
    }

    // ── Static IP or DHCP ───────────────────────────────────────────────
    if (USE_STATIC_IP)
    {
        IPAddress ip, gw, sn, dns;
        ip.fromString(STATIC_IP);
        gw.fromString(STATIC_GATEWAY);
        sn.fromString(STATIC_SUBNET);
        dns.fromString(STATIC_DNS);
        ETH.config(ip, gw, sn, dns);
        Serial.printf("[ETH] Using static IP: %s\n", STATIC_IP);
    }
    else
    {
        Serial.println("[ETH] Waiting for DHCP ...");
    }

    // ── Wait for an IP address ──────────────────────────────────────────
    unsigned long start = millis();
    while (!ethHasIP)
    {
        delay(100);
        if (millis() - start > DHCP_TIMEOUT_MS)
        {
            Serial.println("[ETH] DHCP timeout — applying static fallback.");
            IPAddress ip, gw, sn, dns;
            ip.fromString(STATIC_IP);
            gw.fromString(STATIC_GATEWAY);
            sn.fromString(STATIC_SUBNET);
            dns.fromString(STATIC_DNS);
            ETH.config(ip, gw, sn, dns);

            // Give the stack a moment after reconfiguration
            unsigned long retry = millis();
            while (!ethHasIP && millis() - retry < 5000) { delay(100); }

            if (!ethHasIP)
            {
                Serial.println("[ETH] FATAL: No IP address. Check cable / PoE.");
                while (true) { delay(1000); }
            }
        }
    }
    Serial.println();

    // ── Start SIP Server ────────────────────────────────────────────────
    String bindIP = ETH.localIP().toString();
    Serial.printf("[SIP] Binding to %s:%d ...\n", bindIP.c_str(), SIP_PORT);

    try
    {
        server = new SipServer(std::string(bindIP.c_str()), SIP_PORT);
        Serial.println("[SIP] Server is RUNNING.");
    }
    catch (const std::exception& e)
    {
        Serial.printf("[SIP] FATAL: %s\n", e.what());
        Serial.println("[SIP] Server failed to start. Halting.");
        while (true) { delay(1000); }
    }

    // ── Start HTTP Dashboard ────────────────────────────────────────────
    try
    {
        httpServer = new HttpServer(std::string(bindIP.c_str()), HTTP_PORT, server->getHandler());
        httpServer->start();
        Serial.printf("[HTTP] Dashboard RUNNING at http://%s:%d/\n", bindIP.c_str(), HTTP_PORT);
    }
    catch (const std::exception& e)
    {
        Serial.printf("[HTTP] FATAL: %s\n", e.what());
    }

    Serial.println();
    Serial.printf("Ready.  Point softphones at %s:%d\n", bindIP.c_str(), SIP_PORT);
    Serial.printf("Dashboard: http://%s:%d/\n", bindIP.c_str(), HTTP_PORT);
    Serial.println("──────────────────────────────────────────────────────────────");
}

// ── Loop ───────────────────────────────────────────────────────────────────
void loop()
{
    if (server)
    {
        server->getHandler().tick();
    }

    // Periodic heartbeat — link status + uptime
    static unsigned long lastPrint = 0;
    unsigned long now = millis();

    if (now - lastPrint >= 30000)
    {
        lastPrint = now;
        if (ethHasIP)
        {
            Serial.printf("[ETH] Link OK  IP: %s  Speed: %d Mbps  Uptime: %lus\n",
                          ETH.localIP().toString().c_str(),
                          ETH.linkSpeed(),
                          now / 1000);
        }
        else
        {
            Serial.println("[ETH] WARNING: No IP — link may be down.");
        }
    }

    delay(100);
}
