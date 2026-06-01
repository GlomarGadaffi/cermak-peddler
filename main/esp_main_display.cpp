#include <cstring>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c.h"

// standard Espressif esp_lcd panel/touch drivers
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"

// LVGL & UI Layer
#include "lvgl.h"
#include "ui.h"

// SIP & Servers
#include "SipServer.hpp"
#include "HttpServer.hpp"
#include "DnsServer.hpp"
#include "IPHelper.hpp"

static const char *TAG = "main_display";

// Board Pin mappings (Guition cheap black display JC3248W535)
#define TFT_CS      45
#define TFT_SCK     47
#define TFT_D0      21
#define TFT_D1      48
#define TFT_D2      40
#define TFT_D3      39
#define TFT_BL      1

#define TOUCH_SDA   4
#define TOUCH_SCL   8
#define TOUCH_RST   12
#define TOUCH_INT   11

#define BATTERY_ADC_PIN 5

// Onboarding credentials
#define ONBOARDING_SSID "My-Ap"
#define ONBOARDING_PASS "12345678"

// Active server objects
static SipServer* g_sipServer = nullptr;
static HttpServer* g_httpServer = nullptr;
static DnsServer* g_dnsServer = nullptr;

static std::string g_localIp = "192.168.4.1";
static int g_stationNum = 0;
static bool g_setupComplete = false;

// Task scheduler loop counters
static int prevStations = -1;
static int prevExtensions = -1;
static int prevSessions = -1;
static float batteryVolts = 0.0f;
static int batteryPercent = -1;

// Forward event declarations
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

// ─── FreeRTOS background log interceptor ───
// We redirect ESP_LOGX outputs directly to the on-screen scroll area in addition to Serial.
extern "C" {
    vprintf_like_t original_log_vprintf = NULL;
    int screen_log_vprintf(const char *fmt, va_list tag) {
        char buf[256];
        int len = vsnprintf(buf, sizeof(buf) - 1, fmt, tag);
        if (len > 0) {
            // Trim carriage returns and newlines
            while(len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
                buf[len-1] = '\0';
                len--;
            }
            if (len > 0) {
                ui_add_log(buf);
            }
        }
        if (original_log_vprintf) {
            return original_log_vprintf(fmt, tag);
        }
        return len;
    }
}

// ─── Low Level Hardware Panel & Touch init ───
static void hardware_display_init(lv_disp_drv_t* disp_drv) {
    ESP_LOGI(TAG, "Initializing AXS15231B Backlight...");
    gpio_config_t bk_gpio_config = {};
    bk_gpio_config.mode = GPIO_MODE_OUTPUT;
    bk_gpio_config.pin_bit_mask = 1ULL << TFT_BL;
    gpio_config(&bk_gpio_config);
    gpio_set_level((gpio_num_t)TFT_BL, 1); // Backlight HIGH

    ESP_LOGI(TAG, "Initializing LCD QSPI Panel Bus...");
    esp_lcd_panel_bus_handle_t qspi_bus = NULL;
    esp_lcd_spi_bus_config_t bus_config = AXS15231B_PANEL_BUS_QSPI_CONFIG(
        TFT_SCK, TFT_D0, TFT_D1, TFT_D2, TFT_D3, 320 * 480 * sizeof(uint16_t)
    );
    ESP_ERROR_CHECK(esp_lcd_new_spi_bus((spi_host_device_t)SPI2_HOST, &bus_config, &qspi_bus));

    ESP_LOGI(TAG, "Initializing Panel IO handle...");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = AXS15231B_PANEL_IO_QSPI_CONFIG(
        TFT_CS, 
        [](esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) -> bool {
            lv_disp_drv_t *drv = (lv_disp_drv_t *)user_ctx;
            lv_disp_flush_ready(drv);
            return false;
        }, 
        disp_drv
    );
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(qspi_bus, &io_config, &io_handle));

    ESP_LOGI(TAG, "Instantiating axs15231b panel driver...");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_dev_config = {};
    panel_dev_config.reset_gpio_num = -1; // Reset pin is not defined
    panel_dev_config.rgb_endian = LCD_RGB_ENDIAN_BGR;
    panel_dev_config.bits_per_pixel = 16;
    
    axs15231b_vendor_config_t vendor_config = {};
    vendor_config.flags.use_qspi_interface = 1;
    panel_dev_config.vendor_config = &vendor_config;
    
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(io_handle, &panel_dev_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    disp_drv->user_data = panel_handle;
}

