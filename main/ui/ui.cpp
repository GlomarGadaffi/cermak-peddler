#include "ui.h"
#include "lvgl.h"
#include "qrcode.h"
#include "esp_log.h"
#include "esp_system.h"
#include <cstdio>
#include <cstring>

static const char* TAG = "ui_cpp";

// ─── Theme Palettes ───
enum Theme {
    THEME_CGA_BLUE = 0,
    THEME_AMBER,
    THEME_GREEN,
    THEME_COUNT
};

struct LVColorTheme {
    lv_color_t bg;
    lv_color_t border;
    lv_color_t text;
    lv_color_t highlight;
    lv_color_t btn_active;
    lv_color_t alert;
};

static const LVColorTheme PALETTES[THEME_COUNT] = {
    // 0: CGA Blue
    {
        LV_COLOR_MAKE(0, 0, 120),     // Deep Blue
        LV_COLOR_MAKE(85, 255, 255),  // Cyan border
        LV_COLOR_MAKE(255, 255, 255), // White text
        LV_COLOR_MAKE(255, 255, 85),  // Yellow highlight
        LV_COLOR_MAKE(85, 255, 85),   // Green button
        LV_COLOR_MAKE(255, 85, 85)    // Red alert
    },
    // 1: Amber CRT
    {
        LV_COLOR_MAKE(0, 0, 0),       // Black
        LV_COLOR_MAKE(255, 110, 0),   // Dark Amber border
        LV_COLOR_MAKE(255, 160, 0),   // Amber text
        LV_COLOR_MAKE(255, 210, 0),   // Light Amber highlight
        LV_COLOR_MAKE(255, 180, 0),   // Amber button
        LV_COLOR_MAKE(255, 50, 0)     // Red-orange alert
    },
    // 2: Green Phosphor
    {
        LV_COLOR_MAKE(0, 0, 0),       // Black
        LV_COLOR_MAKE(0, 180, 0),     // Green border
        LV_COLOR_MAKE(50, 255, 50),   // Bright Green text
        LV_COLOR_MAKE(180, 255, 180), // Pale Green highlight
        LV_COLOR_MAKE(0, 255, 0),     // Green button
        LV_COLOR_MAKE(255, 50, 50)    // Red alert
    }
};

// ─── Globals & UI State ───
static Theme current_theme = THEME_CGA_BLUE;
static bool is_onboarding = false;

// Core UI Objects
static lv_obj_t* main_container = nullptr;
static lv_obj_t* header_bar = nullptr;
static lv_obj_t* title_label = nullptr;
static lv_obj_t* battery_label = nullptr;

static lv_obj_t* status_panel = nullptr;
static lv_obj_t* status_title = nullptr;
static lv_obj_t* status_ip_label = nullptr;
static lv_obj_t* status_uptime_label = nullptr;
static lv_obj_t* status_sta_label = nullptr;
static lv_obj_t* status_ext_label = nullptr;
static lv_obj_t* status_sess_label = nullptr;

static lv_obj_t* buttons_panel = nullptr;
static lv_obj_t* qr_btn = nullptr;
static lv_obj_t* qr_btn_label = nullptr;
static lv_obj_t* color_btn = nullptr;
static lv_obj_t* color_btn_label = nullptr;
static lv_obj_t* reboot_btn = nullptr;
static lv_obj_t* reboot_btn_label = nullptr;

static lv_obj_t* log_panel = nullptr;
static lv_obj_t* log_title = nullptr;
static lv_obj_t* log_textarea = nullptr;

// Overlays / Modals
static lv_obj_t* qr_modal = nullptr;
static lv_obj_t* qr_canvas = nullptr;
static uint8_t* qr_canvas_buffer = nullptr;
static lv_obj_t* qr_close_btn = nullptr;

static lv_obj_t* reboot_modal = nullptr;
static lv_obj_t* reboot_title = nullptr;
static lv_obj_t* reboot_msg = nullptr;
static lv_obj_t* reboot_yes_btn = nullptr;
static lv_obj_t* reboot_no_btn = nullptr;

