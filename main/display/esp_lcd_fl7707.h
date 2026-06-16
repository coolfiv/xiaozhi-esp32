/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file
 * @brief ESP LCD: FL7707 (NV3052 variant)
 */

#pragma once

#include <stdint.h>
#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_mipi_dsi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LCD panel initialization commands.
 */
typedef struct {
    int cmd;                /*<! The specific LCD command */
    const void *data;       /*<! Buffer that holds the command specific data */
    size_t data_bytes;      /*<! Size of `data` in memory, in bytes */
    unsigned int delay_ms;  /*<! Delay in milliseconds after this command */
} fl7707_lcd_init_cmd_t;

/**
 * @brief LCD panel vendor configuration.
 *
 * @note  Pass to `esp_lcd_panel_dev_config_t::vendor_config`.
 */
typedef struct {
    const fl7707_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        esp_lcd_dsi_bus_handle_t dsi_bus;
        const esp_lcd_dpi_panel_config_t *dpi_config;
        uint8_t lane_num;
    } mipi_config;
} fl7707_vendor_config_t;

/**
 * @brief Create LCD panel for model FL7707
 */
esp_err_t esp_lcd_new_panel_fl7707(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel);

/* ───── 便捷配置宏 ───── */

#define FL7707_PANEL_BUS_DSI_2CH_CONFIG()               \
    {                                                     \
        .bus_id = 0,                                      \
        .num_data_lanes = 2,                              \
        .lane_bit_rate_mbps = 800,                        \
    }

#define FL7707_PANEL_IO_DBI_CONFIG()    \
    {                                     \
        .virtual_channel = 0,             \
        .lcd_cmd_bits = 8,                \
        .lcd_param_bits = 8,              \
    }

#define FL7707_720_720_PANEL_60HZ_DPI_CONFIG(px_format) \
    {                                                      \
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,       \
        .dpi_clock_freq_mhz = 50,                          \
        .virtual_channel = 0,                              \
        .pixel_format = px_format,                         \
        .num_fbs = 1,                                      \
        .video_timing = {                                  \
            .h_size = 720,                                 \
            .v_size = 720,                                 \
            .hsync_back_porch = 120,                       \
            .hsync_pulse_width = 60,                       \
            .hsync_front_porch = 106,                      \
            .vsync_back_porch = 20,                        \
            .vsync_pulse_width = 4,                        \
            .vsync_front_porch = 20,                       \
        },                                                 \
        .flags.use_dma2d = true,                           \
    }

#ifdef __cplusplus
}
#endif

#endif  // SOC_MIPI_DSI_SUPPORTED
