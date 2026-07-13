#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "led.h"
#include "modem.h"
#include "webui.h"

static const char *TAG = "app";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    // Shared by the PPP netif (modem) and the WiFi AP (webui)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    modem_init();  // creates the status mutex the LED task reads through
    led_init();
    webui_init();

    ESP_LOGI(TAG, "up — join WiFi \"ESP32-SIM7670G\" (pass \"waveshare\") "
                  "and open http://192.168.4.1/");
}
