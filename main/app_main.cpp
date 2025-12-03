
#include "freertos/FreeRTOS.h"
#include <cstring>
#include <optional>
#include "sdkconfig.h"
#include <esp_log.h>
#include <button.h>
#include <numbers>
#include <numeric>
#include <algorithm>
#include "esp_timer_cxx.hpp"
#include "ssd1306.h"
#include "font8x8_basic.h"

using namespace std::chrono_literals;

static const char *TAG = "main";
constexpr auto diameter_mm = 200;
constexpr auto rotation_length_mm = std::numbers::pi * diameter_mm;
constexpr auto step_per_rotation = 2;
constexpr auto idle_duration = 60s;
constexpr auto history_size = 128;
// constexpr auto history_step = std::chrono::duration_cast<std::chrono::milliseconds>(25h) / history_size;
constexpr auto history_step = 3s;
uint32_t rotation_total = 0;
uint32_t rotation_history[history_size] = {0};
double speed = 0;
double speed_max = 0;

std::optional<std::chrono::steady_clock::time_point> last_click;
button_t wake_up_button;
button_t sensor_button;
SSD1306_t dev;
std::unique_ptr<idf::esp_timer::ESPTimer> sleep_tm;
std::unique_ptr<idf::esp_timer::ESPTimer> history_tm;
bool f_show_data = false;

void show_history()
{
    const auto history_max = *std::max_element(rotation_history, rotation_history + history_size);
    const auto history_total = std::accumulate(rotation_history, rotation_history + history_size, uint32_t{0});
    char buf[32];
    ESP_LOGI(TAG, "history_max=%f,history_total=%f", history_max, history_total);
    snprintf(buf, sizeof(buf), "24h=%0.3f/%0.3f", static_cast<float>(rotation_length_mm * history_max) * step_per_rotation / 1000 / 1000,
             static_cast<float>(rotation_length_mm * history_total) * step_per_rotation / 1000 / 1000);
    ssd1306_display_text(&dev, 2, buf, strlen(buf), false);
    if (history_max > 0)
    {
        constexpr auto size_y = 37;
        constexpr auto size_x = history_size;
        uint8_t history[size_x * size_y / 8 + 1];
        memset(history, 0, sizeof(history));
        // auto put_pixel = [&history](uint8_t x, uint8_t y)
        // {
        //     const auto bit_pos = y * size_x + x;
        //     const auto byte_pos = bit_pos / 8;
        //     const auto bit_mask = 0x80 >> (bit_pos % 8);
        //     history[byte_pos] |= bit_mask;
        // };
        auto put_bar = [&](uint8_t x, uint8_t y)
        {
            auto bit_pos = y * size_x + x;
            constexpr auto bit_total = size_x * size_y;
            while (bit_total > bit_pos)
            {
                const auto byte_pos = bit_pos / 8;
                const auto bit_mask = 0x80 >> (bit_pos % 8);
                history[byte_pos] |= bit_mask;
                bit_pos += size_x;
            }
        };

        for (auto i = 0; i < history_size; i++)
        {
            if (rotation_history[i])
            {
                const auto val_y = size_y - rotation_history[i] * (size_y - 1) / history_max;
                put_bar(i, val_y);
            }
        }

        ssd1306_bitmaps(&dev, 0, 64 - size_y, history, size_x, size_y, false);
    }
}
void show_data()
{
    char buf[32];
    const auto total_length = rotation_length_mm * rotation_total / step_per_rotation / 1000 / 1000;
    snprintf(buf, sizeof(buf), "tot=%.3f", total_length); // km
    ssd1306_display_text(&dev, 0, buf, strlen(buf), false);
    snprintf(buf, sizeof(buf), "kmh=%.3f/%.3f", speed, speed_max);
    ssd1306_display_text(&dev, 1, buf, strlen(buf), false);

    show_history();
}

void restart_timeout()
{
    f_show_data = true;
    sleep_tm = std::make_unique<idf::esp_timer::ESPTimer>([]()
                                                          {
                                                              f_show_data = false;
                                                              // ssd1306_clear_screen(&dev, false);
                                                              ssd1306_fadeout(&dev); });
    sleep_tm->start(idle_duration);
}

static void on_wake_up_button(button_t *, button_state_t state)
{
    ESP_LOGI(TAG, " button %d", state);
    switch (state)
    {
    case BUTTON_PRESSED:
        // WAKE UP
        restart_timeout();
        show_data();
        break;
    case BUTTON_PRESSED_LONG:
        break;
    default: // NOTHING
        break;
    }
}

static void on_sensor_button(button_t *, button_state_t state)
{
    if (BUTTON_PRESSED == state)
    {
        constexpr auto length_by_step = rotation_length_mm / step_per_rotation;
        rotation_total++;
        rotation_history[0]++;
        ESP_LOGI(TAG, "rotation_total=%d, rotation_history=%d ", rotation_total, *rotation_history);
        auto now = std::chrono::steady_clock::now();
        if (last_click.has_value())
        {
            const auto diff = now - last_click.value();
            const auto diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(diff).count();
            ESP_LOGI(TAG, "diff=%lld", diff_ms);
            speed = rotation_length_mm * 60 * 60 / diff_ms / 1000; // km/hour
            if (speed > speed_max)
            {
                speed_max = speed;
            }
        }
        last_click = now;

        if (f_show_data)
        {
            restart_timeout();
            show_data();
        }
    }
}

void init()
{
    ESP_LOGI(TAG, "init");
    wake_up_button.gpio = static_cast<gpio_num_t>(CONFIG_WAKE_UP_BTN);
    wake_up_button.pressed_level = 0;
    wake_up_button.internal_pull = true;
    wake_up_button.autorepeat = false;
    wake_up_button.callback = on_wake_up_button;
    ESP_ERROR_CHECK(button_init(&wake_up_button));

    sensor_button.gpio = static_cast<gpio_num_t>(CONFIG_SENSOR_BTN);
    sensor_button.pressed_level = 0;
    sensor_button.internal_pull = true;
    sensor_button.autorepeat = false;
    sensor_button.callback = on_sensor_button;
    ESP_ERROR_CHECK(button_init(&sensor_button));

    i2c_master_init(&dev, CONFIG_SDA_GPIO, CONFIG_SCL_GPIO, CONFIG_RESET_GPIO);
#if CONFIG_FLIP
    dev._flip = true;
    ESP_LOGW(TAG, "Flip upside down");
#endif
    ESP_LOGI(TAG, "Panel is 128x64");
    ssd1306_init(&dev, 128, 64);
    ssd1306_clear_screen(&dev, false);
    ssd1306_contrast(&dev, 0x0);

    history_tm = std::make_unique<idf::esp_timer::ESPTimer>([]()
                                                            {
                                    
        memmove(rotation_history+1, rotation_history , sizeof(rotation_history) - sizeof(*rotation_history));
        rotation_history[0]=0; 
        if(f_show_data){
            show_history();
        } });
    history_tm->start_periodic(history_step);
    restart_timeout();
    show_data();
}

#define I2C_HOST I2C_NUM_0
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    init();
    ESP_LOGI(TAG, "[APP] done");
}
