#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

// Waveshare ESP32-S3-SIM7670G-4G V2.0 onboard MAX17048 fuel gauge.
// The camera SCCB bus shares these pins, so any future camera support must
// reuse this I2C bus rather than installing a second driver on the same GPIOs.
#define BOARD_BATTERY_SDA_PIN 15
#define BOARD_BATTERY_SCL_PIN 16

typedef enum {
    BOARD_POWER_UNKNOWN = 0,
    BOARD_POWER_BATTERY_OR_SOLAR,
    BOARD_POWER_EXTERNAL_RAIL,
} board_power_source_t;

typedef struct {
    bool comm_ok;
    bool ever_ok;
    bool soc_valid;
    bool soc_in_range;
    bool settling;
    board_power_source_t power_source;
    float voltage_v;          // MAX17048 VCELL; actually the board VBAT rail
    float soc_pct;            // clamped to 0..100 for display
    float raw_soc_pct;        // unmodified gauge value for diagnostics
    uint32_t poll_count;
    uint32_t fail_count;
    int64_t last_ok_us;
} board_battery_status_t;

void board_battery_init(void);
void board_battery_get_status(board_battery_status_t *out);

// Append the "internal_battery" object to the shared /api/status response.
void board_battery_status_json(cJSON *root);
