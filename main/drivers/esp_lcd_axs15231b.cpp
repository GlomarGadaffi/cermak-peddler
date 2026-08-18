/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <sys/cdefs.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_touch.h"

#include "esp_lcd_axs15231b.h"

/*max point num*/
#define AXS_MAX_TOUCH_NUMBER                (1)

#define LCD_OPCODE_WRITE_CMD                (0x02ULL)
#define LCD_OPCODE_READ_CMD                 (0x0BULL)
#define LCD_OPCODE_WRITE_COLOR              (0x32ULL)

static const char *TAG = "lcd_panel.axs15231b";

static esp_err_t panel_axs15231b_del(esp_lcd_panel_t *panel);
static esp_err_t panel_axs15231b_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_axs15231b_init(esp_lcd_panel_t *panel);
static esp_err_t panel_axs15231b_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_axs15231b_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_axs15231b_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_axs15231b_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_axs15231b_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_axs15231b_disp_off(esp_lcd_panel_t *panel, bool off);

static esp_err_t touch_axs15231b_read_data(esp_lcd_touch_handle_t tp);
static bool touch_axs15231b_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
static esp_err_t touch_axs15231b_del(esp_lcd_touch_handle_t tp);
static esp_err_t touch_axs15231b_reset(esp_lcd_touch_handle_t tp);

static esp_err_t i2c_read_bytes(esp_lcd_touch_handle_t tp, int reg, uint8_t *data, uint8_t len);
static esp_err_t i2c_write_bytes(esp_lcd_touch_handle_t tp, int reg, const uint8_t *data, uint8_t len);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // save surrent value of LCD_CMD_COLMOD register
    const axs15231b_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        unsigned int use_qspi_interface: 1;
        unsigned int reset_level: 1;
    } flags;
} axs15231b_panel_t;

esp_err_t esp_lcd_new_panel_axs15231b(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    axs15231b_panel_t *axs15231b = NULL;
    // Declared up here, before the first ESP_GOTO_*, rather than at its point of
    // use further down: this file is compiled as C++, where jumping over a
    // declaration with an initializer is ill-formed ("jump to label 'err' crosses
    // initialization of ..."). C allowed it, which is why this only surfaced when
    // the file was renamed .c -> .cpp.
    uint8_t fb_bits_per_pixel = 0;
    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    axs15231b = (axs15231b_panel_t *)calloc(1, sizeof(axs15231b_panel_t));
    ESP_GOTO_ON_FALSE(axs15231b, ESP_ERR_NO_MEM, err, TAG, "no mem for axs15231b panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        // `= {}` then assign, rather than designated initializers: C++ requires
        // designators in declaration order (C99 did not), and a partial designated
        // initializer also trips -Werror=missing-field-initializers under -Wextra.
        // Value-initializing first sidesteps both, and stays correct if a future
        // IDF adds a field to gpio_config_t.
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num;
        io_conf.mode         = GPIO_MODE_OUTPUT;
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        axs15231b->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        axs15231b->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported RGB element order");
        break;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        axs15231b->colmod_val = 0x55;
        fb_bits_per_pixel = 16;
        break;
    case 18: // RGB666
        axs15231b->colmod_val = 0x66;
        // each color component (R/G/B) should occupy the 6 high bits of a byte, which means 3 full bytes are required for a pixel
        fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    axs15231b->io = io;
    axs15231b->fb_bits_per_pixel = fb_bits_per_pixel;
    axs15231b->reset_gpio_num = panel_dev_config->reset_gpio_num;
    axs15231b->flags.reset_level = panel_dev_config->flags.reset_active_high;
    if (panel_dev_config->vendor_config) {
        axs15231b->init_cmds = ((axs15231b_vendor_config_t *)panel_dev_config->vendor_config)->init_cmds;
        axs15231b->init_cmds_size = ((axs15231b_vendor_config_t *)panel_dev_config->vendor_config)->init_cmds_size;
        axs15231b->flags.use_qspi_interface = ((axs15231b_vendor_config_t *)panel_dev_config->vendor_config)->flags.use_qspi_interface;
    }
    axs15231b->base.del = panel_axs15231b_del;
    axs15231b->base.reset = panel_axs15231b_reset;
    axs15231b->base.init = panel_axs15231b_init;
    axs15231b->base.draw_bitmap = panel_axs15231b_draw_bitmap;
    axs15231b->base.invert_color = panel_axs15231b_invert_color;
    axs15231b->base.set_gap = panel_axs15231b_set_gap;
    axs15231b->base.mirror = panel_axs15231b_mirror;
    axs15231b->base.swap_xy = panel_axs15231b_swap_xy;
    axs15231b->base.disp_on_off  = panel_axs15231b_disp_off;
    *ret_panel = &(axs15231b->base);
    ESP_LOGD(TAG, "new axs15231b panel @%p", axs15231b);
    ESP_LOGI(TAG, "LCD panel create success, version: %d.%d.%d", ESP_LCD_AXS15231B_VER_MAJOR, ESP_LCD_AXS15231B_VER_MINOR,
             ESP_LCD_AXS15231B_VER_PATCH);

    return ESP_OK;

err:
    if (axs15231b) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin((gpio_num_t)panel_dev_config->reset_gpio_num);
        }
        free(axs15231b);
    }
    return ret;
}

