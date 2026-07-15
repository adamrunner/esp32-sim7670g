#include "bms.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

#include "datalog.h"
#include "timesync.h"

static const char *TAG = "bms";

#define NVS_NS "bmscfg"

// Poll cadence, matching esp32-bms-monitor's adaptive scheme.
#define POLL_ACTIVE_MS   1000   // charging or discharging
#define POLL_IDLE_MS     10000
#define POLL_PROBE_MS    30000  // BMS has never answered (probably not wired yet)
#define ACTIVE_CURRENT_A 0.5f
#define ACTIVE_POWER_W   10.0f
// Gaps longer than this (comms outage) don't integrate into the energy total.
#define ENERGY_MAX_GAP_US (120LL * 1000000)

static SemaphoreHandle_t s_mutex;
static bms_status_t s_status;
static bms_interface_t *s_bms;

static void load_options(bool *enabled, bool *sim)
{
    uint8_t en = 1, sm = 0;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "enabled", &en);
        nvs_get_u8(nvs, "sim", &sm);
        nvs_close(nvs);
    }
    *enabled = en != 0;
    *sim = sm != 0;
}

esp_err_t bms_set_options(bool enabled, bool sim)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_u8(nvs, "enabled", enabled ? 1 : 0);
    nvs_set_u8(nvs, "sim", sim ? 1 : 0);
    err = nvs_commit(nvs);
    nvs_close(nvs);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.enabled = enabled;
    s_status.sim = sim;
    xSemaphoreGive(s_mutex);
    return err;
}

void bms_get_status(bms_status_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mutex);
}

// Copy the driver's parsed data into the shared status snapshot and
// integrate energy. Called with real data or sim data already staged in *d.
static void publish_reading(const jbd_bms_data_t *d)
{
    int64_t now = esp_timer_get_time();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_status.last_ok_us &&
        now - s_status.last_ok_us < ENERGY_MAX_GAP_US) {
        double dt_h = (double)(now - s_status.last_ok_us) / 3600e6;
        s_status.total_energy_wh += fabsf(d->power) * dt_h;
    }
    s_status.comm_ok = true;
    s_status.ever_ok = true;
    s_status.last_ok_us = now;

    s_status.pack_voltage_v = d->packVoltage;
    s_status.pack_current_a = d->packCurrent;
    s_status.soc_pct = d->packSOC;
    s_status.power_w = d->power;
    s_status.capacity_ah = d->capacity;
    s_status.full_capacity_ah = d->fullCapacity;
    if (fabsf(d->packCurrent) > s_status.peak_current_a) {
        s_status.peak_current_a = fabsf(d->packCurrent);
    }
    if (fabsf(d->power) > s_status.peak_power_w) {
        s_status.peak_power_w = fabsf(d->power);
    }

    s_status.cell_count = d->cellCount < 16 ? d->cellCount : 16;
    for (int i = 0; i < s_status.cell_count; i++) {
        s_status.cell_v[i] = d->cellVoltages[i];
    }
    s_status.min_cell_v = d->minCellVoltage;
    s_status.max_cell_v = d->maxCellVoltage;
    s_status.min_cell_num = d->minCellNumber;
    s_status.max_cell_num = d->maxCellNumber;

    s_status.temp_count = d->temperatureCount < 8 ? d->temperatureCount : 8;
    for (int i = 0; i < s_status.temp_count; i++) {
        s_status.temp_c[i] = d->temperatures[i];
    }
    s_status.min_temp_c = d->minTemperature;
    s_status.max_temp_c = d->maxTemperature;

    s_status.charging_enabled = d->chargingEnabled;
    s_status.discharging_enabled = d->dischargingEnabled;
    s_status.balancing = d->balancingActive;
    s_status.protection = d->protection;

    // Snapshot for the datalog pipeline, built under the same lock so the
    // CSV row matches what the web UI shows.
    bms_snapshot_t snap = {
        .real_timestamp = timesync_valid() ? time(NULL) : 0,
        .elapsed_sec = (uint32_t)(now / 1000000),
        .total_energy_wh = s_status.total_energy_wh,
        .pack_voltage_v = s_status.pack_voltage_v,
        .pack_current_a = s_status.pack_current_a,
        .soc_pct = s_status.soc_pct,
        .power_w = s_status.power_w,
        .full_capacity_ah = s_status.full_capacity_ah,
        .peak_current_a = s_status.peak_current_a,
        .peak_power_w = s_status.peak_power_w,
        .cell_count = s_status.cell_count,
        .min_cell_voltage_v = s_status.min_cell_v,
        .max_cell_voltage_v = s_status.max_cell_v,
        .min_cell_num = s_status.min_cell_num,
        .max_cell_num = s_status.max_cell_num,
        .cell_voltage_delta_v = s_status.max_cell_v - s_status.min_cell_v,
        .temp_count = s_status.temp_count,
        .min_temp_c = s_status.min_temp_c,
        .max_temp_c = s_status.max_temp_c,
        .charging_enabled = s_status.charging_enabled,
        .discharging_enabled = s_status.discharging_enabled,
    };
    memcpy(snap.cell_v, s_status.cell_v, sizeof(snap.cell_v));
    memcpy(snap.temp_c, s_status.temp_c, sizeof(snap.temp_c));
    xSemaphoreGive(s_mutex);

    datalog_submit(&snap);
}

