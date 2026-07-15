#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "bms.h"
#include "datalog.h"
#include "led.h"
#include "modem.h"
#include "mqtt.h"
#include "ota.h"
#include "sdcard.h"
#include "timesync.h"
#include "webui.h"
#include "wifi.h"

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

    sdcard_init(); // mount the microSD at /sdcard for later data logging;
                   // logs a warning and continues if no card is present
    modem_init();  // creates the status mutex the LED task reads through
    led_init();
    wifi_init();   // joins the stored home network, or falls back to SoftAP
    webui_init();
    ota_init();    // confirms/rolls back a pending image, then polls hourly

    // Telemetry: wall clock, broker link, fan-out pipeline, then producers.
    timesync_init();
    mqtt_init();
    datalog_init();
    bms_init();

    ESP_LOGI(TAG, "up — if no home WiFi is stored/reachable, join "
                  "\"ESP32-SIM7670G\" (pass \"waveshare\") and open "
                  "http://192.168.4.1/ to configure it");
}