static esp_err_t tx_param(axs15231b_panel_t *axs15231b, esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param, size_t param_size)
{
    if (axs15231b->flags.use_qspi_interface) {
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
    }
    return esp_lcd_panel_io_tx_param(io, lcd_cmd, param, param_size);
}

static esp_err_t tx_color(axs15231b_panel_t *axs15231b, esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param, size_t param_size)
{
    if (axs15231b->flags.use_qspi_interface) {
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_COLOR << 24;
    }
    return esp_lcd_panel_io_tx_color(io, lcd_cmd, param, param_size);
}

static esp_err_t panel_axs15231b_del(esp_lcd_panel_t *panel)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);

    if (axs15231b->reset_gpio_num >= 0) {
        gpio_reset_pin((gpio_num_t)axs15231b->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del axs15231b panel @%p", axs15231b);
    free(axs15231b);
    return ESP_OK;
}

static esp_err_t panel_axs15231b_reset(esp_lcd_panel_t *panel)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);
    esp_lcd_panel_io_handle_t io = axs15231b->io;

    // perform hardware reset
    if (axs15231b->reset_gpio_num >= 0) {
        gpio_set_level((gpio_num_t)axs15231b->reset_gpio_num, !axs15231b->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level((gpio_num_t)axs15231b->reset_gpio_num, axs15231b->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level((gpio_num_t)axs15231b->reset_gpio_num, !axs15231b->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else { // perform software reset
        tx_param(axs15231b, io, LCD_CMD_SWRESET, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(120)); // spec, wait at least 5m before sending new command
    }

    return ESP_OK;
}

// Verified working JC3248W535EN init sequence (from tsebelev's known-good ESP-IDF
// example for this exact board). The previous table was a different/incorrect AXS15231B
// variant — its gamma/power registers were wrong and it never set the CASET/RASET window,
// so the panel showed only frozen power-on RAM. This one ends by establishing the full
// 320x480 window (0x2A / 0x2B). DISPON (0x29) is issued separately via disp_on_off().
static const axs15231b_lcd_init_cmd_t vendor_specific_init_default[] = {
    {0xBB, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5}, 8, 0},
    {0xA0, (uint8_t[]){0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00}, 17, 0},
    {0xA2, (uint8_t[]){0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xf9, 0x10, 0x02, 0xff, 0xff, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A}, 31, 0},
    {0xD0, (uint8_t[]){0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0x42, 0xC2, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12}, 30, 0},
    {0xA3, (uint8_t[]){0xA0, 0x06, 0xAa, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55}, 22, 0},
    {0xC1, (uint8_t[]){0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0d, 0x00, 0xFF, 0x40}, 30, 0},
    {0xC3, (uint8_t[]){0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01}, 11, 0},
    {0xC4, (uint8_t[]){0x00, 0x24, 0x33, 0x80, 0x00, 0xea, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50}, 29, 0},
    {0xC5, (uint8_t[]){0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00}, 23, 0},
    {0xC6, (uint8_t[]){0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22}, 20, 0},
    {0xC7, (uint8_t[]){0x50, 0x32, 0x28, 0x00, 0xa2, 0x80, 0x8f, 0x00, 0x80, 0xff, 0x07, 0x11, 0x9c, 0x67, 0xff, 0x24, 0x0c, 0x0d, 0x0e, 0x0f}, 20, 0},
    {0xC9, (uint8_t[]){0x33, 0x44, 0x44, 0x01}, 4, 0},
    {0xCF, (uint8_t[]){0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08}, 27, 0},
    {0xD5, (uint8_t[]){0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00}, 30, 0},
    {0xD6, (uint8_t[]){0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00}, 30, 0},
    {0xD7, (uint8_t[]){0x03, 0x01, 0x0b, 0x09, 0x0f, 0x0d, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F}, 19, 0},
    {0xD8, (uint8_t[]){0x02, 0x00, 0x0a, 0x08, 0x0e, 0x0c, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19}, 12, 0},
    {0xD9, (uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDD, (uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDF, (uint8_t[]){0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90}, 8, 0},
    {0xE0, (uint8_t[]){0x3B, 0x28, 0x10, 0x16, 0x0c, 0x06, 0x11, 0x28, 0x5c, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D}, 17, 0},
    {0xE1, (uint8_t[]){0x37, 0x28, 0x10, 0x16, 0x0b, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F}, 17, 0},
    {0xE2, (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE3, (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F}, 17, 0},
    {0xE4, (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE5, (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F}, 17, 0},
    {0xA4, (uint8_t[]){0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30}, 16, 0},
    {0xA4, (uint8_t[]){0x85, 0x85, 0x95, 0x85}, 4, 0},
    {0xBB, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 0},
    {0x13, (uint8_t[]){0x00}, 0, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x35, (uint8_t[]){0x00}, 1, 0},   // TEON: drive the Tearing-Effect line on V-blank
    {0x2C, (uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x3f}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xdf}, 4, 0},
};

static esp_err_t panel_axs15231b_init(esp_lcd_panel_t *panel)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);
    esp_lcd_panel_io_handle_t io = axs15231b->io;

    // LCD goes into sleep mode and display will be turned off after power on reset, exit sleep mode first
    ESP_RETURN_ON_ERROR(tx_param(axs15231b, io, LCD_CMD_SLPOUT, NULL, 0), TAG, "send command failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(tx_param(axs15231b, io, LCD_CMD_MADCTL, (uint8_t[]) {
        axs15231b->madctl_val,
    }, 1), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(tx_param(axs15231b, io, LCD_CMD_COLMOD, (uint8_t[]) {
        axs15231b->colmod_val,
    }, 1), TAG, "send command failed");

    const axs15231b_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    if (axs15231b->init_cmds) {
        init_cmds = axs15231b->init_cmds;
        init_cmds_size = axs15231b->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(axs15231b_lcd_init_cmd_t);
    }

    bool is_cmd_overwritten = false;
    for (int i = 0; i < init_cmds_size; i++) {
        // Check if the command has been used or conflicts with the internal
        switch (init_cmds[i].cmd) {
        case LCD_CMD_MADCTL:
            is_cmd_overwritten = true;
            axs15231b->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
            break;
        case LCD_CMD_COLMOD:
            is_cmd_overwritten = true;
            axs15231b->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
            break;
        default:
            is_cmd_overwritten = false;
            break;
        }

        if (is_cmd_overwritten) {
            ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence", init_cmds[i].cmd);
        }
        ESP_RETURN_ON_ERROR(tx_param(axs15231b, io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
    }
    ESP_LOGI(TAG, "send init commands success");

    return ESP_OK;
}

static esp_err_t panel_axs15231b_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = axs15231b->io;

    x_start += axs15231b->x_gap;
    x_end += axs15231b->x_gap;
    y_start += axs15231b->y_gap;
    y_end += axs15231b->y_gap;

    // define an area of frame memory where MCU can access
    tx_param(axs15231b, io, LCD_CMD_CASET, (uint8_t[]) {
        (uint8_t)((x_start >> 8) & 0xFF),
        (uint8_t)(x_start & 0xFF),
        (uint8_t)(((x_end - 1) >> 8) & 0xFF),
        (uint8_t)((x_end - 1) & 0xFF),
    }, 4);

    // QSPI: skip per-flush RASET. This panel's QSPI mode does not handle arbitrary windowed
    // writes cleanly (they garble), so the frame is filled by RAMWR for the top band
    // (y_start==0) + RAMWRC continuation for the bands below. This requires whole-screen,
    // top-to-bottom flushes — the app drives LVGL in full_refresh mode so every refresh
    // sweeps the entire frame in order.
    if (0 == axs15231b->flags.use_qspi_interface) {
        tx_param(axs15231b, io, LCD_CMD_RASET, (uint8_t[]) {
            (uint8_t)((y_start >> 8) & 0xFF),
            (uint8_t)(y_start & 0xFF),
            (uint8_t)(((y_end - 1) >> 8) & 0xFF),
            (uint8_t)((y_end - 1) & 0xFF),
        }, 4);
    }

    // transfer frame buffer
    size_t len = (x_end - x_start) * (y_end - y_start) * axs15231b->fb_bits_per_pixel / 8;
    if (y_start == 0) {
        tx_color(axs15231b, io, LCD_CMD_RAMWR, color_data, len);//2C
    } else {
        tx_color(axs15231b, io, LCD_CMD_RAMWRC, color_data, len);//3C
    }

    return ESP_OK;
}

static esp_err_t panel_axs15231b_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);
    esp_lcd_panel_io_handle_t io = axs15231b->io;
    int command = 0;
    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    tx_param(axs15231b, io, command, NULL, 0);
    return ESP_OK;
}

static esp_err_t panel_axs15231b_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);
    esp_lcd_panel_io_handle_t io = axs15231b->io;
    if (mirror_x) {
        axs15231b->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        axs15231b->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        axs15231b->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        axs15231b->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    tx_param(axs15231b, io, LCD_CMD_MADCTL, (uint8_t[]) {
        axs15231b->madctl_val
    }, 1);
    return ESP_OK;
}

