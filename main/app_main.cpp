
#include "freertos/FreeRTOS.h"
#include <cstring>
#include "sdkconfig.h"
#include <esp_log.h>
#include <button.h>

static const char *TAG = "main";

static void on_button(button_t *btn, button_state_t state)
{
    ESP_LOGI(TAG, " button %d", state);
}

button_t btn1;


#define I2C_HOST I2C_NUM_0
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");

    btn1.gpio = GPIO_NUM_0;
    btn1.pressed_level = 0;
    btn1.internal_pull = true;
    btn1.autorepeat = false;
    btn1.callback = on_button;
    ESP_ERROR_CHECK(button_init(&btn1));

    ESP_LOGI(TAG, "[APP] done");
}