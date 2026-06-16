/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_lcd_fl7707.h"

static const char *TAG = "fl7707";

typedef struct {
    esp_lcd_panel_io_handle_t io;          /* DBI IO handle for sending commands */
    int reset_gpio_num;
    uint8_t madctl_val;
    uint8_t colmod_val;
    const fl7707_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    uint8_t lane_num;
    struct {
        unsigned int reset_level : 1;
    } flags;
    /* Save original MIPI DPI panel function pointers */
    esp_err_t (*del)(esp_lcd_panel_t *panel);
    esp_err_t (*init)(esp_lcd_panel_t *panel);
} fl7707_panel_t;

static esp_err_t panel_fl7707_del(esp_lcd_panel_t *panel);
static esp_err_t panel_fl7707_init(esp_lcd_panel_t *panel);
static esp_err_t panel_fl7707_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_fl7707_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_fl7707_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_fl7707_disp_on_off(esp_lcd_panel_t *panel, bool on_off);
static esp_err_t panel_fl7707_sleep(esp_lcd_panel_t *panel, bool sleep);

/* ══════════════════════════════════════════════════
 *  Panel constructor
 * ══════════════════════════════════════════════════ */

esp_err_t esp_lcd_new_panel_fl7707(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");

    fl7707_vendor_config_t *vendor_config = (fl7707_vendor_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(vendor_config && vendor_config->mipi_config.dpi_config &&
                        vendor_config->mipi_config.dsi_bus, ESP_ERR_INVALID_ARG, TAG,
                        "invalid vendor config");

    esp_err_t ret = ESP_OK;
    fl7707_panel_t *fl7707 = (fl7707_panel_t *)calloc(1, sizeof(fl7707_panel_t));
    ESP_RETURN_ON_FALSE(fl7707, ESP_ERR_NO_MEM, TAG, "no mem for fl7707 panel");

    /* Configure reset GPIO if provided */
    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure RST GPIO failed");
    }

    /* MADCTL: RGB element order */
    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        fl7707->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        fl7707->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported RGB order");
        break;
    }

    /* COLMOD: bits per pixel */
    switch (panel_dev_config->bits_per_pixel) {
    case 16:
        fl7707->colmod_val = 0x55;  // RGB565
        break;
    case 18:
        fl7707->colmod_val = 0x66;  // RGB666
        break;
    case 24:
        fl7707->colmod_val = 0x77;  // RGB888
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported bpp");
        break;
    }

    fl7707->io = io;
    fl7707->init_cmds = vendor_config->init_cmds;
    fl7707->init_cmds_size = vendor_config->init_cmds_size;
    fl7707->lane_num = vendor_config->mipi_config.lane_num;
    fl7707->reset_gpio_num = panel_dev_config->reset_gpio_num;
    fl7707->flags.reset_level = panel_dev_config->flags.reset_active_high;

    /* Create the underlying MIPI DPI panel */
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_dpi(vendor_config->mipi_config.dsi_bus,
                                            vendor_config->mipi_config.dpi_config,
                                            ret_panel),
                      err, TAG, "create MIPI DPI panel failed");
    ESP_LOGD(TAG, "MIPI DPI panel created @%p", *ret_panel);

    /* Save original DPI panel callbacks */
    fl7707->del = (*ret_panel)->del;
    fl7707->init = (*ret_panel)->init;

    /* Overwrite with FL7707 callbacks */
    (*ret_panel)->del = panel_fl7707_del;
    (*ret_panel)->init = panel_fl7707_init;
    (*ret_panel)->reset = panel_fl7707_reset;
    (*ret_panel)->mirror = panel_fl7707_mirror;
    (*ret_panel)->invert_color = panel_fl7707_invert_color;
    (*ret_panel)->disp_on_off = panel_fl7707_disp_on_off;
    (*ret_panel)->disp_sleep = panel_fl7707_sleep;
    (*ret_panel)->user_data = fl7707;

    ESP_LOGD(TAG, "FL7707 panel created @%p", fl7707);
    return ESP_OK;

err:
    if (fl7707) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(fl7707);
    }
    return ret;
}

