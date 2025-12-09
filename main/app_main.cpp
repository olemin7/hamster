
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
#include "esp_adc/adc_oneshot.h"

using namespace std::chrono_literals;

static const char *TAG = "main";
constexpr auto RING_DIAMETER_MM = 200;
constexpr auto RING_LENGTH_MM = std::numbers::pi * RING_DIAMETER_MM;
constexpr auto STEP_PER_ROTATION = 2;
constexpr auto STEP_LENGTH_MM = RING_LENGTH_MM / STEP_PER_ROTATION;
constexpr auto MAX_SPEED_KMH = 10;
constexpr auto MIN_STEP_DURATION_MS = static_cast<uint64_t>(STEP_LENGTH_MM * (60 * 60 * 1000) / (MAX_SPEED_KMH * 1000 * 1000));
constexpr auto idle_duration = 60s;
constexpr auto history_size = 128;
constexpr auto history_step = std::chrono::duration_cast<std::chrono::milliseconds>(25h) / history_size;
constexpr auto step_to_km_koef = static_cast<float>(STEP_LENGTH_MM) / 1000 / 1000;
// constexpr auto history_step = 3s;
uint32_t steps_total = 0;
uint32_t steps_history[history_size] = {0};
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
    const auto history_max = *std::max_element(steps_history, steps_history + history_size);
    const auto history_total = std::accumulate(steps_history, steps_history + history_size, uint32_t{0});
    char buf[32];
    ESP_LOGI(TAG, "history_max=%u,history_total=%u", history_max, history_total);

    snprintf(buf, sizeof(buf), "24h=%0.3f/%0.3f", step_to_km_koef * history_max,
             step_to_km_koef * history_total);
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
            if (steps_history[i])
            {
                const auto val_y = size_y - steps_history[i] * (size_y - 1) / history_max;
                put_bar(i, val_y);
            }
        }

        ssd1306_bitmaps(&dev, 0, 64 - size_y, history, size_x, size_y, false);
    }
}
void show_data()
{
    char buf[32];

    const auto total_length = step_to_km_koef * steps_total;
    snprintf(buf, sizeof(buf), "tot=%.3f", total_length); // km
    ssd1306_display_text(&dev, 0, buf, strlen(buf), false);
    snprintf(buf, sizeof(buf), "kmh=%.3f/%.3f", speed, speed_max);
    ssd1306_display_text(&dev, 1, buf, strlen(buf), false);

    show_history();
}

void show_service()
{
    char buf[32];
    adc_oneshot_unit_handle_t adc_handle = nullptr;
    do // adc
    {
        constexpr auto ADC_4_2 = 3489;
        constexpr auto ADC_3_0 = 2557;
        int adc_raw;
        adc_oneshot_unit_init_cfg_t init_config1 = {
            .unit_id = ADC_UNIT_1,
        };

        if (ESP_OK != adc_oneshot_new_unit(&init_config1, &adc_handle))
        {
            ESP_LOGE(TAG, "ADC adc_oneshot_new_unit failed");
            break;
        }
        //-------------ADC1 Config---------------//
        adc_oneshot_chan_cfg_t config = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (ESP_OK != adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_2, &config))
        {
            ESP_LOGE(TAG, "ADC adc_oneshot_config_channel failed");
            break;
        }

        if (ESP_OK != adc_oneshot_read(adc_handle, ADC_CHANNEL_2, &adc_raw))
        {
            ESP_LOGE(TAG, "ADC read failed");
            break;
        }
        ESP_LOGI(TAG, "adc_raw=%d", adc_raw);
        auto voltage = 3.0 + (adc_raw - ADC_3_0) * 1.2 / (ADC_4_2 - ADC_3_0);
        snprintf(buf, sizeof(buf), "bat=%.2f", voltage); // km
        ssd1306_display_text(&dev, 0, buf, strlen(buf), false);
    } while (0);
    if (adc_handle)
    {
        adc_oneshot_del_unit(adc_handle);
    }
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
        ssd1306_clear_screen(&dev, 0);
        restart_timeout();
        show_data();
        break;
    case BUTTON_PRESSED_LONG:
        ssd1306_clear_screen(&dev, 0);
        show_service();
        restart_timeout();
        break;
    default: // NOTHING
        break;
    }
}

static void on_sensor_button(button_t *, button_state_t state)
{
    if (BUTTON_PRESSED == state)
    {

        const auto now = std::chrono::steady_clock::now();
        if (last_click.has_value())
        {
            const auto diff = now - last_click.value();
            const auto diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(diff).count();
            ESP_LOGI(TAG, "diff=%lld", diff_ms);
            last_click = now;
            // ESP_LOGI(TAG, " min=%" PRIu64, MIN_STEP_DURATION_MS);
            if (MIN_STEP_DURATION_MS > diff_ms)
            {
                ESP_LOGI(TAG, "too fast, min=%d", MIN_STEP_DURATION_MS);
                return;
            }
            speed = step_to_km_koef * 60 * 60 * 1000 / diff_ms; // km/hour
            if (speed > speed_max)
            {
                speed_max = speed;
            }
            ESP_LOGI(TAG, "speed=%f, speed_max=%f", speed, speed_max);
        }
        else
        {
            last_click = now;
        }

        steps_total++;
        steps_history[0]++;
        ESP_LOGI(TAG, "steps_total=%d, steps_history=%d ", steps_total, *steps_history);

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
        memmove(steps_history+1, steps_history , sizeof(steps_history) - sizeof(*steps_history));
        steps_history[0]=0; 
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