static esp_lcd_touch_handle_t hardware_touch_init() {
    ESP_LOGI(TAG, "Initializing Touch I2C Bus...");
    i2c_config_t i2c_conf = {};
    i2c_conf.mode = I2C_MODE_MASTER;
    i2c_conf.sda_io_num = (gpio_num_t)TOUCH_SDA;
    i2c_conf.scl_io_num = (gpio_num_t)TOUCH_SCL;
    i2c_conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_conf.master.clk_speed = 400000;
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0));

    esp_lcd_panel_io_handle_t touch_io = NULL;
    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(I2C_NUM_0, &touch_io_config, &touch_io));

    ESP_LOGI(TAG, "Creating axs15231b touch driver...");
    esp_lcd_touch_handle_t touch_handle = NULL;
    esp_lcd_touch_config_t touch_config = {};
    touch_config.x_max = 320;
    touch_config.y_max = 480;
    touch_config.rst_gpio_num = (gpio_num_t)TOUCH_RST;
    touch_config.int_gpio_num = (gpio_num_t)TOUCH_INT;
    touch_config.flags.swap_xy = 0;
    touch_config.flags.mirror_x = 0;
    touch_config.flags.mirror_y = 0;
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_axs15231b(touch_io, &touch_config, &touch_handle));

    return touch_handle;
}

// ─── LVGL Tick & Execution loops ───
static void lvgl_task(void *pvParameters) {
    ESP_LOGI("LVTask", "Core 1 Graphics task running...");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        lv_timer_handler();
    }
}

// ─── WiFi Bringup logic ───
static void wifi_init_softap(const char* ssid, const char* pass, bool open_network) {
    ESP_LOGI(TAG, "Starting SoftAP Mode SSID:%s", ssid);
    
    esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.ap.ssid, ssid);
    wifi_config.ap.ssid_len = strlen(ssid);
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 10;
    if (open_network) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        wifi_config.ap.password[0] = '\0';
    } else {
        wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
        strcpy((char*)wifi_config.ap.password, pass);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static bool wifi_init_sta(const char* ssid, const char* pass) {
    ESP_LOGI(TAG, "Attempting Wi-Fi Station connection to SSID: %s", ssid);
    
    esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, ssid);
    strcpy((char*)wifi_config.sta.password, pass);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Awaiting local network association...");
    esp_err_t connect_err = esp_wifi_connect();
    if (connect_err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi Connect failed!");
        return false;
    }

    // Wait and check connection status up to 5 seconds
    int retries = 0;
    while (retries < 5) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            char ip_str[32];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            g_localIp = ip_str;
            ESP_LOGI(TAG, "Connected successfully! Local IP is: %s", g_localIp.c_str());
            return true;
        }
        retries++;
        ESP_LOGI(TAG, "WiFi retry count: %d...", retries);
    }
    
    return false;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*)event_data;
            g_stationNum++;
            ESP_LOGI(TAG, "station " MACSTR " joined local AP, AID=%d", MAC2STR(event->mac), event->aid);
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*)event_data;
            if (g_stationNum > 0) g_stationNum--;
            ESP_LOGI(TAG, "station " MACSTR " left local AP, AID=%d", MAC2STR(event->mac), event->aid);
        }
    }
}

// ─── Server Tasks on Core 0 ───
static void sip_server_task(void *pvParameters) {
    ESP_LOGI("SipTask", "Starting SipServer engine on Core 0 IP %s:%d", g_localIp.c_str(), 5060);
    g_sipServer = new SipServer(g_localIp, 5060);
    while (1) {
        if (g_sipServer) {
            g_sipServer->getHandler().tick();
        }
        vTaskDelay(pdMS_TO_TICKS(30)); // match Arduino 30ms latency cycle
    }
    vTaskDelete(NULL);
}

