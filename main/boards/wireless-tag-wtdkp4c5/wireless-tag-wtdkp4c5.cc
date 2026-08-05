#include "esp_h264_types.h"
#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "application.h"
#include "display/lcd_display.h"
// #include "display/no_display.h"
#include "button.h"
#include "config.h"

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"

// #include "esp_lcd_ek79007.h"
#include "display/esp_lcd_fl7707.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lvgl_port.h>
#include "esp_lcd_touch_gt911.h"

#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#include "freertos/task.h"
#include "esp_log.h"
#include "esp_h264_dec.h"  // Espressif H264 解码器头文件
#include "esp_h264_dec_sw.h"
#include "esp_heap_caps.h" // 用于分配 PSRAM 内存
#include "assets.h"
#include "esp_imgfx_color_convert.h" // ppa

// 如果使用 ESP32-P4 的硬件 PPA 转换 YUV 到 RGB，需要引入此头文件
#include "driver/ppa.h"

#define TAG "WirelessTagEsp32p4c5"

class MyMipiLcdDisplay : public MipiLcdDisplay {
private:
    TaskHandle_t m_video_task = nullptr;
    volatile bool m_stop_requested = false;

    // 【修改】不再保存文件路径，而是直接保存 assets 里的视频数据指针和大小
    const uint8_t* m_video_data_ptr = nullptr;
    size_t m_video_data_size = 0;

    // 播放任务入口（C 风格静态函数）
    static void VideoPlayTaskEntry(void* param) {
        auto* instance = static_cast<MyMipiLcdDisplay*>(param);
        instance->PlayVideoLoop();
        instance->m_video_task = nullptr;
        vTaskDelete(NULL);
    }

    // 实际的解码与播放循环
    void PlayVideoLoop() {
        if (!m_video_data_ptr || m_video_data_size == 0) {
            ESP_LOGE("MipiVideo", "Invalid video data pointer or size.");
            return;
        }

        // 1. 初始化 esp_h264 解码器
        esp_h264_dec_cfg_t dec_cfg = {
            // 根据 esp_h264 库的实际版本配置参数
            // 通常可以设置为默认配置
            // SW 解码器只输出 I420（YUV420 planar），必须设置 pic_type，否则创建失败
            .pic_type = ESP_H264_RAW_FMT_I420,
        };
        esp_h264_dec_handle_t dec_handle = nullptr;

        // 3. 【核心修复】根据芯片直接调用对应的创建函数
        esp_err_t ret = esp_h264_dec_sw_new(&dec_cfg, &dec_handle);


        if (ret != ESP_OK) {
            ESP_LOGE("MipiVideo", "Failed to create H264 decoder");
            return;
        }

        // 2. 申请缓冲区
        // 解码器输入流缓冲区
        const size_t in_buf_size = 4096 * 4;
        uint8_t* in_buf = (uint8_t*)heap_caps_malloc(in_buf_size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);

        // 假设视频分辨率（实际可以从解码器获取，这里以 800x480 为例）
        int video_width = 480;
        int video_height = 480;

        // RGB565 屏幕缓冲区，每个像素 2 字节
        size_t rgb_buf_size = video_width * video_height * 2;
        uint8_t* rgb_buf = (uint8_t*)heap_caps_malloc(rgb_buf_size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);

        if (!in_buf || !rgb_buf) {
            ESP_LOGE("MipiVideo", "Failed to allocate video buffers in PSRAM");
            free(in_buf);
            free(rgb_buf);
            esp_h264_dec_del(dec_handle);
            return;
        }

        ESP_LOGI("MipiVideo", "Start decoding pipeline from memory...");

        // 【新增】用来记录当前在内存视频数据中读取的偏移量（模拟文件指针）
        size_t mem_offset = 0;

        while (!m_stop_requested) {
            // 模拟 fread：从内存数据中“读取”一小段填充到输入缓冲区
            size_t remaining_bytes = m_video_data_size - mem_offset;
            size_t bytes_to_read = (remaining_bytes < in_buf_size) ? remaining_bytes : in_buf_size;

            if (bytes_to_read <= 0) {
                // 模拟 fseek(f, 0, SEEK_SET) 循环播放：把偏移量重置为 0
                mem_offset = 0;
                continue;
            }

            // 零拷贝或直接 memcpy 到输入缓冲区进行切片解码
            memcpy(in_buf, m_video_data_ptr + mem_offset, bytes_to_read);
            mem_offset += bytes_to_read; // 更新读取进度

            // 填充输入输出帧结构体
            esp_h264_dec_in_frame_t in_frame = {
                .raw_data = {
                    .buffer = in_buf,              // 填充到子结构体的 buffer 指针
                    .len  = (uint32_t)bytes_to_read,  // 填充到子结构体的 len 长度
                },
                .consume = 0,
                .dts = 0,
                .pts = 0
            };
            esp_h264_dec_out_frame_t out_frame = {};

            // 4. 【核心修复】直接使用通用的解码函数
            ret = esp_h264_dec_process(dec_handle, &in_frame, &out_frame);

            if (ret == ESP_OK && out_frame.out_size > 0 && out_frame.outbuf != nullptr) {
                // 3. 将解码出的 YUV420p 数据转换为 RGB565/RGB888
                // 注意：这里需要调用 YUV 转 RGB 的算法。如果是 P4 芯片，强烈推荐使用硬件 PPA：
                // 注册 PPA 客户端（SRM 类型）
                ppa_client_handle_t srm_client;
                ppa_client_config_t client_cfg = {
                    .oper_type = PPA_OPERATION_SRM,
                    .max_pending_trans_num = 1,  // 阻塞模式设为1即可
                    .data_burst_length = PPA_DATA_BURST_LENGTH_128,
                };
                ESP_ERROR_CHECK(ppa_register_client(&client_cfg, &srm_client));

                // 4. 将 RGB 数据刷写到屏幕
                // 这里调用你基类 MipiLcdDisplay 的绘制函数，例如：
                // this->DrawBitmap(0, 0, width, height, rgb_buf);

                // 5. 控制帧率 (例如 30fps = 33ms)
                // 实际项目中推荐配合硬件 VSYNC 中断或高精度定时器来控制帧率
                vTaskDelay(pdMS_TO_TICKS(33)); // 帧率控制
            } else if (ret != ESP_OK) {
                ESP_LOGW("MipiVideo", "Decoder processed with error code: %d", ret);
            }
        }

        // 6. 释放资源
        free(in_buf);
        free(rgb_buf);
        esp_h264_dec_del(dec_handle);
        ESP_LOGI("MipiVideo", "Video playback stopped & resources freed.");
    }

public:
    using MipiLcdDisplay::MipiLcdDisplay;

