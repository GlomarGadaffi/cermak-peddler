/*
 * SipServerETH - Arduino Sketch for ESP32-S3 + W5500 Ethernet (PoE)
 *
 * Targets: Waveshare ESP32-S3-ETH + PoE module
 *          (ESP32-S3R8 + W5500 via SPI)
 *
 * Initialises the W5500 wired Ethernet interface, obtains an IP via
 * DHCP (or uses a static fallback), then starts the SIP registrar/proxy
 * bound to that address on port 5060.
 *
 * Board:  ESP32-S3 Dev Module  (Arduino IDE 2.x / ESP32 Arduino Core ≥ 3.0)
 * Requires: ESP32 Arduino Core 3.x  (ships with native W5500 ETH support)
 *
 * IMPORTANT – Pin mapping:
 *   The SPI pins below match the Waveshare ESP32-S3-ETH schematic.
 *   Verify against YOUR board revision before flashing.
 *   Waveshare wiki: https://www.waveshare.com/wiki/ESP32-S3-ETH
 *
 * IMPORTANT – Arduino IDE:
 *   Sketch → Add File…  and add every .cpp from src/Helpers/ and src/SIP/
 *   OR use the "src/" folder convention described in the README.
 */

#include <SPI.h>
#include <ETH.h>
#include "src/SIP/SipServer.hpp"

// ── W5500 SPI Pin Mapping (Waveshare ESP32-S3-ETH) ────────────────────────
//    Verify these against your board's schematic / silkscreen.
static constexpr int W5500_SCLK =  12;
static constexpr int W5500_MISO =  13;
static constexpr int W5500_MOSI =  11;
static constexpr int W5500_CS   =  10;
static constexpr int W5500_INT  =  14;
static constexpr int W5500_RST  =  -1;   // -1 if not wired to a GPIO

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
static SipServer* server = nullptr;
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
    Serial.println("║   ESP32-S3  SIP Server  (W5500 / PoE)   ║");
    Serial.println("║         Retro-Console Edition        ║");
    Serial.println("╚══════════════════════════════════════════╝");
    Serial.println();

    // ── Register Ethernet events ────────────────────────────────────────
    Network.onEvent(onEthEvent);

    // ── Initialise W5500 over SPI ───────────────────────────────────────
    Serial.println("[ETH] Initialising W5500 ...");

    SPI.begin(W5500_SCLK, W5500_MISO, W5500_MOSI, W5500_CS);

    if (!ETH.begin(ETH_PHY_W5500,   // PHY type
                   1,                // PHY address (W5500 default)
                   W5500_CS,
                   W5500_INT,
                   W5500_RST,
                   SPI,
                   ETH_PHY_SPI_FREQ_MHZ))
    {
        Serial.println("[ETH] FATAL: ETH.begin() failed. Check wiring / pin config.");
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

    Serial.println();
    Serial.printf("Ready.  Point softphones at %s:%d\n", bindIP.c_str(), SIP_PORT);
    Serial.println("──────────────────────────────────────────────────────────────");
}

// ── Loop ───────────────────────────────────────────────────────────────────
void loop()
{
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
