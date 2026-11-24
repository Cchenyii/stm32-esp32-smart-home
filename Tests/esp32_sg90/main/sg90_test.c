#include <stdint.h>

#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum {
    SERVO_GPIO = 25,
    PWM_HZ = 50,
    PERIOD_US = 1000000 / PWM_HZ,
    CENTER_US = 1500,
    OPEN_US = 1100,
    CLOSED_US = 1900,
    HOLD_MS = 2000,
};

static uint32_t duty_for_pulse(unsigned pulse_us)
{
    const uint32_t counts_per_period = 1U << LEDC_TIMER_14_BIT;
    return (pulse_us * counts_per_period + PERIOD_US / 2) / PERIOD_US;
}

static void hold_position(unsigned pulse_us)
{
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                                  duty_for_pulse(pulse_us)));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
    ESP_LOGI("SG90", "GPIO%d %d Hz, pulse %u us", SERVO_GPIO, PWM_HZ, pulse_us);
    vTaskDelay(pdMS_TO_TICKS(HOLD_MS));
}

void app_main(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .freq_hz = PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    const ledc_channel_config_t channel = {
        .gpio_num = SERVO_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = duty_for_pulse(CENTER_US),
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));

    hold_position(CENTER_US);
    for (;;) {
        hold_position(OPEN_US);
        hold_position(CLOSED_US);
    }
}
