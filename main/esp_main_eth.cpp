/*
 * esp_main_eth.cpp — ESP-IDF entry point for SipServer over W5500 Ethernet
 *
 * Targets: Waveshare ESP32-S3-ETH + PoE module
 *          (ESP32-S3R8 + W5500 via SPI, 10/100 Mbps)
 *
 * Initialises the W5500 Ethernet MAC/PHY through the esp_eth driver,
 * waits for a DHCP lease (or applies a static IP), then launches the
 * SIP registrar/proxy on the assigned address.
 *
 * Build:   idf.py set-target esp32s3 && idf.py build
 *
 * Pin mapping matches the Waveshare ESP32-S3-ETH schematic.
 * Verify against YOUR board revision before flashing.
 */

#include <cstring>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "SipServer.hpp"
#include "HttpServer.hpp"

// ── Tag for ESP_LOG ────────────────────────────────────────────────────────
static const char* TAG = "SipServerETH";

// ── W5500 SPI Pin Mapping (Waveshare ESP32-S3-ETH) ────────────────────────
//    Verify these against your board's schematic / silkscreen.
#define W5500_SCLK_GPIO   12
#define W5500_MISO_GPIO   13
#define W5500_MOSI_GPIO   11
#define W5500_CS_GPIO     10
#define W5500_INT_GPIO    14
#define W5500_RST_GPIO    -1    // -1 if not wired to a GPIO

#define W5500_SPI_HOST    SPI2_HOST
#define W5500_SPI_CLOCK   36    // MHz — W5500 supports up to 80 MHz

// ── Static IP fallback (set USE_STATIC_IP to 1 to skip DHCP) ──────────────
#define USE_STATIC_IP     0
#define STATIC_IP         "192.168.1.200"
#define STATIC_GATEWAY    "192.168.1.1"
#define STATIC_NETMASK    "255.255.255.0"

// ── SIP configuration ─────────────────────────────────────────────────────
#define SIP_PORT          5060

// ── Event group bits ──────────────────────────────────────────────────────
#define ETH_CONNECTED_BIT BIT0
#define ETH_GOT_IP_BIT    BIT1

static EventGroupHandle_t s_eth_event_group = nullptr;
static esp_netif_t*       s_eth_netif       = nullptr;
static std::string        s_ip_addr;

// Global SIP server pointer so the HTTP dashboard task can reach the handler
static SipServer*         g_sipServer = nullptr;

#define HTTP_DASHBOARD_PORT 80

// ── Event handlers ────────────────────────────────────────────────────────

static void eth_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    switch (event_id)
    {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet link UP");
            xEventGroupSetBits(s_eth_event_group, ETH_CONNECTED_BIT);
            break;

        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Ethernet link DOWN");
            xEventGroupClearBits(s_eth_event_group, ETH_CONNECTED_BIT | ETH_GOT_IP_BIT);
            break;

        default:
            break;
    }
}

static void ip_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    if (event_id == IP_EVENT_ETH_GOT_IP)
    {
        ip_event_got_ip_t* event = static_cast<ip_event_got_ip_t*>(event_data);
        char buf[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, buf, sizeof(buf));
        s_ip_addr = buf;

        ESP_LOGI(TAG, "IP:      %s", buf);
        esp_ip4addr_ntoa(&event->ip_info.gw, buf, sizeof(buf));
        ESP_LOGI(TAG, "Gateway: %s", buf);
        esp_ip4addr_ntoa(&event->ip_info.netmask, buf, sizeof(buf));
        ESP_LOGI(TAG, "Netmask: %s", buf);

        xEventGroupSetBits(s_eth_event_group, ETH_GOT_IP_BIT);
    }
}

// ── Ethernet initialisation ───────────────────────────────────────────────

static esp_eth_handle_t eth_init_w5500(void)
{
    // ── SPI bus ─────────────────────────────────────────────────────────
    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num   = W5500_MISO_GPIO;
    buscfg.mosi_io_num   = W5500_MOSI_GPIO;
    buscfg.sclk_io_num   = W5500_SCLK_GPIO;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    ESP_ERROR_CHECK(spi_bus_initialize(W5500_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // ── SPI device for W5500 ────────────────────────────────────────────
    spi_device_interface_config_t devcfg = {};
    devcfg.command_bits   = 16;   // W5500 uses 16-bit address phase
    devcfg.address_bits   = 8;    // 8-bit control phase
    devcfg.mode           = 0;
    devcfg.clock_speed_hz = W5500_SPI_CLOCK * 1000 * 1000;
    devcfg.spics_io_num   = W5500_CS_GPIO;
    devcfg.queue_size     = 20;

    spi_device_handle_t spi_handle = nullptr;
    ESP_ERROR_CHECK(spi_bus_add_device(W5500_SPI_HOST, &devcfg, &spi_handle));

    // ── MAC config ──────────────────────────────────────────────────────
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(spi_handle);
    w5500_config.int_gpio_num = static_cast<gpio_num_t>(W5500_INT_GPIO);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);

    // ── PHY config ──────────────────────────────────────────────────────
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.autonego_timeout_ms = 0; // W5500 has internal PHY
    phy_config.reset_gpio_num      = static_cast<gpio_num_t>(W5500_RST_GPIO);
    esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&phy_config);

    // ── Install driver ──────────────────────────────────────────────────
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = nullptr;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    // ── Set MAC address from ESP32 efuse (base + 1) ─────────────────────
    uint8_t mac_addr[6];
    esp_read_mac(mac_addr, ESP_MAC_ETH);
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));

    return eth_handle;
}