static void http_server_task(void *pvParameters) {
    ESP_LOGI("HttpTask", "Starting Web CGA CRT dashboard on Core 0 IP %s:80", g_localIp.c_str());
    // block task until SipServer is fully instanced
    while (g_sipServer == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    g_httpServer = new HttpServer(g_localIp, 80, g_sipServer->getHandler());
    g_httpServer->start();
    ESP_LOGI("HttpTask", "CGA Web UI Running successfully!");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}

// ─── Periodic system update task ───
static void system_status_task(void *pvParameters) {
    uint32_t ticks = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        ticks++;

        // Read battery voltage from Divider GPIO 5 (Simulated or ADC read)
        batteryVolts = 3.7f + ((float)(rand() % 40) / 100.0f); // Simulated 3.7V - 4.1V
        batteryPercent = (int)((batteryVolts - 3.3f) / (4.2f - 3.3f) * 100.0f);
        ui_set_battery(batteryVolts, batteryPercent);

        if (g_setupComplete) {
            uint32_t uptime = esp_timer_get_time() / 1000000;
            
            // Query extensions and active VoIP call counts
            int clientCount = g_sipServer ? g_sipServer->getHandler().getClientCount() : 0;
            int sessionCount = g_sipServer ? g_sipServer->getHandler().getSessionCount() : 0;

            ui_update_status(g_localIp, uptime, g_stationNum, clientCount, sessionCount);

            // Log changes to UI
            if (g_stationNum != prevStations) {
                if (prevStations != -1) {
                    ESP_LOGI(TAG, "Stations change detected: %d", g_stationNum);
                }
                prevStations = g_stationNum;
            }
            if (clientCount != prevExtensions) {
                if (prevExtensions != -1) {
                    ESP_LOGI(TAG, "Active SIP extensions changed: %d", clientCount);
                }
                prevExtensions = clientCount;
            }
            if (sessionCount != prevSessions) {
                if (prevSessions != -1) {
                    ESP_LOGI(TAG, "Active sessions changed: %d", sessionCount);
                }
                prevSessions = sessionCount;
            }
        }
    }
}

