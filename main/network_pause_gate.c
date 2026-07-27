#include "network_pause_gate.h"

#include <string.h>

#include "freertos/semphr.h"
#include "esp_timer.h"


static SemaphoreHandle_t s_gate;
static SemaphoreHandle_t s_status_mutex;
static network_pause_gate_status_t s_status;

static uint64_t uptime_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

void network_pause_gate_init(void)
{
    s_gate = xSemaphoreCreateMutex();
    s_status_mutex = xSemaphoreCreateMutex();
    configASSERT(s_gate && s_status_mutex);
}

bool network_pause_gate_at_begin(TickType_t wait_ticks)
{
    if (!s_gate || xSemaphoreTake(s_gate, wait_ticks) != pdTRUE) {
        if (s_status_mutex) {
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_status.at_window_deferred_count++;
            s_status.last_at_deferred_uptime_ms = uptime_ms();
            xSemaphoreGive(s_status_mutex);
        }
        return false;
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.at_window_active = true;
    s_status.at_window_count++;
    s_status.last_at_window_uptime_ms = uptime_ms();
    xSemaphoreGive(s_status_mutex);
    return true;
}

void network_pause_gate_at_end(void)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.at_window_active = false;
    xSemaphoreGive(s_status_mutex);
    xSemaphoreGive(s_gate);
}

bool network_pause_gate_puback_begin(void)
{
    if (!s_gate || xSemaphoreTake(s_gate, 0) != pdTRUE) {
        if (s_status_mutex) {
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_status.publish_deferred_count++;
            s_status.last_publish_deferred_uptime_ms = uptime_ms();
            xSemaphoreGive(s_status_mutex);
        }
        return false;
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.puback_wait_active = true;
    s_status.puback_wait_count++;
    s_status.last_puback_wait_uptime_ms = uptime_ms();
    xSemaphoreGive(s_status_mutex);
    return true;
}

void network_pause_gate_puback_end(void)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.puback_wait_active = false;
    xSemaphoreGive(s_status_mutex);
    xSemaphoreGive(s_gate);
}

void network_pause_gate_get_status(network_pause_gate_status_t *out)
{
    if (!s_status_mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_mutex);
}

void network_pause_gate_status_json(cJSON *root)
{
    network_pause_gate_status_t status;
    network_pause_gate_get_status(&status);
    cJSON *gate = cJSON_AddObjectToObject(root, "network_pause");
    cJSON_AddBoolToObject(gate, "at_window_active",
                          status.at_window_active);
    cJSON_AddBoolToObject(gate, "puback_wait_active",
                          status.puback_wait_active);
    cJSON_AddNumberToObject(gate, "at_window_count",
                            status.at_window_count);
    cJSON_AddNumberToObject(gate, "at_window_deferred_count",
                            status.at_window_deferred_count);
    cJSON_AddNumberToObject(gate, "puback_wait_count",
                            status.puback_wait_count);
    cJSON_AddNumberToObject(gate, "publish_deferred_count",
                            status.publish_deferred_count);
    cJSON_AddNumberToObject(gate, "last_at_window_uptime_ms",
                            (double)status.last_at_window_uptime_ms);
    cJSON_AddNumberToObject(gate, "last_at_deferred_uptime_ms",
                            (double)status.last_at_deferred_uptime_ms);
    cJSON_AddNumberToObject(gate, "last_puback_wait_uptime_ms",
                            (double)status.last_puback_wait_uptime_ms);
    cJSON_AddNumberToObject(gate, "last_publish_deferred_uptime_ms",
                            (double)status.last_publish_deferred_uptime_ms);
}
