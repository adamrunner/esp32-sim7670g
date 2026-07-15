#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// microSD (TF) card on the Waveshare ESP32-S3-SIM7670G-4G, wired to the
// ESP32-S3 SDMMC peripheral in 1-bit mode: CLK=GPIO5, CMD=GPIO4, D0=GPIO6.
// A FAT filesystem is mounted at SDCARD_MOUNT_POINT so callers can just open
// files under it, e.g. fopen("/sdcard/log.csv", "a").
#define SDCARD_MOUNT_POINT "/sdcard"

typedef struct {
    bool mounted;             // card mounted and FAT filesystem usable
    uint64_t capacity_bytes;  // total card capacity
    uint64_t free_bytes;      // free space on the FAT volume
    char name[24];            // card product/model name
    char type[8];             // "SDHC", "SDSC", or "MMC"
} sdcard_info_t;

// Mount the card at SDCARD_MOUNT_POINT. Safe to call once at startup; if no
// card is present (or it can't be mounted) this logs a warning and returns the
// error without aborting — the rest of the app runs fine without the card.
// Does NOT format the card on mount failure. Idempotent: a second call while
// already mounted returns ESP_OK.
esp_err_t sdcard_init(void);

// True once a card is mounted and ready for file I/O.
bool sdcard_mounted(void);

// Fill *out with the current card state (capacity/free/name). Recomputes free
// space at call time. Returns ESP_ERR_INVALID_STATE if no card is mounted.
esp_err_t sdcard_info(sdcard_info_t *out);

// Unmount and release the SDMMC bus. Returns ESP_ERR_INVALID_STATE if the card
// was not mounted.
esp_err_t sdcard_unmount(void);