// ─── Entry main function ───
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " POCKET-DIAL ESP-IDF DISPLAY CONTROLLER");
    ESP_LOGI(TAG, "================================================");

    // Initialise Kconfig systems
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Register Wi-Fi events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    // 1. Start LCD display graphics engine registers
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    
    // Low level Panel configuration CS=45, Backlight=1, QSPI lines
    hardware_display_init(&disp_drv);
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)disp_drv.user_data;

    // Allocate frame buffers inside PSRAM (320 * 480 * 2 bytes = 307.2 KB each)
    size_t draw_buf_sz = 320 * 480;
    size_t bytes_to_alloc = draw_buf_sz * sizeof(uint16_t);
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(bytes_to_alloc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(bytes_to_alloc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(buf1 != NULL);
    assert(buf2 != NULL);

    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, draw_buf_sz);
    
    disp_drv.hor_res = 320;
    disp_drv.ver_res = 480;
    disp_drv.flush_cb = [](lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
        esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
        esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
    };
    disp_drv.draw_buf = &disp_buf;
    lv_disp_drv_register(&disp_drv);

    // 2. Initialize touchscreen registers (SDA 4, SCL 8, INT 11, RST 12)
    esp_lcd_touch_handle_t touch_handle = hardware_touch_init();
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.user_data = touch_handle;
    indev_drv.read_cb = [](lv_indev_drv_t *drv, lv_indev_data_t *data) {
        esp_lcd_touch_handle_t touch = (esp_lcd_touch_handle_t)drv->user_data;
        uint16_t touchX[1];
        uint16_t touchY[1];
        uint16_t touchStrength[1];
        uint8_t touchPoints = 0;
        
        esp_lcd_touch_read_data(touch);
        bool pressed = esp_lcd_touch_get_coordinates(touch, touchX, touchY, touchStrength, &touchPoints, 1);
        
        if (pressed && touchPoints > 0) {
            data->point.x = touchX[0];
            data->point.y = touchY[0];
            data->state = LV_INDEV_STATE_PR;
            ui_handle_touch_press(touchX[0], touchY[0]);
        } else {
            data->state = LV_INDEV_STATE_REL;
        }
    };
    lv_indev_drv_register(&indev_drv);

    // 3. Start Core 1 graphics loop
    xTaskCreatePinnedToCore(&lvgl_task, "lvgl_task", 8192, NULL, 5, NULL, 1);
    
    // Draw initial empty frame
    ui_init();

    // Intercept ESP_LOGI and route logs to console log textarea
    original_log_vprintf = esp_log_set_vprintf(screen_log_vprintf);

    // 4. Retrieve saved operational Wi-Fi configuration from NVS
    uint8_t wifi_mode = 0; // 0 = unconfigured fallback, 1 = STATION, 2 = AP Standalone
    char saved_ssid[33] = {0};
    char saved_pass[64] = {0};

    nvs_handle_t nvs_handle;
    if (nvs_open("storage", NVS_READONLY, &nvs_handle) == ESP_OK) {
        nvs_get_u8(nvs_handle, "wifi_mode", &wifi_mode);
        size_t size = sizeof(saved_ssid);
        nvs_get_str(nvs_handle, "wifi_ssid", saved_ssid, &size);
        size = sizeof(saved_pass);
        nvs_get_str(nvs_handle, "wifi_pass", saved_pass, &size);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Persisted Network settings: Mode=%d, SSID=%s", wifi_mode, saved_ssid);

    // Initialize base WiFi configuration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    if (wifi_mode == 1) { // ─── Wi-Fi Station Mode ───
        ESP_LOGI(TAG, "Configured in STATION mode.");
        bool sta_connected = wifi_init_sta(saved_ssid, saved_pass);
        if (sta_connected) {
            ui_set_onboarding_mode(false);
            
            // SIP on Core 0
            xTaskCreatePinnedToCore(&sip_server_task, "sip_server_task", 8192, NULL, 5, NULL, 0);
            
            // HTTP dashboard on Core 0
            xTaskCreatePinnedToCore(&http_server_task, "http_server_task", 8192, NULL, 4, NULL, 0);
            
            g_setupComplete = true;
        } else {
            ESP_LOGW(TAG, "WiFi station association failed! Falling back to Onboarding Mode...");
            wifi_mode = 0; // trigger onboarding fallback
        }
    } else if (wifi_mode == 2) { // ─── Wi-Fi Standalone AP Mode ───
        ESP_LOGI(TAG, "Configured in Standalone AP Mode.");
        ui_set_onboarding_mode(false);
        g_localIp = "192.168.4.1";
        
        wifi_init_softap("esp32-sipserver", "", true); // open network
        
        // SIP on Core 0
        xTaskCreatePinnedToCore(&sip_server_task, "sip_server_task", 8192, NULL, 5, NULL, 0);
        
        // HTTP dashboard on Core 0
        xTaskCreatePinnedToCore(&http_server_task, "http_server_task", 8192, NULL, 4, NULL, 0);
        
        g_setupComplete = true;
    }

    if (wifi_mode == 0) { // ─── Fallback Wi-Fi Onboarding Setup Mode ───
        ESP_LOGI(TAG, "Initializing Fallback Captive portal Setup AP: SSID=%s, Password=%s", ONBOARDING_SSID, ONBOARDING_PASS);
        ui_set_onboarding_mode(true, ONBOARDING_SSID, ONBOARDING_PASS);
        
        wifi_init_softap(ONBOARDING_SSID, ONBOARDING_PASS, false); // secure onboarding AP
        
        // Start UDP DNS redirection server on Port 53 resolving to 192.168.4.1
        g_dnsServer = new DnsServer();
        g_dnsServer->start("192.168.4.1");
        
        // Start configuration Web portal on Port 80
        g_localIp = "192.168.4.1";
        g_httpServer = new HttpServer(g_localIp, 80, *(RequestsHandler*)nullptr); // pass null since no sip server is active yet
        g_httpServer->start();
        
        ESP_LOGI(TAG, "Captive onboarding interface active. Access http://192.168.4.1/");
    }

    // 5. Spin up periodic status and battery updater task
    xTaskCreatePinnedToCore(&system_status_task, "status_task", 4096, NULL, 3, NULL, 0);
}
