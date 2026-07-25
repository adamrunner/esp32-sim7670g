#include "board_battery.h"

#include <math.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "board_battery";

#define MAX17048_ADDRESS       0x36
#define MAX17048_REG_VCELL     0x02
#define MAX17048_REG_SOC       0x04

#define POLL_INTERVAL_MS       5000
#define I2C_TIMEOUT_MS         250

// V2.0's USB buck regulator drives VBAT to approximately
// 0.6 V * (1 + 110K / 18K) = 4.27 V. A normal one-cell Li-ion battery tops
// out around 4.20 V, so that rail is not a useful battery measurement while
// externally forced. Hysteresis and consecutive low samples keep modem-load
// transients from declaring USB power gone.
#define EXTERNAL_ENTER_V       4.23f
#define EXTERNAL_EXIT_V        4.21f
#define EXTERNAL_EXIT_SAMPLES  3
#define RECOVERY_MIN_MS        15000
#define RECOVERY_SOC_DELTA_PCT 1.0f
#define PLAUSIBLE_FULL_V       4.10f
#define PLAUSIBLE_FULL_SOC_PCT 90.0f

static SemaphoreHandle_t s_mutex;
static board_battery_status_t s_status;
static i2c_master_dev_handle_t s_gauge;
static bool s_external_rail;
static int s_external_exit_count;
static bool s_recovering;
static float s_external_soc_pct;
static int64_t s_recovery_start_us;

static esp_err_t read_register(uint8_t reg, uint16_t *value)
{
    uint8_t data[2];
    esp_err_t err = i2c_master_transmit_receive(
        s_gauge, &reg, sizeof(reg), data, sizeof(data), I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        *value = ((uint16_t)data[0] << 8) | data[1];
    }
    return err;
}

static void update_power_state(float voltage_v, float raw_soc_pct, int64_t now_us)
{
    if (voltage_v >= EXTERNAL_ENTER_V) {
        s_external_rail = true;
        s_external_exit_count = 0;
        s_recovering = false;
        s_external_soc_pct = raw_soc_pct;
        return;
    }

    if (!s_external_rail) {
        if (s_recovering && now_us - s_recovery_start_us >= RECOVERY_MIN_MS * 1000LL) {
            bool soc_changed = fabsf(raw_soc_pct - s_external_soc_pct)
                             >= RECOVERY_SOC_DELTA_PCT;
            bool plausibly_full = voltage_v >= PLAUSIBLE_FULL_V &&
                                  raw_soc_pct >= PLAUSIBLE_FULL_SOC_PCT &&
                                  raw_soc_pct <= 101.0f;
            if (soc_changed || plausibly_full) {
                s_recovering = false;
                ESP_LOGI(TAG, "fuel gauge recovered after external rail removal");
            }
        }
        return;
    }

    if (voltage_v <= EXTERNAL_EXIT_V) {
        if (++s_external_exit_count >= EXTERNAL_EXIT_SAMPLES) {
            s_external_rail = false;
            s_external_exit_count = 0;
            s_recovering = true;
            s_recovery_start_us = now_us;
            ESP_LOGI(TAG, "external rail removed; waiting for fuel gauge recovery");
        }
    } else {
        s_external_exit_count = 0;
    }
}

