#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "cJSON.h"


typedef struct {
    bool at_window_active;
    bool puback_wait_active;
    uint32_t at_window_count;
    uint32_t at_window_deferred_count;
    uint32_t puback_wait_count;
    uint32_t publish_deferred_count;
    uint64_t last_at_window_uptime_ms;
    uint64_t last_at_deferred_uptime_ms;
    uint64_t last_puback_wait_uptime_ms;
    uint64_t last_publish_deferred_uptime_ms;
} network_pause_gate_status_t;

// Shared exclusion between a live-PPP AT window and a synchronous MQTT
// publish/PUBACK transaction. Call once before modem or MQTT tasks start.
void network_pause_gate_init(void);

// AT callers may wait (manual maintenance) or pass zero ticks (periodic poll).
// A successful begin must be paired with network_pause_gate_at_end().
bool network_pause_gate_at_begin(TickType_t wait_ticks);
void network_pause_gate_at_end(void);

// Publishers use a zero-wait begin so an in-progress AT window turns into a
// deliberate queue/spool decision rather than a manufactured PUBACK timeout.
bool network_pause_gate_puback_begin(void);
void network_pause_gate_puback_end(void);

void network_pause_gate_get_status(network_pause_gate_status_t *out);
void network_pause_gate_status_json(cJSON *root);
