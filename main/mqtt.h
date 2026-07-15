#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

// MQTT telemetry reporting over whichever link is up (WiFi STA or cellular
// PPP — plain sockets on the default route, so no AT/PPP contention). The
// broker is configurable via NVS/web UI because the backend hosting decision
// is still open; with no URI configured the module stays idle.

#define MQTT_URI_MAX   128
#define MQTT_USER_MAX  64
#define MQTT_PASS_MAX  64
#define MQTT_TOPIC_MAX 64

typedef struct {
    bool enabled;
    char uri[MQTT_URI_MAX];        // mqtt://host:1883 or mqtts://host:8883
    char username[MQTT_USER_MAX];  // empty = anonymous
    char password[MQTT_PASS_MAX];
    char base_topic[MQTT_TOPIC_MAX]; // telemetry topic prefix, default "bms/telemetry"
} mqtt_config_t;

typedef enum {
    MQTT_UI_DISABLED = 0,  // no URI configured or explicitly disabled
    MQTT_UI_CONNECTING,    // client running, no broker session yet
    MQTT_UI_CONNECTED,
} mqtt_ui_state_t;

typedef struct {
    mqtt_ui_state_t state;
    char uri[MQTT_URI_MAX];
    char base_topic[MQTT_TOPIC_MAX];
    uint32_t published;      // PUBACK-confirmed telemetry messages since boot
    uint32_t publish_fails;  // publish attempts that timed out or errored
    char last_error[64];
} mqtt_status_t;

// Load config from NVS and start the client if enabled. Requires the default
// event loop. Safe to call with no config stored (module stays disabled).
void mqtt_init(void);

// Persist config to NVS and restart the client to apply it. An empty URI or
// enabled=false stops the client.
esp_err_t mqtt_set_config(const mqtt_config_t *cfg);

void mqtt_get_config(mqtt_config_t *out);
void mqtt_get_status(mqtt_status_t *out);

// Append the "mqtt" runtime-status object to the shared /api/status response.
void mqtt_status_json(cJSON *root);
bool mqtt_connected(void);

// Publish one telemetry payload to "<base_topic>/<device_id>" at QoS 1 and
// block until the broker acks it (bounded wait, ~8 s worst case). Returns
// ESP_ERR_INVALID_STATE when disabled/disconnected, ESP_ERR_TIMEOUT when the
// ack never came. Callers treat any failure as "spool it for later".
esp_err_t mqtt_publish_telemetry(const char *payload);
