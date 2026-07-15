idf_component_register(
    SRCS "wifi.c" "app_main.c" "modem.c" "webui.c" "led.c" "ota.c" "sdcard.c"
    INCLUDE_DIRS "."
    EMBED_FILES "www/index.html"
    PRIV_REQUIRES driver esp_wifi esp_http_server nvs_flash json esp_netif esp_timer esp_event lwip
                  esp_https_ota esp_http_client app_update esp_app_format esp_partition mbedtls
                  fatfs esp_driver_sdmmc
)

# Testing hook: point OTA at a staging manifest without editing ota.c, e.g.
#   idf.py -DOTA_MANIFEST_URL=https://example.com/test/manifest.json build
if(DEFINED OTA_MANIFEST_URL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
                               "OTA_MANIFEST_URL=\"${OTA_MANIFEST_URL}\"")
endif()