/* ══════════════════════════════════════════════════
 *  Default vendor init sequence for FL7707
 * ══════════════════════════════════════════════════ */

static const fl7707_lcd_init_cmd_t vendor_specific_init_default[] = {
    // {cmd, { data }, data_size, delay_ms}
    {0xB9, (uint8_t[]){0xF1, 0x12, 0x87}, 3, 0},
    {0xB2, (uint8_t[]){0xB4, 0x03, 0x70}, 3, 0},
    {0xB3, (uint8_t[]){0x10, 0x10, 0x28, 0x28, 0x03, 0xFF, 0x00, 0x00, 0x00, 0x00}, 10, 0},
    {0xB4, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x0A, 0x0A}, 2, 0},
    {0xB6, (uint8_t[]){0x8D, 0x8D}, 2, 0},
    {0xB8, (uint8_t[]){0x26, 0x22, 0xF0, 0x13}, 4, 0},
    {0xBA, (uint8_t[]){0x31, 0x81, 0x05, 0xF9, 0x0E, 0x0E, 0x20, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x44, 0x25, 0x00, 0x91, 0x0A, 0x00, 0x00, 0x01,
                       0x4F, 0x01, 0x00, 0x00, 0x37}, 27, 0},
    {0xBC, (uint8_t[]){0x47}, 1, 0},
    {0xBF, (uint8_t[]){0x02, 0x10, 0x00, 0x80, 0x04}, 5, 0},
    {0xC0, (uint8_t[]){0x73, 0x73, 0x50, 0x50, 0x00, 0x00, 0x12, 0x73, 0x00}, 9, 0},
    {0xC1, (uint8_t[]){0x36, 0x00, 0x32, 0x32, 0x77, 0xE1, 0x77, 0x77, 0xCC, 0xCC,
                       0xFF, 0xFF, 0x11, 0x11, 0x00, 0x00, 0x32}, 17, 0},
    {0xC7, (uint8_t[]){0x10, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0xED, 0xC5, 0x00, 0xA5}, 12, 0},
    {0xC8, (uint8_t[]){0x10, 0x40, 0x1E, 0x03}, 4, 0},
    {0xCC, (uint8_t[]){0x0B}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x0A, 0x0F, 0x2A, 0x33, 0x3F, 0x44, 0x39, 0x06, 0x0C,
                       0x0E, 0x14, 0x15, 0x13, 0x15, 0x10, 0x18,
                       0x00, 0x0A, 0x0F, 0x2A, 0x33, 0x3F, 0x44, 0x39, 0x06, 0x0C,
                       0x0E, 0x14, 0x15, 0x13, 0x15, 0x10, 0x18}, 34, 0},
    {0xE1, (uint8_t[]){0x11, 0x11, 0x91, 0x00, 0x00, 0x00, 0x00}, 7, 0},
    {0xE3, (uint8_t[]){0x07, 0x07, 0x0B, 0x0B, 0x0B, 0x0B, 0x00, 0x00, 0x00, 0x00,
                       0xFF, 0x04, 0xC0, 0x10}, 14, 0},
    {0xE9, (uint8_t[]){0xC8, 0x10, 0x0A, 0x00, 0x00, 0x80, 0x81, 0x12, 0x31, 0x23,
                       0x4F, 0x86, 0xA0, 0x00, 0x47, 0x08,
                       0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x00,
                       0x00, 0x00, 0x98, 0x02, 0x8B, 0xAF,
                       0x46, 0x02, 0x88, 0x88, 0x88, 0x88, 0x88, 0x98, 0x13, 0x8B,
                       0xAF, 0x57, 0x13, 0x88, 0x88, 0x88,
                       0x88, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00}, 63, 0},
    {0xEA, (uint8_t[]){0x97, 0x0C, 0x09, 0x09, 0x09, 0x78, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x9F, 0x31, 0x8B, 0xA8,
                       0x31, 0x75, 0x88, 0x88, 0x88, 0x88, 0x88, 0x9F, 0x20, 0x8B,
                       0xA8, 0x20, 0x64, 0x88, 0x88, 0x88,
                       0x88, 0x88, 0x23, 0x00, 0x00, 0x02, 0x71, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x80, 0x81, 0x00,
                       0x00, 0x00, 0x00}, 61, 0},
    {0xEF, (uint8_t[]){0xFF, 0xFF, 0x01}, 3, 0},
    {0x11, NULL, 0, 250},   // Sleep Out
    {0x29, NULL, 0, 50},    // Display On
};