static esp_err_t panel_axs15231b_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);
    esp_lcd_panel_io_handle_t io = axs15231b->io;
    if (swap_axes) {
        axs15231b->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        axs15231b->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    tx_param(axs15231b, io, LCD_CMD_MADCTL, (uint8_t[]) {
        axs15231b->madctl_val
    }, 1);
    return ESP_OK;
}

static esp_err_t panel_axs15231b_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);
    axs15231b->x_gap = x_gap;
    axs15231b->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_axs15231b_disp_off(esp_lcd_panel_t *panel, bool off)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);
    esp_lcd_panel_io_handle_t io = axs15231b->io;
    int command = 0;
    if (off) {
        command = LCD_CMD_DISPOFF;
    } else {
        command = LCD_CMD_DISPON;
    }
    tx_param(axs15231b, io, command, NULL, 0);
    return ESP_OK;
}

esp_err_t esp_lcd_touch_new_i2c_axs15231b(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *tp)
{
    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_ARG, TAG, "Invalid io");
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid config");
    ESP_RETURN_ON_FALSE(tp, ESP_ERR_INVALID_ARG, TAG, "Invalid touch handle");

    /* Prepare main structure */
    esp_err_t ret = ESP_OK;
    esp_lcd_touch_handle_t axs15231b = (esp_lcd_touch_handle_t)calloc(1, sizeof(esp_lcd_touch_t));
    ESP_GOTO_ON_FALSE(axs15231b, ESP_ERR_NO_MEM, err, TAG, "Touch handle malloc failed");

    /* Communication interface */
    axs15231b->io = io;
    /* Only supported callbacks are set */
    axs15231b->read_data = touch_axs15231b_read_data;
    axs15231b->get_xy = touch_axs15231b_get_xy;
    axs15231b->del = touch_axs15231b_del;
    /* Mutex */
    axs15231b->data.lock.owner = portMUX_FREE_VAL;
    /* Save config */
    memcpy(&axs15231b->config, config, sizeof(esp_lcd_touch_config_t));

    /* Prepare pin for touch interrupt */
    if (axs15231b->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_config_t int_gpio_config = {};   // see the note on io_conf above
        int_gpio_config.pin_bit_mask = BIT64(axs15231b->config.int_gpio_num);
        int_gpio_config.mode         = GPIO_MODE_INPUT;
        int_gpio_config.intr_type    = GPIO_INTR_NEGEDGE;
        ESP_GOTO_ON_ERROR(gpio_config(&int_gpio_config), err, TAG, "GPIO intr config failed");

        /* Register interrupt callback */
        if (axs15231b->config.interrupt_callback) {
            esp_lcd_touch_register_interrupt_callback(axs15231b, axs15231b->config.interrupt_callback);
        }
    }
    /* Prepare pin for touch controller reset */
    if (axs15231b->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_config_t rst_gpio_config = {};   // see the note on io_conf above
        rst_gpio_config.pin_bit_mask = BIT64(axs15231b->config.rst_gpio_num);
        rst_gpio_config.mode         = GPIO_MODE_OUTPUT;
        ESP_GOTO_ON_ERROR(gpio_config(&rst_gpio_config), err, TAG, "GPIO reset config failed");
    }
    /* Reset controller */
    ESP_GOTO_ON_ERROR(touch_axs15231b_reset(axs15231b), err, TAG, "Reset failed");
    *tp = axs15231b;

    return ESP_OK;
