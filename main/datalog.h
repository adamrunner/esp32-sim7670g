#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

// Telemetry pipeline: producers hand in snapshots, a background task fans
// them out to sinks so sampling never blocks on I/O. Sinks today:
//  - SD card: /sdcard/bms/YYYY-MM-DD.csv, daily rotation, header on create.
//  - MQTT: live publish via mqtt.c; failures divert to an SD spool that is
//    replayed (rate-limited, in order) when the broker comes back.
// The CSV line is byte-compatible with esp32-bms-monitor's serializer so
// bms-dashboard ingests it unchanged: 23 fixed columns, then cell_v_1..N,
// then temp_c_1..M. Keep it that way or version it deliberately.

#define DATALOG_MAX_CELLS 16   // CSV schema width, not a JBD limit
#define DATALOG_MAX_TEMPS 8

// One BMS sample. Field-for-field the reference project's BMSSnapshot.
typedef struct {
    time_t real_timestamp;      // epoch seconds, 0 until timesync_valid()
    uint32_t elapsed_sec;       // uptime seconds when sampled
    double total_energy_wh;     // integrated |energy| since boot

    float pack_voltage_v;
    float pack_current_a;       // signed: + charging, - discharging
    float soc_pct;
    float power_w;
    float full_capacity_ah;

    float peak_current_a;
    float peak_power_w;

    int cell_count;
    float min_cell_voltage_v;
    float max_cell_voltage_v;
    int min_cell_num;           // 1-based
    int max_cell_num;           // 1-based
    float cell_voltage_delta_v;

    int temp_count;
    float min_temp_c;
    float max_temp_c;

    bool charging_enabled;
    bool discharging_enabled;

    float cell_v[DATALOG_MAX_CELLS];
    float temp_c[DATALOG_MAX_TEMPS];
} bms_snapshot_t;

typedef struct {
    uint32_t rows;             // snapshots accepted since boot
    uint32_t dropped;          // snapshots lost to a full queue
    bool sd_ok;                // SD sink writing (card mounted, not full)
    char sd_file[40];          // current log file, "" if none yet
    uint32_t sd_rows;          // rows written to SD
    uint32_t spool_pending;    // bytes waiting for replay to the broker
    uint32_t spool_replayed;   // spooled rows delivered since boot
    uint32_t mqtt_rows;        // rows delivered live (not via spool)
} datalog_status_t;

// Start the queue + fan-out task. Call after sdcard_init()/mqtt_init().
void datalog_init(void);

// Queue one snapshot (copied; returns immediately). Drops with a counter
// bump if the pipeline is backed up.
void datalog_submit(const bms_snapshot_t *snap);

void datalog_get_status(datalog_status_t *out);

// This node's telemetry identity: NVS logcfg/device_id if set, else
// "gw-" + the last three MAC octets. Used as CSV column 1 and MQTT topic
// suffix.
void datalog_device_id(char *out, size_t out_len);