    // 析构时确保安全释放任务
    ~MyMipiLcdDisplay() {
        StopVideo();
    }

    // 停止当前播放的视频
    void StopVideo() {
        if (m_video_task != nullptr) {
            m_stop_requested = true;
            ESP_LOGI("MipiVideo", "Waiting for video task to exit...");
            // 等待后台任务自己跑完循环并销毁
            while (m_video_task != nullptr) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }

    void SetVideo(const char* video) override {
        ESP_LOGI("MipiVideo", "Play video requested: %s", video);

        // 1. 先安全停止正在播放的视频
        StopVideo();

        // 2. 更新视频路径和控制信号
        void* temp_ptr = nullptr;
        size_t temp_size = 0;

        auto& assets = Assets::GetInstance();

        // 调用小智的 C++ 资源获取 API
        if (!assets.GetAssetData(video, temp_ptr, temp_size)) {
            ESP_LOGE("MipiVideo", "Failed to find asset: %s in assets.bin", video);
            return;
        }

        m_video_data_ptr = static_cast<const uint8_t*>(temp_ptr);
        m_video_data_size = temp_size;

        // 3. 创建异步 FreeRTOS 任务进行后台解码，防止阻塞主 UI 线程
        // H.264 解码比较吃栈空间，建议分配 8KB 以上，并绑定到 Core 1 运行
        xTaskCreatePinnedToCore(
            VideoPlayTaskEntry,
            "video_play_task",
            1024 * 8,
            this,
            5, // 优先级需要根据你的 UI 线程进行微调
            &m_video_task,
            1  // 绑定到核心 1
        );
    }
};

class WirelessTagEsp32p4c5 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    LcdDisplay *display_;

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    static esp_err_t bsp_enable_dsi_phy_power(void) {
#if MIPI_DSI_PHY_PWR_LDO_CHAN > 0
        // Turn on the power for MIPI DSI PHY, so it can go from "No Power" state to "Shutdown" state
        static esp_ldo_channel_handle_t phy_pwr_chan = NULL;
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
            .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
        };
        esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan);
        ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