/* ══════════════════════════════════════════════════
 *  Callback implementations
 * ══════════════════════════════════════════════════ */

static esp_err_t panel_fl7707_del(esp_lcd_panel_t *panel)
{
    fl7707_panel_t *fl7707 = (fl7707_panel_t *)panel->user_data;

    if (fl7707->reset_gpio_num >= 0) {
        gpio_reset_pin(fl7707->reset_gpio_num);
    }
    /* Delete the underlying DPI panel first */
    fl7707->del(panel);
    ESP_LOGD(TAG, "del fl7707 panel @%p", fl7707);
    free(fl7707);
    return ESP_OK;
}

static esp_err_t panel_fl7707_init(esp_lcd_panel_t *panel)
{
    fl7707_panel_t *fl7707 = (fl7707_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = fl7707->io;
    const fl7707_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;

    /* Pick init commands: custom or default */
    if (fl7707->init_cmds) {
        init_cmds = fl7707->init_cmds;
        init_cmds_size = fl7707->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(fl7707_lcd_init_cmd_t);
    }

    /* Send all init commands via DBI IO */
    for (int i = 0; i < init_cmds_size; i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd,
                                                      init_cmds[i].data,
                                                      init_cmds[i].data_bytes),
                            TAG, "send cmd 0x%02X failed", init_cmds[i].cmd);
        if (init_cmds[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
        }
    }

    /* Now call the underlying DPI panel init */
    ESP_RETURN_ON_ERROR(fl7707->init(panel), TAG, "DPI panel init failed");

    return ESP_OK;
}

static esp_err_t panel_fl7707_reset(esp_lcd_panel_t *panel)
{
    fl7707_panel_t *fl7707 = (fl7707_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = fl7707->io;

    if (fl7707->reset_gpio_num >= 0) {
        /* Hardware reset: assert → wait → de-assert → wait */
        gpio_set_level(fl7707->reset_gpio_num, fl7707->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(fl7707->reset_gpio_num, !fl7707->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(50));
    } else if (io) {
        /* Software reset fallback */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

static esp_err_t panel_fl7707_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    fl7707_panel_t *fl7707 = (fl7707_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = fl7707->io;

    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_STATE, TAG, "invalid panel IO");
    uint8_t command = invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0),
                        TAG, "send invert command failed");
    return ESP_OK;
}

static esp_err_t panel_fl7707_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    fl7707_panel_t *fl7707 = (fl7707_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = fl7707->io;
    uint8_t madctl_val = fl7707->madctl_val;

    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_STATE, TAG, "invalid panel IO");

    if (mirror_x) {
        madctl_val |= LCD_CMD_MX_BIT;
    } else {
        madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        madctl_val |= LCD_CMD_MV_BIT;
    } else {
        madctl_val &= ~LCD_CMD_MV_BIT;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL,
                                                  (uint8_t[]){madctl_val}, 1),
                        TAG, "send mirror command failed");
    fl7707->madctl_val = madctl_val;
    return ESP_OK;
}

static esp_err_t panel_fl7707_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    fl7707_panel_t *fl7707 = (fl7707_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = fl7707->io;

    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_STATE, TAG, "invalid panel IO");
    int command = on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0),
                        TAG, "send disp_on_off failed");
    return ESP_OK;
}

static esp_err_t panel_fl7707_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    fl7707_panel_t *fl7707 = (fl7707_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = fl7707->io;

    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_STATE, TAG, "invalid panel IO");
    int command = sleep ? LCD_CMD_SLPIN : LCD_CMD_SLPOUT;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0),
                        TAG, "send sleep command failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

#endif  // SOC_MIPI_DSI_SUPPORTED
