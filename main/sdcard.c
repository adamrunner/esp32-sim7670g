#include "sdcard.h"

#include <string.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

// SDMMC slot-1 pins for this board (Waveshare ESP32-S3-SIM7670G-4G). The
// on-board TF slot is wired for 1-bit SD mode; there is no D1/D2/D3.
#define SDCARD_PIN_CLK 5
#define SDCARD_PIN_CMD 4
#define SDCARD_PIN_D0  6

static const char *TAG = "sdcard";

static sdmmc_card_t *s_card;

bool sdcard_mounted(void)
{
    return s_card != NULL;
}

esp_err_t sdcard_init(void)
{
    if (s_card != NULL) {
        return ESP_OK;  // already mounted
    }

    // Don't reformat a card that won't mount — a fresh insert may just be a
    // different filesystem, and reformatting would silently destroy user data.
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();  // slot 1, 20 MHz default

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;              // board only breaks out D0
    slot_config.clk = SDCARD_PIN_CLK;
    slot_config.cmd = SDCARD_PIN_CMD;
    slot_config.d0 = SDCARD_PIN_D0;
    // Enable the SoC's internal pull-ups as a backstop; the board carries its
    // own line pull-ups but this keeps CMD/D0 defined if a card is marginal.
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host,
                                            &slot_config, &mount_config, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        if (err == ESP_FAIL) {
            ESP_LOGW(TAG, "no filesystem found on the card (mount failed); "
                          "insert a FAT-formatted card and reboot");
        } else {
            ESP_LOGW(TAG, "failed to initialize the card: %s — is one inserted?",
                     esp_err_to_name(err));
        }
        return err;
    }

    uint64_t total = 0, freeb = 0;
    esp_vfs_fat_info(SDCARD_MOUNT_POINT, &total, &freeb);
    ESP_LOGI(TAG, "mounted %s at %s: %.2f GB total, %.2f GB free",
             s_card->cid.name, SDCARD_MOUNT_POINT,
             total / 1e9, freeb / 1e9);
    return ESP_OK;
}

esp_err_t sdcard_info(sdcard_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (s_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    out->mounted = true;
    // csd.capacity is in sectors; sector_size is bytes/sector.
    out->capacity_bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;

    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(SDCARD_MOUNT_POINT, &total, &freeb) == ESP_OK) {
        out->free_bytes = freeb;
    }

    strlcpy(out->name, s_card->cid.name, sizeof(out->name));
    const char *type;
    if (s_card->is_mmc) {
        type = "MMC";
    } else if (s_card->ocr & (1 << 30)) {  // CCS bit: high-capacity (SDHC/SDXC)
        type = "SDHC";
    } else {
        type = "SDSC";
    }
    strlcpy(out->type, type, sizeof(out->type));
    return ESP_OK;
}

esp_err_t sdcard_unmount(void)
{
    if (s_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
    if (err == ESP_OK) {
        s_card = NULL;
        ESP_LOGI(TAG, "unmounted %s", SDCARD_MOUNT_POINT);
    } else {
        ESP_LOGW(TAG, "unmount failed: %s", esp_err_to_name(err));
    }
    return err;
}