// ---------------------------------------------------------------------------
// Simulation: plausible 4S LiFePO4 numbers so the datalog/MQTT/spool pipeline
// can be exercised end-to-end before the BMS is wired up. Models a fridge
// compressor duty cycle: ~3 min drawing ~3.8 A, ~5 min at standby draw.

static float frand(float lo, float hi)
{
    return lo + (hi - lo) * ((float)esp_random() / (float)UINT32_MAX);
}

static void sim_reading(jbd_bms_data_t *d)
{
    static float soc = 87.0f;

    int64_t cycle_s = (esp_timer_get_time() / 1000000) % 480;
    bool compressor_on = cycle_s < 180;
    float current = compressor_on ? frand(-4.0f, -3.6f) : frand(-0.2f, -0.1f);

    soc -= fabsf(current) / 100.0f / 36.0f;  // ~100 Ah pack, per ~1 s tick
    if (soc < 20.0f) {
        soc = 87.0f;
    }

    memset(d, 0, sizeof(*d));
    d->cellCount = 4;
    float pack = 0;
    for (int i = 0; i < 4; i++) {
        d->cellVoltages[i] = 3.30f + (soc - 50.0f) * 0.002f
                           + current * 0.003f + frand(-0.004f, 0.004f);
        pack += d->cellVoltages[i];
    }
    d->packVoltage = pack;
    d->packCurrent = current;
    d->packSOC = soc;
    d->pctCapacity = (uint8_t)soc;
    d->power = pack * current;
    d->fullCapacity = 100.0f;
    d->capacity = soc;
    d->temperatureCount = 2;
    d->temperatures[0] = 22.0f + frand(-0.3f, 0.3f);
    d->temperatures[1] = 24.0f + frand(-0.3f, 0.3f) + (compressor_on ? 1.5f : 0);
    d->minTemperature = d->temperatures[0] < d->temperatures[1] ? d->temperatures[0] : d->temperatures[1];
    d->maxTemperature = d->temperatures[0] > d->temperatures[1] ? d->temperatures[0] : d->temperatures[1];
    d->chargingEnabled = true;
    d->dischargingEnabled = true;

    d->minCellVoltage = 5.0f;
    d->maxCellVoltage = 0.0f;
    for (int i = 0; i < 4; i++) {
        if (d->cellVoltages[i] < d->minCellVoltage) {
            d->minCellVoltage = d->cellVoltages[i];
            d->minCellNumber = i + 1;
        }
        if (d->cellVoltages[i] > d->maxCellVoltage) {
            d->maxCellVoltage = d->cellVoltages[i];
            d->maxCellNumber = i + 1;
        }
    }
}

// ---------------------------------------------------------------------------

static void bms_task(void *arg)
{
    int64_t last_fail_log_us = 0;

    while (true) {
        bool enabled, sim;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        enabled = s_status.enabled;
        sim = s_status.sim;
        xSemaphoreGive(s_mutex);

        if (!enabled) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (sim) {
            jbd_bms_data_t d;
            sim_reading(&d);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.poll_count++;
            xSemaphoreGive(s_mutex);
            publish_reading(&d);
            vTaskDelay(pdMS_TO_TICKS(POLL_ACTIVE_MS));
            continue;
        }

        if (!s_bms) {
            s_bms = jbd_bms_create_ex(BMS_UART_PORT, BMS_RX_PIN, BMS_TX_PIN, BMS_BAUD);
            if (!s_bms) {
                ESP_LOGE(TAG, "UART setup failed; retrying in 30 s");
                vTaskDelay(pdMS_TO_TICKS(POLL_PROBE_MS));
                continue;
            }
        }

        bool ok = s_bms->readMeasurements(s_bms->handle);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.poll_count++;
        if (!ok) {
            s_status.fail_count++;
            s_status.comm_ok = false;
        }
        bool ever_ok = s_status.ever_ok;
        xSemaphoreGive(s_mutex);

        int delay_ms;
        if (ok) {
            last_fail_log_us = 0;  // a future outage logs right away
            jbd_bms_handle_t *h = s_bms->handle;
            publish_reading(&h->data);
            bool active = fabsf(h->data.packCurrent) > ACTIVE_CURRENT_A ||
                          fabsf(h->data.power) > ACTIVE_POWER_W;
            delay_ms = active ? POLL_ACTIVE_MS : POLL_IDLE_MS;
        } else if (ever_ok) {
            // Was talking, went quiet: keep trying at the idle rate but
            // don't spam the log.
            if (!last_fail_log_us ||
                esp_timer_get_time() - last_fail_log_us > 60LL * 1000000) {
                ESP_LOGW(TAG, "BMS not responding");
                last_fail_log_us = esp_timer_get_time();
            }
            delay_ms = POLL_IDLE_MS;
        } else {
            // Never seen: probably not wired yet. Probe quietly.
            if (!last_fail_log_us ||
                esp_timer_get_time() - last_fail_log_us > 300LL * 1000000) {
                ESP_LOGI(TAG, "no BMS detected on UART%d (TX=%d RX=%d); probing every %d s",
                         BMS_UART_PORT, BMS_TX_PIN, BMS_RX_PIN, POLL_PROBE_MS / 1000);
                last_fail_log_us = esp_timer_get_time();
            }
            delay_ms = POLL_PROBE_MS;
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void bms_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    load_options(&s_status.enabled, &s_status.sim);
    xTaskCreate(bms_task, "bms", 5120, NULL, 4, NULL);
}
