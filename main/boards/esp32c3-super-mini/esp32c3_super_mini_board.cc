#include "application.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "display/oled_display.h"
#include "led/single_led.h"
#include "light_controller.h"
#include "mcp_server.h"
#include "power_save_timer.h"
#include "press_to_talk_mcp_tool.h"
#include "settings.h"
#include "wifi_board.h"

#include <cstring>
#include <driver/i2c_master.h>
#include <esp_efuse_table.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>

#define TAG "Esp32c3SuperMiniBoard"

// lv_line 只保存指针，所以点坐标数组必须是静态生命周期
static const lv_point_precise_t kAngryBrowLeft[]  = {{0, 0}, {20, 6}};   // 内低外高
static const lv_point_precise_t kAngryBrowRight[] = {{0, 6}, {20, 0}};
static const lv_point_precise_t kMouthSmile[]     = {{0, 0}, {5, 4}, {11, 4}, {16, 0}};  // ∪
static const lv_point_precise_t kMouthFrown[]     = {{0, 4}, {5, 0}, {11, 0}, {16, 4}};  // ∩
static const lv_point_precise_t kMouthFlat[]      = {{0, 0}, {16, 0}};

// 本板定制：隐藏顶部栏 + 可切换表情的动画眼睛
class SuperMiniOledDisplay : public OledDisplay {
public:
    enum class EyeMood {
        Neutral,
        Happy,
        Angry,
        Sad,
        Crying,
        Sleepy,
        Thinking,
        Surprised,
        Winking,
    };

    SuperMiniOledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, bool mirror_x, bool mirror_y)
        : OledDisplay(panel_io, panel, width, height, mirror_x, mirror_y) {}

    void SetupUI() override {
        OledDisplay::SetupUI();
        if (height_ != 64) {
            return;
        }

        DisplayLockGuard lock{this};

        // 隐藏顶部图标栏/状态栏/原来的 FontAwesome 表情
        if (top_bar_ != nullptr) {
            lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        if (status_bar_ != nullptr) {
            lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emotion_label_ != nullptr) {
            lv_obj_add_flag(emotion_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (content_ != nullptr) {
            lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
        }

        left_eye_ = CreateShape(24, 34);
        right_eye_ = CreateShape(24, 34);
        tear_left_ = CreateShape(5, 8);
        tear_right_ = CreateShape(5, 8);
        left_brow_ = CreateLine(kAngryBrowLeft, 2);
        right_brow_ = CreateLine(kAngryBrowRight, 2);
        mouth_ = CreateLine(kMouthFlat, 2);

        zzz_ = lv_label_create(content_);
        lv_obj_add_flag(zzz_, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_style_text_color(zzz_, lv_color_black(), 0);
        lv_label_set_text(zzz_, "z");

        ApplyMood(EyeMood::Neutral);
    }

    // 服务端/LLM 的 emotion 字段最终会调用到这里
    void SetEmotion(const char* emotion) override {
        SetMood(ParseMood(emotion));
    }

    // 也可以在自己的代码里直接调用（比如按键、MCP 工具）
    void SetMood(EyeMood mood) {
        DisplayLockGuard lock(this);
        ApplyMood(mood);
    }

private:
    static constexpr int kEyeOffsetX = 28;

    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* left_brow_ = nullptr;
    lv_obj_t* right_brow_ = nullptr;
    lv_obj_t* mouth_ = nullptr;
    lv_obj_t* tear_left_ = nullptr;
    lv_obj_t* tear_right_ = nullptr;
    lv_obj_t* zzz_ = nullptr;

    EyeMood mood_ = EyeMood::Neutral;

    static EyeMood ParseMood(const char* emotion) {
        if (emotion == nullptr) {
            return EyeMood::Neutral;
        }
        if (!strcmp(emotion, "happy") || !strcmp(emotion, "laughing") || !strcmp(emotion, "funny") ||
            !strcmp(emotion, "loving") || !strcmp(emotion, "delicious") ||
            !strcmp(emotion, "confident") || !strcmp(emotion, "embarrassed")) {
            return EyeMood::Happy;
        }
        if (!strcmp(emotion, "angry")) {
            return EyeMood::Angry;
        }
        if (!strcmp(emotion, "sad")) {
            return EyeMood::Sad;
        }
        if (!strcmp(emotion, "crying")) {
            return EyeMood::Crying;
        }
        if (!strcmp(emotion, "sleepy") || !strcmp(emotion, "relaxed") ||
            !strcmp(emotion, "sleeping")) {
            return EyeMood::Sleepy;
        }
        if (!strcmp(emotion, "thinking") || !strcmp(emotion, "confused") ||
            !strcmp(emotion, "hesitate") || !strcmp(emotion, "hesitating")) {
            return EyeMood::Thinking;
        }
        if (!strcmp(emotion, "surprised") || !strcmp(emotion, "shocked") ||
            !strcmp(emotion, "panic")) {
            return EyeMood::Surprised;
        }
        if (!strcmp(emotion, "winking")) {
            return EyeMood::Winking;
        }
        return EyeMood::Neutral;  // 未知情绪一律回到普通眼睛
    }

    lv_obj_t* CreateShape(int w, int h) {
        lv_obj_t* obj = lv_obj_create(content_);
        lv_obj_remove_style_all(obj);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(obj, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_size(obj, w, h);
        lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(obj, lv_color_black(), 0);   // 看不见就换 lv_color_white()
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        return obj;
    }

    lv_obj_t* CreateLine(const lv_point_precise_t* points, uint32_t count) {
        lv_obj_t* line = lv_line_create(content_);
        lv_obj_add_flag(line, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_style_line_width(line, 3, 0);
        lv_obj_set_style_line_color(line, lv_color_black(), 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_line_set_points(line, points, count);
        return line;
    }

    static void HeightCb(void* var, int32_t value) {
        lv_obj_set_height(static_cast<lv_obj_t*>(var), value);
    }
    static void TranslateXCb(void* var, int32_t value) {
        lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(var), value, 0);
    }
    static void TranslateYCb(void* var, int32_t value) {
        lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(var), value, 0);
    }

    void ShapeEye(lv_obj_t* eye, int x, int y, int w, int h) {
        lv_obj_set_size(eye, w, h);
        lv_obj_align(eye, LV_ALIGN_CENTER, x, y);
    }
    void ShapeEyes(int w, int h, int y) {
        ShapeEye(left_eye_, -kEyeOffsetX, y, w, h);
        ShapeEye(right_eye_, kEyeOffsetX, y, w, h);
    }
    void SetMouth(const lv_point_precise_t* points, uint32_t count, int y) {
        lv_line_set_points(mouth_, points, count);
        lv_obj_align(mouth_, LV_ALIGN_CENTER, 0, y);
        lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
    }

    void StartBlink(lv_obj_t* eye, int open_height) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, eye);
        lv_anim_set_values(&a, open_height, 2);
        lv_anim_set_duration(&a, 90);
        lv_anim_set_reverse_duration(&a, 90);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_repeat_delay(&a, 3500);
        lv_anim_set_exec_cb(&a, HeightCb);
        lv_anim_start(&a);
    }

    void StartLook(int amplitude, int duration_ms = 1400) {
        for (lv_obj_t* eye : {left_eye_, right_eye_}) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, eye);
            lv_anim_set_values(&a, -amplitude, amplitude);
            lv_anim_set_duration(&a, duration_ms);
            lv_anim_set_reverse_duration(&a, duration_ms);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_repeat_delay(&a, 1800);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
            lv_anim_set_exec_cb(&a, TranslateXCb);
            lv_anim_start(&a);
        }
    }

    void StartBreathe(lv_obj_t* eye) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, eye);
        lv_anim_set_values(&a, 6, 11);
        lv_anim_set_duration(&a, 2400);
        lv_anim_set_reverse_duration(&a, 2400);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, HeightCb);
        lv_anim_start(&a);
    }

    void StartTear(lv_obj_t* tear, int delay_ms, int duration_ms, int gap_ms) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, tear);
        lv_anim_set_values(&a, 0, 22);
        lv_anim_set_delay(&a, delay_ms);
        lv_anim_set_duration(&a, duration_ms);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_repeat_delay(&a, gap_ms);
        lv_anim_set_exec_cb(&a, TranslateYCb);
        lv_anim_start(&a);
    }

    void StartTears(int duration_ms, int gap_ms) {
        lv_obj_align(tear_left_, LV_ALIGN_CENTER, -kEyeOffsetX + 7, 16);
        lv_obj_align(tear_right_, LV_ALIGN_CENTER, kEyeOffsetX - 7, 16);
        lv_obj_remove_flag(tear_left_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(tear_right_, LV_OBJ_FLAG_HIDDEN);
        StartTear(tear_left_, 0, duration_ms, gap_ms);
        StartTear(tear_right_, duration_ms / 2, duration_ms, gap_ms);
    }

    void StartZzz() {
        lv_obj_align(zzz_, LV_ALIGN_CENTER, kEyeOffsetX + 8, -18);
        lv_obj_remove_flag(zzz_, LV_OBJ_FLAG_HIDDEN);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, zzz_);
        lv_anim_set_values(&a, 0, -14);
        lv_anim_set_duration(&a, 1600);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_repeat_delay(&a, 300);
        lv_anim_set_exec_cb(&a, TranslateYCb);
        lv_anim_start(&a);
    }

    void ApplyMood(EyeMood mood) {
        if (left_eye_ == nullptr) {
            return;
        }
        mood_ = mood;

        // 停掉旧动画并复位
        lv_anim_delete(left_eye_, nullptr);
        lv_anim_delete(right_eye_, nullptr);
        lv_anim_delete(tear_left_, nullptr);
        lv_anim_delete(tear_right_, nullptr);
        lv_anim_delete(zzz_, nullptr);
        lv_obj_set_style_translate_x(left_eye_, 0, 0);
        lv_obj_set_style_translate_x(right_eye_, 0, 0);
        lv_obj_set_style_translate_y(left_eye_, 0, 0);
        lv_obj_set_style_translate_y(right_eye_, 0, 0);
        lv_obj_set_style_translate_x(zzz_, 0, 0);
        lv_obj_set_style_translate_y(zzz_, 0, 0);

        lv_obj_add_flag(left_brow_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(right_brow_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tear_left_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tear_right_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(zzz_, LV_OBJ_FLAG_HIDDEN);

        switch (mood) {
            case EyeMood::Happy:  // 眯眼 + 微笑
                ShapeEyes(22, 12, 0);
                SetMouth(kMouthSmile, 4, 18);
                StartLook(2);
                break;

            case EyeMood::Angry:  // 眯眼 + 倒八眉毛 + 平嘴
                ShapeEyes(24, 16, 2);
                lv_obj_align(left_brow_, LV_ALIGN_CENTER, -kEyeOffsetX, -14);
                lv_obj_align(right_brow_, LV_ALIGN_CENTER, kEyeOffsetX, -14);
                lv_obj_remove_flag(left_brow_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(right_brow_, LV_OBJ_FLAG_HIDDEN);
                SetMouth(kMouthFlat, 2, 20);
                StartBlink(left_eye_, 16);
                StartBlink(right_eye_, 16);
                break;

            case EyeMood::Sad:  // 下垂嘴角 + 慢慢掉泪
                ShapeEyes(22, 24, 0);
                SetMouth(kMouthFrown, 4, 20);
                StartTears(1200, 700);
                break;

            case EyeMood::Crying:  // 更眯 + 快掉泪
                ShapeEyes(20, 12, -2);
                SetMouth(kMouthFrown, 4, 16);
                StartTears(800, 250);
                break;

            case EyeMood::Sleepy:  // 闭眼 + 呼吸 + z
                ShapeEyes(24, 6, 3);
                StartBreathe(left_eye_);
                StartBreathe(right_eye_);
                StartZzz();
                break;

            case EyeMood::Thinking:  // 一只眼眯、一只眼大 + 缓慢左右看
                ShapeEye(left_eye_, -kEyeOffsetX, -8, 18, 10);
                ShapeEye(right_eye_, kEyeOffsetX, 4, 20, 24);
                SetMouth(kMouthFlat, 2, 20);
                StartLook(6, 2000);
                break;

            case EyeMood::Surprised:  // 瞪大
                ShapeEyes(26, 38, 0);
                SetMouth(kMouthFlat, 2, 22);
                break;

            case EyeMood::Winking:  // 眨眼
                ShapeEyes(24, 34, 0);
                ShapeEye(right_eye_, kEyeOffsetX, 2, 24, 6);
                SetMouth(kMouthSmile, 4, 18);
                break;

            case EyeMood::Neutral:
            default:
                ShapeEyes(24, 34, 0);
                StartBlink(left_eye_, 34);
                StartBlink(right_eye_, 34);
                StartLook(4);
                break;
        }
    }
};

class Esp32c3SuperMiniBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    PowerSaveTimer* power_save_timer_ = nullptr;
    PressToTalkMcpTool* press_to_talk_tool_ = nullptr;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(160, 300);
        power_save_timer_->OnEnterSleepMode([this]() { GetDisplay()->SetPowerSaveMode(true); });
        power_save_timer_->OnExitSleepMode([this]() { GetDisplay()->SetPowerSaveMode(false); });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));

        // Print I2C bus info
        if (i2c_master_probe(codec_i2c_bus_, 0x18, 1000) != ESP_OK) {
            while (true) {
                ESP_LOGE(TAG,
                         "Failed to probe I2C bus, please check if you have installed the correct "
                         "firmware");
                vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
        }
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags =
                {
                    .dc_low_on_data = 0,
                    .disable_control_phase = 0,
                },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(codec_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new SuperMiniOledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            if (!press_to_talk_tool_ || !press_to_talk_tool_->IsPressToTalkEnabled()) {
                app.ToggleChatState();
            }
        });
        boot_button_.OnPressDown([this]() {
            if (power_save_timer_) {
                power_save_timer_->WakeUp();
            }
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StartListening();
            }
        });
        boot_button_.OnPressUp([this]() {
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StopListening();
            }
        });
    }

    void InitializeTools() {
        press_to_talk_tool_ = new PressToTalkMcpTool();
        press_to_talk_tool_->Initialize();
        static LightController light(LIGHT_GPIO, LIGHT_OUTPUT_INVERT);
    }

public:
    Esp32c3SuperMiniBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeCodecI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializePowerSaveTimer();
        InitializeTools();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(Esp32c3SuperMiniBoard);
