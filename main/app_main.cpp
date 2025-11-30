
#include "freertos/FreeRTOS.h"
#include <cstring>
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
constexpr auto diameter = 160;
constexpr auto rotation_length = std::numbers::pi * diameter / 1000;
constexpr auto idle_duration = 60s;
constexpr auto history_size = 128;
constexpr auto history_step = std::chrono::duration_cast<std::chrono::milliseconds>(25h) / history_size;

uint16_t rotation_count = 0;
double speed = 0;
double speed_max = 0;
uint16_t rotation_history[history_size] = {0};

std::unique_ptr<std::chrono::steady_clock::time_point> last_click;
button_t wake_up_button;
button_t sensor_button;
SSD1306_t dev;
std::unique_ptr<idf::esp_timer::ESPTimer> sleep_tm;
std::unique_ptr<idf::esp_timer::ESPTimer> history_tm;
bool f_show_data = false;

void show_history()
{
    const auto history_max = *std::max_element(rotation_history, rotation_history + history_size);
    const auto history_total = std::accumulate(rotation_history, rotation_history + history_size, 0);
    char buf[32];
    ESP_LOGI(TAG, "history_max=%u,history_total=%u", history_max, history_total);
    snprintf(buf, sizeof(buf), "24h=%u/%u", history_max, history_total);
    ssd1306_display_text(&dev, 2, buf, strlen(buf), false);
    if (history_max > 0)
    {
        constexpr auto size_y = 32;
        constexpr auto size_x = history_size;
        uint8_t history[size_x * size_y / 8 + 1];
        memset(history, 0, sizeof(history));
        auto put_pixel = [&history](uint8_t x, uint8_t y)
        {
            const auto bit_pos = y * size_x + x;
            const auto byte_pos = bit_pos / 8;
            const auto bit_mask = 0x80 >> (bit_pos % 8);
            history[byte_pos] |= bit_mask;
        };

        for (auto i = 0; i < history_size; i++)
        {
            if (rotation_history[i])
            {
                auto val_y = size_y - rotation_history[i] * (size_y - 1) / history_max;
                while (val_y < size_y)
                {
                    put_pixel(i, val_y);
                    val_y++;
                }
            }
        }

        ssd1306_bitmaps(&dev, 0, 64 - size_y, history, size_x, size_y, false);
    }
}
void show_data()
{
    char buf[32];
    const auto distance = rotation_count * rotation_length;
    snprintf(buf, sizeof(buf), "rot=%u/%.1g", rotation_count, distance);
    ssd1306_display_text(&dev, 0, buf, strlen(buf), false);
    snprintf(buf, sizeof(buf), "spd=%.3g/%.3g", speed, speed_max);
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
        rotation_count++;
        rotation_history[0]++;
        ESP_LOGI(TAG, "rotation_count=%d", rotation_count);
        auto now = std::chrono::steady_clock::now();
        if (last_click)
        {
            const auto diff = now - *last_click;
            const auto diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(diff).count();
            ESP_LOGI(TAG, "diff=%d", diff_ms);
            speed = rotation_length * 60 * 60 / diff_ms; // km/hour
            if (speed > speed_max)
            {
                speed_max = speed;
            }
        }
        last_click = std::make_unique<std::chrono::steady_clock::time_point>(now);

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