err:
    if (axs15231b) {
        touch_axs15231b_del(axs15231b);
    }
    ESP_LOGE(TAG, "Initialization failed!");
    return ret;
}

static esp_err_t touch_axs15231b_read_data(esp_lcd_touch_handle_t tp)
{
    typedef struct {
        uint8_t gesture;    //AXS_TOUCH_GESTURE_POS:0
        uint8_t num;        //AXS_TOUCH_POINT_NUM:1
        uint8_t x_h : 4;    //AXS_TOUCH_X_H_POS:2
        uint8_t : 2;
        uint8_t event : 2;  //AXS_TOUCH_EVENT_POS:2
        uint8_t x_l;        //AXS_TOUCH_X_L_POS:3
        uint8_t y_h : 4;    //AXS_TOUCH_Y_H_POS:4
        uint8_t : 4;
        uint8_t y_l;        //AXS_TOUCH_Y_L_POS:5
    } __attribute__((packed)) touch_record_struct_t;

    touch_record_struct_t *p_touch_data = NULL;

    uint8_t data[AXS_MAX_TOUCH_NUMBER * 6 + 2] = {0}; /*1 Point:8;  2 Point: 14 */
    const uint8_t read_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, (AXS_MAX_TOUCH_NUMBER * 6 + 2) >> 8, (AXS_MAX_TOUCH_NUMBER * 6 + 2) & 0xff, 0x00, 0x00, 0x00};

    ESP_RETURN_ON_ERROR(i2c_write_bytes(tp, -1, read_cmd, sizeof(read_cmd)), TAG, "I2C write failed");
    ESP_RETURN_ON_ERROR(i2c_read_bytes(tp, -1, data, sizeof(data)), TAG, "I2C read failed");

    p_touch_data = (touch_record_struct_t *) data;
    if (p_touch_data->num && (AXS_MAX_TOUCH_NUMBER >= p_touch_data->num)) {
        portENTER_CRITICAL(&tp->data.lock);
        tp->data.points = p_touch_data->num;
        /* Fill all coordinates */
        for (int i = 0; i < tp->data.points; i++) {
            tp->data.coords[i].x = ((p_touch_data->x_h & 0x0F) << 8) | p_touch_data->x_l;
            tp->data.coords[i].y = ((p_touch_data->y_h & 0x0F) << 8) | p_touch_data->y_l;
        }
        portEXIT_CRITICAL(&tp->data.lock);
    }

    return ESP_OK;
}