// ── SIP server task ───────────────────────────────────────────────────────

static void sip_server_task(void* pvParameters)
{
    ESP_LOGI(TAG, "Starting SipServer on %s:%d", s_ip_addr.c_str(), SIP_PORT);

    g_sipServer = new SipServer(s_ip_addr, SIP_PORT);
    ESP_LOGI(TAG, "SIP server is RUNNING.  Point softphones at %s:%d",
             s_ip_addr.c_str(), SIP_PORT);

    unsigned long lastHeartbeat = 0;
    while (true)
    {
        if (g_sipServer) {
            g_sipServer->getHandler().tick();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        unsigned long nowSec = (unsigned long)(esp_timer_get_time() / 1000000);
        if (nowSec - lastHeartbeat >= 30)
        {
            lastHeartbeat = nowSec;
            ESP_LOGI(TAG, "Heartbeat — IP: %s  Uptime: %lus",
                     s_ip_addr.c_str(), nowSec);
        }
    }

    vTaskDelete(nullptr);
}

static void http_server_task(void* pvParameters)
{
    // Wait for SIP server to initialize
    while (g_sipServer == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Starting CGA CRT Dashboard on %s:%d", s_ip_addr.c_str(), HTTP_DASHBOARD_PORT);

    HttpServer http(s_ip_addr, HTTP_DASHBOARD_PORT, &g_sipServer->getHandler());
    http.start();
    ESP_LOGI(TAG, "CGA CRT Dashboard RUNNING at http://%s:%d/",
             s_ip_addr.c_str(), HTTP_DASHBOARD_PORT);

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    vTaskDelete(nullptr);
}

// ── app_main ──────────────────────────────────────────────────────────────

extern "C" void app_main(void)
{
    // ── NVS (required by some drivers) ──────────────────────────────────
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ── Networking stack ────────────────────────────────────────────────
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_eth_event_group = xEventGroupCreate();

    // ── Register event handlers ─────────────────────────────────────────
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               &eth_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               &ip_event_handler, nullptr));

    // ── Create default netif for Ethernet ───────────────────────────────
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);

    // ── Initialise W5500 ────────────────────────────────────────────────
    esp_eth_handle_t eth_handle = eth_init_w5500();

    // ── Glue driver to netif ────────────────────────────────────────────
    esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(eth_handle));

    // ── Static IP (optional) ────────────────────────────────────────────
#if USE_STATIC_IP
    esp_netif_dhcpc_stop(s_eth_netif);
    esp_netif_ip_info_t ip_info = {};
    esp_netif_str_to_ip4(STATIC_IP,      &ip_info.ip);
    esp_netif_str_to_ip4(STATIC_GATEWAY,  &ip_info.gw);
    esp_netif_str_to_ip4(STATIC_NETMASK,  &ip_info.netmask);
    esp_netif_set_ip_info(s_eth_netif, &ip_info);
    ESP_LOGI(TAG, "Static IP configured: %s", STATIC_IP);
#endif

    // ── Start Ethernet ──────────────────────────────────────────────────
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    ESP_LOGI(TAG, "Waiting for Ethernet link + IP ...");

    // ── Wait for IP ─────────────────────────────────────────────────────
    EventBits_t bits = xEventGroupWaitBits(s_eth_event_group,
                                           ETH_GOT_IP_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(30000));
    if (!(bits & ETH_GOT_IP_BIT))
    {
        ESP_LOGE(TAG, "FATAL: No IP after 30 s.  Check cable / PoE / DHCP.");
        return;
    }

    // ── Launch SIP server on Core 1 ─────────────────────────────────────
    xTaskCreatePinnedToCore(&sip_server_task, "sip_server", 8192, nullptr, 5, nullptr, 1);

    // ── Launch HTTP dashboard on Core 0 ─────────────────────────────────
    xTaskCreatePinnedToCore(&http_server_task, "http_dashboard", 8192, nullptr, 4, nullptr, 0);
}
