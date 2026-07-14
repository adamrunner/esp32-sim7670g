idf_component_register(
    SRCS "wifi.c" "app_main.c" "modem.c" "webui.c" "led.c"
    INCLUDE_DIRS "."
    EMBED_FILES "www/index.html"
    PRIV_REQUIRES driver esp_wifi esp_http_server nvs_flash json esp_netif esp_timer esp_event lwip
)