// Onboarding Objects
static lv_obj_t* onboarding_modal = nullptr;
static lv_obj_t* onboarding_title = nullptr;
static lv_obj_t* onboarding_ssid_label = nullptr;
static lv_obj_t* onboarding_qr_canvas = nullptr;
static uint8_t* onboarding_qr_buffer = nullptr;
static lv_obj_t* onboarding_ap_btn = nullptr;
static lv_obj_t* onboarding_ap_btn_label = nullptr;

// Theme Apply Helper
static void apply_theme() {
    const LVColorTheme& t = PALETTES[current_theme];
    
    // Main background
    lv_obj_set_style_bg_color(main_container, t.bg, LV_PART_MAIN);
    
    // Header
    lv_obj_set_style_bg_color(header_bar, t.border, LV_PART_MAIN);
    lv_obj_set_style_text_color(title_label, t.bg, LV_PART_MAIN);
    lv_obj_set_style_text_color(battery_label, t.bg, LV_PART_MAIN);
    
    // Status Panel
    lv_obj_set_style_border_color(status_panel, t.border, LV_PART_MAIN);
    lv_obj_set_style_bg_color(status_panel, t.bg, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_title, t.highlight, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_ip_label, t.text, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_uptime_label, t.text, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_sta_label, t.text, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_ext_label, t.text, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_sess_label, t.text, LV_PART_MAIN);
    
    // Buttons Panel
    lv_obj_set_style_bg_color(qr_btn, t.btn_active, LV_PART_MAIN);
    lv_obj_set_style_text_color(qr_btn_label, t.bg, LV_PART_MAIN);
    
    lv_obj_set_style_bg_color(color_btn, t.highlight, LV_PART_MAIN);
    lv_obj_set_style_text_color(color_btn_label, t.bg, LV_PART_MAIN);
    
    lv_obj_set_style_bg_color(reboot_btn, t.alert, LV_PART_MAIN);
    lv_obj_set_style_text_color(reboot_btn_label, LV_COLOR_MAKE(255, 255, 255), LV_PART_MAIN);
    
    // Log Panel
    lv_obj_set_style_border_color(log_panel, t.border, LV_PART_MAIN);
    lv_obj_set_style_bg_color(log_panel, t.bg, LV_PART_MAIN);
    lv_obj_set_style_bg_color(log_title, t.border, LV_PART_MAIN);
    lv_obj_set_style_text_color(log_title, t.bg, LV_PART_MAIN);
    lv_obj_set_style_bg_color(log_textarea, t.bg, LV_PART_MAIN);
    lv_obj_set_style_text_color(log_textarea, t.text, LV_PART_MAIN);
    
    // Modals if active
    if (qr_modal) {
        lv_obj_set_style_bg_color(qr_modal, t.bg, LV_PART_MAIN);
        lv_obj_set_style_border_color(qr_modal, t.border, LV_PART_MAIN);
        lv_obj_set_style_bg_color(qr_close_btn, t.btn_active, LV_PART_MAIN);
    }
    
    if (reboot_modal) {
        lv_obj_set_style_bg_color(reboot_modal, t.bg, LV_PART_MAIN);
        lv_obj_set_style_border_color(reboot_modal, t.alert, LV_PART_MAIN);
        lv_obj_set_style_bg_color(reboot_title, t.alert, LV_PART_MAIN);
        lv_obj_set_style_text_color(reboot_msg, t.text, LV_PART_MAIN);
        lv_obj_set_style_bg_color(reboot_yes_btn, t.alert, LV_PART_MAIN);
        lv_obj_set_style_bg_color(reboot_no_btn, t.border, LV_PART_MAIN);
    }

    if (onboarding_modal) {
        lv_obj_set_style_bg_color(onboarding_modal, t.bg, LV_PART_MAIN);
        lv_obj_set_style_border_color(onboarding_modal, t.border, LV_PART_MAIN);
        lv_obj_set_style_bg_color(onboarding_title, t.border, LV_PART_MAIN);
        lv_obj_set_style_text_color(onboarding_title, t.bg, LV_PART_MAIN);
        lv_obj_set_style_text_color(onboarding_ssid_label, t.highlight, LV_PART_MAIN);
        lv_obj_set_style_bg_color(onboarding_ap_btn, t.btn_active, LV_PART_MAIN);
        lv_obj_set_style_text_color(onboarding_ap_btn_label, t.bg, LV_PART_MAIN);
    }
}