#endif // BSP_MIPI_DSI_PHY_PWR_LDO_CHAN > 0

        return ESP_OK;
    }

    void InitializeLCD() {
        bsp_enable_dsi_phy_power();
        esp_lcd_panel_io_handle_t io = NULL;
        esp_lcd_panel_handle_t disp_panel = NULL;

        esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
        esp_lcd_dsi_bus_config_t bus_config = {
            .bus_id = 0,
            .num_data_lanes = 2,
            .lane_bit_rate_mbps = 900,
        };
        esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus);

        ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
        // we use DBI interface to send LCD commands and parameters
        esp_lcd_dbi_io_config_t dbi_config = FL7707_PANEL_IO_DBI_CONFIG();
        esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io);

        esp_lcd_dpi_panel_config_t dpi_config = {
            .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
            .dpi_clock_freq_mhz = 52,
            .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
            .num_fbs = 1,
            .video_timing = {
                .h_size = 1024,
                .v_size = 600,
                .hsync_pulse_width = 10,
                .hsync_back_porch = 160,
                .hsync_front_porch = 160,
                .vsync_pulse_width = 1,
                .vsync_back_porch = 23,
                .vsync_front_porch = 12,
            },
            .flags = {
                .use_dma2d = true,
            },
        };
        fl7707_vendor_config_t vendor_config = {
            .mipi_config = {
                .dsi_bus = mipi_dsi_bus,
                .dpi_config = &dpi_config,
            },
        };

        const esp_lcd_panel_dev_config_t lcd_dev_config = {
            .reset_gpio_num = PIN_NUM_LCD_RST,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
            .flags = {
                .reset_active_high = false,
            },
            .vendor_config = &vendor_config,
        };
        esp_lcd_new_panel_fl7707(io, &lcd_dev_config, &disp_panel);
        esp_lcd_panel_reset(disp_panel);
        esp_lcd_panel_init(disp_panel);

        display_ = new MyMipiLcdDisplay(io, disp_panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                       DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#if 0
        lv_display_t *disp = lv_display_get_default();
        if (disp) {
            lv_disp_set_rotation(disp, LV_DISPLAY_ROTATION_180);
            ESP_LOGI(TAG, "Display rotated 180 degrees");
        } else {
            ESP_LOGE(TAG, "Failed to get default display for rotation");
        }
#endif
    }

    void InitializeTouch()
    {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_21,
            .levels = {
                .reset = 1,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = DISPLAY_SWAP_XY,
                .mirror_x = DISPLAY_MIRROR_X,
                .mirror_y = DISPLAY_MIRROR_Y,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 16,
            .flags =
            {
                .disable_control_phase = 1,
            }
	    };
        tp_io_config.scl_speed_hz = 400 * 1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp));
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            // During startup (before connected), pressing BOOT button enters Wi-Fi config mode without reboot
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeSdCard() {
#if SDCARD_SDMMC_ENABLED
        sd_pwr_ctrl_handle_t sd_ldo = NULL;
        sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = 4 };
        if (sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &sd_ldo) == ESP_OK) {
            ESP_LOGI(TAG, "SD LDO channel 4 enabled");
        } else {
            ESP_LOGW(TAG, "Failed to enable SD LDO channel 4");
        }
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        // Map pins via GPIO matrix if needed
        slot_config.clk = SDCARD_SDMMC_CLK_PIN;
        slot_config.cmd = SDCARD_SDMMC_CMD_PIN;
        slot_config.d0 = SDCARD_SDMMC_D0_PIN;
        slot_config.width = SDCARD_SDMMC_BUS_WIDTH;
        if (SDCARD_SDMMC_BUS_WIDTH == 4) {
            slot_config.d1 = SDCARD_SDMMC_D1_PIN;
            slot_config.d2 = SDCARD_SDMMC_D2_PIN;
            slot_config.d3 = SDCARD_SDMMC_D3_PIN;
        }

        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 0,
            .disk_status_check_enable = true,
        };
        sdmmc_card_t* card;
        host.pwr_ctrl_handle = sd_ldo;
        esp_err_t ret = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
        if (ret == ESP_OK) {
            sdmmc_card_print_info(stdout, card);
            ESP_LOGI(TAG, "SD card mounted at %s (SDMMC)", SDCARD_MOUNT_POINT);
        } else {
            ESP_LOGW(TAG, "Failed to mount SD card (SDMMC): %s", esp_err_to_name(ret));
        }
#elif SDCARD_SDSPI_ENABLED
        sd_pwr_ctrl_handle_t sd_ldo = NULL;
        sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = 4 };
        if (sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &sd_ldo) == ESP_OK) {
            ESP_LOGI(TAG, "SD LDO channel 4 enabled");
        } else {
            ESP_LOGW(TAG, "Failed to enable SD LDO channel 4");
        }
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = SDCARD_SPI_MOSI,
            .miso_io_num = SDCARD_SPI_MISO,
            .sclk_io_num = SDCARD_SPI_SCLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4000,
        };
        ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_initialize((spi_host_device_t)SDCARD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));
        sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_config.gpio_cs = SDCARD_SPI_CS;
        slot_config.host_id = (spi_host_device_t)SDCARD_SPI_HOST;

        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 0,
            .disk_status_check_enable = true,
        };
        sdmmc_card_t* card;
        host.pwr_ctrl_handle = sd_ldo;
        esp_err_t ret = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
        if (ret == ESP_OK) {
            sdmmc_card_print_info(stdout, card);
            ESP_LOGI(TAG, "SD card mounted at %s (SDSPI)", SDCARD_MOUNT_POINT);
        } else {
            ESP_LOGW(TAG, "Failed to mount SD card (SDSPI): %s", esp_err_to_name(ret));
        }
#else
        ESP_LOGI(TAG, "SD card disabled (enable SDCARD_SDMMC_ENABLED or SDCARD_SDSPI_ENABLED)");
#endif
    }

public:
    WirelessTagEsp32p4c5() :
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializeCodecI2c();
        InitializeLCD();
        // InitializeTouch();
        InitializeSdCard();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_,
            I2C_NUM_1,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display *GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

};

DECLARE_BOARD(WirelessTagEsp32p4c5);
