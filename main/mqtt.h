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
    uint32_t publish_deferred; // queued because a short AT window was active
    bool availability_confirmed;  // retained online state received a PUBACK
    uint32_t availability_attempts;
    uint32_t availability_failures;
    uint32_t availability_deferred;
    bool availability_requested;
    bool availability_queued;
    char availability_last_error[64];
    char last_error[64];
    uint32_t connect_count;
    uint32_t disconnect_count;
    uint32_t error_count;
    uint32_t puback_count;
    uint32_t puback_timeouts;
    uint32_t availability_requested_count;
    uint64_t last_connect_uptime_ms;
    uint64_t last_disconnect_uptime_ms;
    uint64_t last_error_uptime_ms;
    uint64_t last_puback_uptime_ms;
    uint64_t last_puback_timeout_uptime_ms;
    uint64_t last_publish_attempt_uptime_ms;
    uint64_t availability_last_requested_uptime_ms;
    uint64_t availability_last_queued_uptime_ms;
    uint64_t availability_last_ack_uptime_ms;
    uint64_t availability_last_failure_uptime_ms;
} mqtt_status_t;

// Load config from NVS and start the client if enabled. Requires the default
// event loop. Safe to call with no config stored (module stays disabled).
// Each broker session has a retained offline last will on
// "bms/availability/<device_id>" and publishes retained online state after
// connecting.
void mqtt_init(void);

// Persist config to NVS and restart the client to apply it. An empty URI or
// enabled=false stops the client.
esp_err_t mqtt_set_config(const mqtt_config_t *cfg);

void mqtt_get_config(mqtt_config_t *out);
void mqtt_get_status(mqtt_status_t *out);

// Append the "mqtt" runtime-status object to the shared /api/status response.
void mqtt_status_json(cJSON *root);
bool mqtt_connected(void);

// Retry retained online availability from the existing datalog task. This
// avoids allocating another task during the constrained post-boot window.
// Call from one task only; the function may wait for a bounded QoS 1 PUBACK.
void mqtt_maintenance_tick(void);

// Publish a retained schema-v2 OTA-verified status event to
// "bms/status/<device_id>". Boot, reconnect, pending-verify, and first-time-
// synchronization events are sent automatically. Each logical event receives
// a boot-scoped status_seq, which is reused by MQTT's QoS retry.
esp_err_t mqtt_publish_status(void);

// Publish one telemetry payload to "<base_topic>/<device_id>" at QoS 1 and
// block until the broker acks it (bounded wait, ~8 s worst case). Returns
// ESP_ERR_INVALID_STATE when disabled/disconnected, ESP_ERR_TIMEOUT when the
// ack never came. Callers treat any failure as "spool it for later".
esp_err_t mqtt_publish_telemetry(const char *payload);
