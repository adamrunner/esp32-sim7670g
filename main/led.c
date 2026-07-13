#include "led.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "modem.h"

#define LED_GPIO 38

static led_strip_handle_t s_strip;

static void led_task(void *arg)
{
    bool on = false;
    while (1) {
        modem_status_t st;
        modem_get_status(&st);

        uint8_t r = 40, g = 0, b = 0;                 // red: no modem
        if (st.at_ok) {
            r = 40; g = 25; b = 0;                    // yellow: no registration
            if (st.reg_status == 1 || st.reg_status == 5) {
                r = 0; g = 40; b = 0;                 // green: registered
            }
            if (st.pdp_active) {
                r = 0; g = 0; b = 40;                 // blue: data connection up
            }
        }

        on = !on;
        if (on) {
            led_strip_set_pixel(s_strip, 0, r, g, b);
        } else {
            led_strip_set_pixel(s_strip, 0, 0, 0, 0);
        }
        led_strip_refresh(s_strip);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip));
    led_strip_clear(s_strip);
    xTaskCreate(led_task, "led", 2048, NULL, 3, NULL);
}