static bool touch_axs15231b_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    /* Count of points */
    *point_num = (tp->data.points > max_point_num ? max_point_num : tp->data.points);
    for (size_t i = 0; i < *point_num; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;

        if (strength) {
            strength[i] = tp->data.coords[i].strength;
        }
    }
    /* Invalidate */
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);

    return (*point_num > 0);
}

static esp_err_t touch_axs15231b_del(esp_lcd_touch_handle_t tp)
{
    /* Reset GPIO pin settings */
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
    }
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.rst_gpio_num);
    }
    /* Release memory */
    free(tp);

    return ESP_OK;
}

static esp_err_t touch_axs15231b_reset(esp_lcd_touch_handle_t tp)
{
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset), TAG, "GPIO set level failed");
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset), TAG, "GPIO set level failed");
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    return ESP_OK;
}

static esp_err_t i2c_read_bytes(esp_lcd_touch_handle_t tp, int reg, uint8_t *data, uint8_t len)
{
    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "Invalid data");

    return esp_lcd_panel_io_rx_param(tp->io, reg, data, len);
}

static esp_err_t i2c_write_bytes(esp_lcd_touch_handle_t tp, int reg, const uint8_t *data, uint8_t len)
{
    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "Invalid data");

    return esp_lcd_panel_io_tx_param(tp->io, reg, data, len);
}
