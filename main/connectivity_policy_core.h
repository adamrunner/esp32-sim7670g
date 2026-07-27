#pragma once

#include <stdbool.h>
#include <stdint.h>


#define CONNECTIVITY_MQTT_UNHEALTHY_MS (90ULL * 1000ULL)
#define CONNECTIVITY_RECOVERY_ATTEMPT_MS (90ULL * 1000ULL)
#define CONNECTIVITY_FAILED_REDIALS_BEFORE_RESTART 2U
#define CONNECTIVITY_MODEM_RESTART_COOLDOWN_MS (15ULL * 60ULL * 1000ULL)
#define CONNECTIVITY_STABLE_RECOVERY_MS (5ULL * 60ULL * 1000ULL)

typedef enum {
    CONNECTIVITY_DECISION_HEALTHY = 0,
    CONNECTIVITY_DECISION_WIFI_UPLINK_HEALTHY,
    CONNECTIVITY_DECISION_OTA_ACTIVE,
    CONNECTIVITY_DECISION_WAITING_FOR_COVERAGE,
    CONNECTIVITY_DECISION_WAITING_FOR_MQTT,
    CONNECTIVITY_DECISION_REQUEST_REDIAL,
    CONNECTIVITY_DECISION_WAITING_FOR_RECOVERY,
    CONNECTIVITY_DECISION_REQUEST_MODEM_RESTART,
    CONNECTIVITY_DECISION_RESTART_COOLDOWN,
} connectivity_decision_t;

typedef struct {
    bool wifi_working;
    bool registered;
    bool packet_attached;
    bool ppp_up;
    bool mqtt_connected;
    bool puback_seen;
    uint64_t last_puback_ms;
    bool modem_responsive;
    bool ota_active;
} connectivity_snapshot_t;

typedef struct {
    uint32_t failed_redials;
    bool recovery_deadline_set;
    uint64_t recovery_deadline_ms;
    bool last_restart_set;
    uint64_t last_restart_ms;
    bool stable_since_set;
    uint64_t stable_since_ms;
} connectivity_policy_t;

void connectivity_policy_init(connectivity_policy_t *policy);

connectivity_decision_t connectivity_policy_evaluate(
    connectivity_policy_t *policy,
    uint64_t now_ms,
    const connectivity_snapshot_t *snapshot
);

void connectivity_policy_note_redial_result(
    connectivity_policy_t *policy,
    uint64_t now_ms,
    bool recovered
);

void connectivity_policy_note_modem_restart(
    connectivity_policy_t *policy,
    uint64_t now_ms
);

void connectivity_policy_cancel_attempt(connectivity_policy_t *policy);

bool connectivity_policy_mqtt_healthy(
    uint64_t now_ms,
    const connectivity_snapshot_t *snapshot
);

const char *connectivity_decision_name(connectivity_decision_t decision);