static void battery_task(void *arg)
{
    bool logged_present = false;

    while (true) {
        uint16_t raw_vcell;
        uint16_t raw_soc;
        esp_err_t voltage_err = read_register(MAX17048_REG_VCELL, &raw_vcell);
        esp_err_t soc_err = voltage_err == ESP_OK
                          ? read_register(MAX17048_REG_SOC, &raw_soc)
                          : voltage_err;
        int64_t now_us = esp_timer_get_time();

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.poll_count++;
        if (voltage_err == ESP_OK && soc_err == ESP_OK) {
            float voltage_v = raw_vcell * 78.125e-6f;
            float raw_soc_pct = raw_soc / 256.0f;
            update_power_state(voltage_v, raw_soc_pct, now_us);

            s_status.comm_ok = true;
            s_status.ever_ok = true;
            s_status.voltage_v = voltage_v;
            s_status.raw_soc_pct = raw_soc_pct;
            s_status.soc_pct = fminf(100.0f, fmaxf(0.0f, raw_soc_pct));
            s_status.soc_in_range = raw_soc_pct <= 101.0f;
            s_status.power_source = s_external_rail
                                  ? BOARD_POWER_EXTERNAL_RAIL
                                  : BOARD_POWER_BATTERY_OR_SOLAR;
            s_status.settling = s_recovering;
            s_status.soc_valid = !s_external_rail && !s_recovering &&
                                 s_status.soc_in_range;
            s_status.last_ok_us = now_us;
            if (!logged_present) {
                ESP_LOGI(TAG, "MAX17048 online at 0x%02x (SDA=%d SCL=%d)",
                         MAX17048_ADDRESS, BOARD_BATTERY_SDA_PIN, BOARD_BATTERY_SCL_PIN);
                logged_present = true;
            }
        } else {
            s_status.comm_ok = false;
            s_status.fail_count++;
            s_status.soc_valid = false;
            s_status.soc_in_range = false;
            s_status.power_source = BOARD_POWER_UNKNOWN;
            if (!s_status.ever_ok && s_status.fail_count == 1) {
                ESP_LOGW(TAG, "MAX17048 not responding: %s",
                         esp_err_to_name(voltage_err != ESP_OK ? voltage_err : soc_err));
            }
        }
        xSemaphoreGive(s_mutex);

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void board_battery_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_BATTERY_SDA_PIN,
        .scl_io_num = BOARD_BATTERY_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MAX17048_ADDRESS,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &device_config, &s_gauge));

    xTaskCreate(battery_task, "board_battery", 3072, NULL, 4, NULL);
}

void board_battery_get_status(board_battery_status_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mutex);
}

void board_battery_status_json(cJSON *root)
{
    board_battery_status_t b;
    board_battery_get_status(&b);
    const char *validity = !b.ever_ok ? "no_sample"
                           : !b.comm_ok ? "read_error"
                           : b.power_source == BOARD_POWER_EXTERNAL_RAIL ? "external_rail"
                           : b.settling ? "recovering"
                           : !b.soc_in_range ? "out_of_range"
                           : "valid";

    cJSON *battery = cJSON_AddObjectToObject(root, "internal_battery");
    cJSON_AddBoolToObject(battery, "comm_ok", b.comm_ok);
    cJSON_AddBoolToObject(battery, "ever_ok", b.ever_ok);
    cJSON_AddBoolToObject(battery, "soc_valid", b.soc_valid);
    cJSON_AddBoolToObject(battery, "soc_in_range", b.soc_in_range);
    cJSON_AddBoolToObject(battery, "settling", b.settling);
    cJSON_AddStringToObject(battery, "validity", validity);
    cJSON_AddStringToObject(battery, "power_source",
        b.power_source == BOARD_POWER_EXTERNAL_RAIL ? "external_rail"
      : b.power_source == BOARD_POWER_BATTERY_OR_SOLAR ? "battery_or_solar"
                                                       : "unknown");
    cJSON_AddBoolToObject(battery, "external_power_inferred",
                         b.power_source == BOARD_POWER_EXTERNAL_RAIL);
    cJSON_AddNumberToObject(battery, "polls", b.poll_count);
    cJSON_AddNumberToObject(battery, "fails", b.fail_count);
    if (b.ever_ok) {
        cJSON_AddNumberToObject(battery, "voltage_v", b.voltage_v);
        cJSON_AddNumberToObject(battery, "soc_pct", b.soc_pct);
        cJSON_AddNumberToObject(battery, "raw_soc_pct", b.raw_soc_pct);
        cJSON_AddNumberToObject(battery, "sample_age_s",
                                (double)((esp_timer_get_time() - b.last_ok_us) / 1000000));
    }
}