// ─── Button Events ───
static void color_btn_cb(lv_event_t* e) {
    current_theme = (Theme)((current_theme + 1) % THEME_COUNT);
    apply_theme();
}

static void qr_btn_cb(lv_event_t* e) {
    if (qr_modal) {
        lv_obj_clear_flag(qr_modal, LV_OBJ_FLAG_HIDDEN);
        apply_theme();
    }
}

static void qr_close_cb(lv_event_t* e) {
    if (qr_modal) {
        lv_obj_add_flag(qr_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void reboot_btn_cb(lv_event_t* e) {
    if (reboot_modal) {
        lv_obj_clear_flag(reboot_modal, LV_OBJ_FLAG_HIDDEN);
        apply_theme();
    }
}

static void reboot_no_cb(lv_event_t* e) {
    if (reboot_modal) {
        lv_obj_add_flag(reboot_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void reboot_yes_cb(lv_event_t* e) {
    ESP_LOGI(TAG, "Hardware Reboot Triggered from Touch screen!");
    esp_restart();
}

static void onboarding_ap_cb(lv_event_t* e) {
    ESP_LOGI(TAG, "Standalone AP Mode triggered from Touch screen!");
    // Directly inject AP mode selection into NVS and reboot
    #if defined(ESP_PLATFORM)
    #include "nvs_flash.h"
    #include "nvs.h"
    nvs_handle_t nvs_handle;
    if (nvs_open("storage", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, "wifi_mode", 2); // 2 = Standalone AP
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    esp_restart();
    #endif
}

// ─── QR Code Generator Drawing ───
static void draw_qr(lv_obj_t* canvas, uint8_t* canvas_buffer, const char* text) {
    QRCode qrcode;
    uint8_t qrcodeData[qrcode_getBufferSize(4)]; // Fits onboarding strings safely
    if (qrcode_initText(&qrcode, qrcodeData, 4, ECC_LOW, text) != 0) {
        ESP_LOGE(TAG, "Failed to generate QR Code");
        return;
    }

    int scale = 3; // 33 * 3 = 99 pixels canvas width/height
    
    // Clear canvas to white
    lv_canvas_fill_bg(canvas, lv_color_make(255, 255, 255), LV_OPA_COVER);
    
    for (int y = 0; y < qrcode.size; y++) {
        for (int x = 0; x < qrcode.size; x++) {
            lv_color_t color = qrcode_getModule(&qrcode, x, y) ? lv_color_make(0, 0, 0) : lv_color_make(255, 255, 255);
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    lv_canvas_set_px(canvas, x * scale + sx, y * scale + sy, color);
                }
            }
        }
    }
}

// ─── UI Init ───
void ui_init(void) {
    ESP_LOGI(TAG, "Initializing LVGL HMI switchboard interface...");
    
    // Create base container covering 320x480 screen area
    main_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(main_container, 320, 480);
    lv_obj_set_style_pad_all(main_container, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(main_container, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(main_container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(main_container, LV_OBJ_FLAG_SCROLLABLE);

    // 1. Header Title Bar
    header_bar = lv_obj_create(main_container);
    lv_obj_set_size(header_bar, 320, 32);
    lv_obj_align(header_bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_pad_all(header_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(header_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header_bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header_bar, LV_OBJ_FLAG_SCROLLABLE);
    
    title_label = lv_label_create(header_bar);
    lv_label_set_text(title_label, " POCKET-DIAL HMI SWITCHBOARD");
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 6, 0);
    
    battery_label = lv_label_create(header_bar);
    lv_label_set_text(battery_label, "----V --%");
    lv_obj_align(battery_label, LV_ALIGN_RIGHT_MID, -6, 0);

    // 2. Status Box Panel
    status_panel = lv_obj_create(main_container);
    lv_obj_set_size(status_panel, 300, 142);
    lv_obj_align(status_panel, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_border_width(status_panel, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(status_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(status_panel, 10, LV_PART_MAIN);
    lv_obj_clear_flag(status_panel, LV_OBJ_FLAG_SCROLLABLE);
    
    status_title = lv_label_create(status_panel);
    lv_label_set_text(status_title, "☼ SYSTEM TELEMETRY");
    lv_obj_align(status_title, LV_ALIGN_TOP_LEFT, 0, 0);
    
    status_ip_label = lv_label_create(status_panel);
    lv_label_set_text(status_ip_label, "IP Address : 0.0.0.0");
    lv_obj_align(status_ip_label, LV_ALIGN_TOP_LEFT, 0, 22);
    
    status_uptime_label = lv_label_create(status_panel);
    lv_label_set_text(status_uptime_label, "Uptime     : 00:00:00");
    lv_obj_align(status_uptime_label, LV_ALIGN_TOP_LEFT, 0, 40);
    
    status_sta_label = lv_label_create(status_panel);
    lv_label_set_text(status_sta_label, "Stations   : 0 connected");
    lv_obj_align(status_sta_label, LV_ALIGN_TOP_LEFT, 0, 58);
    
    status_ext_label = lv_label_create(status_panel);
    lv_label_set_text(status_ext_label, "Extensions : 0 active");
    lv_obj_align(status_ext_label, LV_ALIGN_TOP_LEFT, 0, 76);
    
    status_sess_label = lv_label_create(status_panel);
    lv_label_set_text(status_sess_label, "Sessions   : 0 active");
    lv_obj_align(status_sess_label, LV_ALIGN_TOP_LEFT, 0, 94);

    // 3. Interactive Buttons Panel
    buttons_panel = lv_obj_create(main_container);
    lv_obj_set_size(buttons_panel, 300, 36);
    lv_obj_align(buttons_panel, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_set_style_pad_all(buttons_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(buttons_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(buttons_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(buttons_panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(buttons_panel, LV_OBJ_FLAG_SCROLLABLE);

    qr_btn = lv_btn_create(buttons_panel);
    lv_obj_set_size(qr_btn, 90, 32);
    lv_obj_align(qr_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(qr_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(qr_btn, qr_btn_cb, LV_EVENT_CLICKED, NULL);
    qr_btn_label = lv_label_create(qr_btn);
    lv_label_set_text(qr_btn_label, "WIFI QR");
    lv_obj_center(qr_btn_label);

    color_btn = lv_btn_create(buttons_panel);
    lv_obj_set_size(color_btn, 90, 32);
    lv_obj_align(color_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(color_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(color_btn, color_btn_cb, LV_EVENT_CLICKED, NULL);
    color_btn_label = lv_label_create(color_btn);
    lv_label_set_text(color_btn_label, "COLOR");
    lv_obj_center(color_btn_label);

    reboot_btn = lv_btn_create(buttons_panel);
    lv_obj_set_size(reboot_btn, 90, 32);
    lv_obj_align(reboot_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(reboot_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(reboot_btn, reboot_btn_cb, LV_EVENT_CLICKED, NULL);
    reboot_btn_label = lv_label_create(reboot_btn);
    lv_label_set_text(reboot_btn_label, "REBOOT");
    lv_obj_center(reboot_btn_label);

    // 4. Console Logs Box Panel
    log_panel = lv_obj_create(main_container);
    lv_obj_set_size(log_panel, 300, 240);
    lv_obj_align(log_panel, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_border_width(log_panel, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(log_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(log_panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(log_panel, LV_OBJ_FLAG_SCROLLABLE);

    log_title = lv_label_create(log_panel);
    lv_label_set_text(log_title, " ═════════ Live Console Logs ═════════");
    lv_obj_set_size(log_title, 296, 20);
    lv_obj_align(log_title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_top(log_title, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_left(log_title, 6, LV_PART_MAIN);

    log_textarea = lv_textarea_create(log_panel);
    lv_obj_set_size(log_textarea, 296, 214);
    lv_obj_align(log_textarea, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_border_width(log_textarea, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(log_textarea, 6, LV_PART_MAIN);
    lv_textarea_set_readonly(log_textarea, true);
    lv_textarea_set_max_length(log_textarea, 2048);

    // 5. Build Wi-Fi Quick Join Modal Overlay
    qr_modal = lv_obj_create(main_container);
    lv_obj_set_size(qr_modal, 260, 260);
    lv_obj_center(qr_modal);
    lv_obj_set_style_border_width(qr_modal, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(qr_modal, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(qr_modal, 10, LV_PART_MAIN);
    lv_obj_clear_flag(qr_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(qr_modal, LV_OBJ_FLAG_HIDDEN); // Hidden initially

    // Setup QR Canvas buffer (requires 99 * 99 * 2 bytes = 19602 bytes for 16-bit format)
    #if defined(ESP_PLATFORM)
    qr_canvas_buffer = (uint8_t*)malloc(19602);
    #else
    static uint8_t static_qr_buf[19602];
    qr_canvas_buffer = static_qr_buf;
    #endif
    
    qr_canvas = lv_canvas_create(qr_modal);
    lv_canvas_set_buffer(qr_canvas, qr_canvas_buffer, 99, 99, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(qr_canvas, LV_ALIGN_TOP_MID, 0, 10);
    draw_qr(qr_canvas, qr_canvas_buffer, "WIFI:S:esp32-sipserver;T:nopass;;"); // Default AP QR Code
    
    qr_close_btn = lv_btn_create(qr_modal);
    lv_obj_set_size(qr_close_btn, 160, 32);
    lv_obj_align(qr_close_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_radius(qr_close_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(qr_close_btn, qr_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* close_label = lv_label_create(qr_close_btn);
    lv_label_set_text(close_label, "CLOSE");
    lv_obj_center(close_label);

    // 6. Build Hardware Reboot Confirmation Modal
    reboot_modal = lv_obj_create(main_container);
    lv_obj_set_size(reboot_modal, 260, 180);
    lv_obj_center(reboot_modal);
    lv_obj_set_style_border_width(reboot_modal, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(reboot_modal, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(reboot_modal, 0, LV_PART_MAIN);
    lv_obj_clear_flag(reboot_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(reboot_modal, LV_OBJ_FLAG_HIDDEN); // Hidden initially

    reboot_title = lv_label_create(reboot_modal);
    lv_label_set_text(reboot_title, " ☣ WARNING: SYSTEM REBOOT ☣");
    lv_obj_set_size(reboot_title, 256, 20);
    lv_obj_align(reboot_title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_top(reboot_title, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_left(reboot_title, 6, LV_PART_MAIN);

    reboot_msg = lv_label_create(reboot_modal);
    lv_label_set_text(reboot_msg, "Are you sure you want to\nreboot the SIP PBX board?");
    lv_obj_align(reboot_msg, LV_ALIGN_CENTER, 0, -10);

    reboot_yes_btn = lv_btn_create(reboot_modal);
    lv_obj_set_size(reboot_yes_btn, 80, 32);
    lv_obj_align(reboot_yes_btn, LV_ALIGN_BOTTOM_LEFT, 20, -15);
    lv_obj_set_style_radius(reboot_yes_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(reboot_yes_btn, reboot_yes_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* yes_lbl = lv_label_create(reboot_yes_btn);
    lv_label_set_text(yes_lbl, "YES");
    lv_obj_center(yes_lbl);

    reboot_no_btn = lv_btn_create(reboot_modal);
    lv_obj_set_size(reboot_no_btn, 80, 32);
    lv_obj_align(reboot_no_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -15);
    lv_obj_set_style_radius(reboot_no_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(reboot_no_btn, reboot_no_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* no_lbl = lv_label_create(reboot_no_btn);
    lv_label_set_text(no_lbl, "NO");
    lv_obj_center(no_lbl);

    // Apply primary CGA Blue theme defaults
    apply_theme();
}

// ─── Set Onboarding UI Mode ───
void ui_set_onboarding_mode(bool onboarding, const char* ssid, const char* pass) {
    is_onboarding = onboarding;
    if (!onboarding) {
        if (onboarding_modal) {
            lv_obj_add_flag(onboarding_modal, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    ESP_LOGI(TAG, "Spinning up Onboarding UI View: SSID=%s", ssid);

    if (!onboarding_modal) {
        // Create full screen overlay modal for Wi-Fi Onboarding
        onboarding_modal = lv_obj_create(main_container);
        lv_obj_set_size(onboarding_modal, 320, 480);
        lv_obj_align(onboarding_modal, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_border_width(onboarding_modal, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(onboarding_modal, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(onboarding_modal, 10, LV_PART_MAIN);
        lv_obj_clear_flag(onboarding_modal, LV_OBJ_FLAG_SCROLLABLE);

        onboarding_title = lv_label_create(onboarding_modal);
        lv_label_set_text(onboarding_title, "  ═══ AWAITING WI-FI CONFIG ═══");
        lv_obj_set_size(onboarding_title, 300, 24);
        lv_obj_align(onboarding_title, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_set_style_pad_top(onboarding_title, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_left(onboarding_title, 14, LV_PART_MAIN);

        onboarding_ssid_label = lv_label_create(onboarding_modal);
        lv_obj_align(onboarding_ssid_label, LV_ALIGN_TOP_MID, 0, 50);

        // QR code join canvas (99x99 scale, requires 19602 byte buffer)
        #if defined(ESP_PLATFORM)
        onboarding_qr_buffer = (uint8_t*)malloc(19602);
        #else
        static uint8_t static_onboard_qr_buf[19602];
        onboarding_qr_buffer = static_onboard_qr_buf;
        #endif
        
        onboarding_qr_canvas = lv_canvas_create(onboarding_modal);
        lv_canvas_set_buffer(onboarding_qr_canvas, onboarding_qr_buffer, 99, 99, LV_IMG_CF_TRUE_COLOR);
        lv_obj_align(onboarding_qr_canvas, LV_ALIGN_CENTER, 0, -20);

        lv_obj_t* prompt_lbl = lv_label_create(onboarding_modal);
        lv_label_set_text(prompt_lbl, "Scan the QR code to join Wi-Fi,\nthen access: http://192.168.4.1/\nor choose AP mode below to run\nthe server standalone.");
        lv_obj_set_style_text_align(prompt_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(prompt_lbl, LV_ALIGN_BOTTOM_MID, 0, -110);

        onboarding_ap_btn = lv_btn_create(onboarding_modal);
        lv_obj_set_size(onboarding_ap_btn, 240, 40);
        lv_obj_align(onboarding_ap_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
        lv_obj_set_style_radius(onboarding_ap_btn, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(onboarding_ap_btn, onboarding_ap_cb, LV_EVENT_CLICKED, NULL);
        onboarding_ap_btn_label = lv_label_create(onboarding_ap_btn);
        lv_label_set_text(onboarding_ap_btn_label, "START STANDALONE AP MODE");
        lv_obj_center(onboarding_ap_btn_label);
    }

    lv_obj_clear_flag(onboarding_modal, LV_OBJ_FLAG_HIDDEN);
    
    char ssid_msg[128];
    snprintf(ssid_msg, sizeof(ssid_msg), "SSID: %s\nPass: %s", ssid, pass);
    lv_label_set_text(onboarding_ssid_label, ssid_msg);
    lv_obj_set_style_text_align(onboarding_ssid_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // Format QR configuration string (SSID open or WPA)
    char qr_str[128];
    if (strlen(pass) > 0) {
        snprintf(qr_str, sizeof(qr_str), "WIFI:S:%s;T:WPA;P:%s;;", ssid, pass);
    } else {
        snprintf(qr_str, sizeof(qr_str), "WIFI:S:%s;T:nopass;;", ssid);
    }
    
    draw_qr(onboarding_qr_canvas, onboarding_qr_buffer, qr_str);
    
    // Also recreate/draw the default dashboard's QR modal to match the onboarding settings
    if (qr_canvas) {
        draw_qr(qr_canvas, qr_canvas_buffer, qr_str);
    }

    apply_theme();
}

// ─── Update Status Dashboard ───
void ui_update_status(const std::string& ip, int uptimeSec, int stationNum, int clientCount, int sessionCount) {
    if (is_onboarding) return; // Do not overwrite onboarding view values

    char buf[64];
    snprintf(buf, sizeof(buf), "IP Address : %s", ip.c_str());
    lv_label_set_text(status_ip_label, buf);

    int hrs = uptimeSec / 3600;
    int mins = (uptimeSec % 3600) / 60;
    int secs = uptimeSec % 60;
    snprintf(buf, sizeof(buf), "Uptime     : %02d:%02d:%02d", hrs, mins, secs);
    lv_label_set_text(status_uptime_label, buf);

    snprintf(buf, sizeof(buf), "Stations   : %d connected", stationNum);
    lv_label_set_text(status_sta_label, buf);

    snprintf(buf, sizeof(buf), "Extensions : %d active", clientCount);
    lv_label_set_text(status_ext_label, buf);

    snprintf(buf, sizeof(buf), "Sessions   : %d active", sessionCount);
    lv_label_set_text(status_sess_label, buf);
}

// ─── Add Scrolling Console Log ───
void ui_add_log(const char* line) {
    if (log_textarea) {
        lv_textarea_add_text(log_textarea, line);
        lv_textarea_add_text(log_textarea, "\n");
    }
}

// ─── Set Battery Status Header ───
void ui_set_battery(float volts, int percent) {
    char buf[32];
    if (percent >= 0) {
        snprintf(buf, sizeof(buf), "%.1fV %d%%", volts, percent);
    } else {
        snprintf(buf, sizeof(buf), "----V --%");
    }
    if (battery_label) {
        lv_label_set_text(battery_label, buf);
    }
}

// ─── Direct Touch Coordinates Press Router ───
void ui_handle_touch_press(int16_t x, int16_t y) {
    ESP_LOGI(TAG, "Touch coordinates input: X=%d, Y=%d", x, y);
    
    // Simulate press events by directly mapping touch coordinates to on-screen buttons
    // This serves as an extremely robust fallback if standard LVGL input drivers 
    // hit coordinate calibration offsets.
    if (is_onboarding) {
        // [ START STANDALONE AP MODE ] button centered near bottom: Y: 410..450, X: 40..280
        if (x >= 40 && x <= 280 && y >= 405 && y <= 455) {
            ESP_LOGI(TAG, "Simulated press: Onboarding Standalone AP button");
            lv_event_send(onboarding_ap_btn, LV_EVENT_CLICKED, NULL);
        }
        return;
    }

    if (reboot_modal && !lv_obj_has_flag(reboot_modal, LV_OBJ_FLAG_HIDDEN)) {
        // YES reboot button: Y: 133..165, X: 45..125
        if (x >= 35 && x <= 135 && y >= 125 && y <= 170) {
            ESP_LOGI(TAG, "Simulated press: Reboot YES button");
            lv_event_send(reboot_yes_btn, LV_EVENT_CLICKED, NULL);
        }
        // NO cancel button: Y: 133..165, X: 165..245
        else if (x >= 155 && x <= 255 && y >= 125 && y <= 170) {
            ESP_LOGI(TAG, "Simulated press: Reboot NO button");
            lv_event_send(reboot_no_btn, LV_EVENT_CLICKED, NULL);
        }
        return;
    }

    if (qr_modal && !lv_obj_has_flag(qr_modal, LV_OBJ_FLAG_HIDDEN)) {
        // Close QR code button centered near bottom: Y: 218..250, X: 50..210
        if (x >= 40 && x <= 220 && y >= 210 && y <= 255) {
            ESP_LOGI(TAG, "Simulated press: QR Close button");
            lv_event_send(qr_close_btn, LV_EVENT_CLICKED, NULL);
        }
        return;
    }

    // Main buttons coordinates inside buttons_panel (Y: 190..222)
    if (y >= 185 && y <= 226) {
        // WIFI QR Button (X: 10..100)
        if (x >= 10 && x <= 105) {
            ESP_LOGI(TAG, "Simulated press: Main WIFI QR button");
            lv_event_send(qr_btn, LV_EVENT_CLICKED, NULL);
        }
        // COLOR Theme Toggle Button (X: 115..205)
        else if (x >= 110 && x <= 210) {
            ESP_LOGI(TAG, "Simulated press: Main COLOR theme button");
            lv_event_send(color_btn, LV_EVENT_CLICKED, NULL);
        }
        // REBOOT Warning Button (X: 215..305)
        else if (x >= 212 && x <= 310) {
            ESP_LOGI(TAG, "Simulated press: Main REBOOT button");
            lv_event_send(reboot_btn, LV_EVENT_CLICKED, NULL);
        }
    }
}
