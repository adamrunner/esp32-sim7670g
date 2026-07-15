#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "jbd_bms.h"

// JBD BMS on the 4S4P LiFePO4 house battery, polled over UART2 at 9600.
// Default pins below are free on the Waveshare ESP32-S3-SIM7670G-4G (modem
// owns 17/18, SD owns 4/5/6 + CD 46, LED 38, camera header 7-16/34-37) —
// confirm against the physical header when wiring the BMS.
#define BMS_UART_PORT  UART_NUM_2
#define BMS_TX_PIN     1
#define BMS_RX_PIN     2
#define BMS_BAUD       9600

typedef struct {
    bool enabled;             // polling enabled (NVS bmscfg/enabled)
    bool sim;                 // synthetic data mode (NVS bmscfg/sim)
    bool comm_ok;             // most recent poll succeeded
    bool ever_ok;             // the BMS has answered at least once since boot
    uint32_t poll_count;
    uint32_t fail_count;
    int64_t last_ok_us;       // esp_timer time of last good poll, 0 = never

    // Latest reading (valid once ever_ok, or in sim mode)
    float pack_voltage_v;
    float pack_current_a;     // signed: + charging, - discharging
    float soc_pct;
    float power_w;
    float capacity_ah;        // residual
    float full_capacity_ah;
    double total_energy_wh;   // integrated |power| since boot
    float peak_current_a;
    float peak_power_w;

    int cell_count;
    float cell_v[16];
    float min_cell_v, max_cell_v;
    int min_cell_num, max_cell_num;

    int temp_count;
    float temp_c[8];
    float min_temp_c, max_temp_c;

    bool charging_enabled;    // charge FET on
    bool discharging_enabled; // discharge FET on
    bool balancing;
    jbd_protect_t protection;
} bms_status_t;

// Start the polling task. Call after datalog_init(). With no BMS wired the
// task probes quietly every 30 s; once the BMS has answered it polls
// adaptively (1 s under load, 10 s idle) like esp32-bms-monitor.
void bms_init(void);

void bms_get_status(bms_status_t *out);

// Persist enable/sim flags to NVS; takes effect within one poll cycle.
esp_err_t bms_set_options(bool enabled, bool sim);
