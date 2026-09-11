#ifndef __LIGHT_CONTROLLER_H__
#define __LIGHT_CONTROLLER_H__

#include "mcp_server.h"
#include "settings.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>

// 通用灯光亮度控制器（非屏幕背光）
// 通过 LEDC 在指定 GPIO 上输出 PWM 实现 0-100 的亮度调节，
// 并注册 MCP 工具 self.light.get_brightness / self.light.set_brightness。
class LightController {
private:
    gpio_num_t gpio_num_;
    int brightness_ = 0;

public:
    LightController(gpio_num_t gpio_num, bool output_invert = false, uint32_t freq_hz = 5000)
        : gpio_num_(gpio_num) {
        if (gpio_num_ == GPIO_NUM_NC) {
            ESP_LOGW("LightController", "LightController initialized with GPIO_NUM_NC, light will not function");
            return;
        }

        ledc_timer_config_t light_timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_1,
            .freq_hz = freq_hz,
            .clk_cfg = LEDC_AUTO_CLK,
            .deconfigure = false,
        };
        ESP_ERROR_CHECK(ledc_timer_config(&light_timer));

        ledc_channel_config_t light_channel = {
            .gpio_num = gpio_num_,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_1,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_1,
            .duty = 0,
            .hpoint = 0,
            .flags = {
                .output_invert = output_invert,
            },
        };
        ESP_ERROR_CHECK(ledc_channel_config(&light_channel));

        // 从设置中恢复上次的亮度，默认关闭
        Settings settings("light");
        SetBrightness(settings.GetInt("brightness", 0));

        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.light.get_brightness",
            "Get the brightness of the light (0-100).",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                return brightness_;
            });

        mcp_server.AddTool("self.light.set_brightness",
            "Set the brightness of the light (0-100). 0 turns the light off and 100 is the brightest.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                SetBrightness(properties["brightness"].value<int>());
                return true;
            });
    }

    void SetBrightness(int brightness) {
        if (brightness < 0) {
            brightness = 0;
        } else if (brightness > 100) {
            brightness = 100;
        }
        brightness_ = brightness;

        if (gpio_num_ != GPIO_NUM_NC) {
            // LEDC 分辨率为 10bit，100% 对应 1023
            uint32_t duty = (1023 * brightness_) / 100;
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        }

        Settings settings("light", true);
        settings.SetInt("brightness", brightness_);
    }

    inline int brightness() const { return brightness_; }
};

#endif // __LIGHT_CONTROLLER_H__
