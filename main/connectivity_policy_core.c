#include "connectivity_policy_core.h"

#include <string.h>


static bool elapsed_at_least(uint64_t now, uint64_t then, uint64_t interval)
{
    return now >= then && now - then >= interval;
}

void connectivity_policy_init(connectivity_policy_t *policy)
{
    memset(policy, 0, sizeof(*policy));
}

bool connectivity_policy_mqtt_healthy(
    uint64_t now_ms,
    const connectivity_snapshot_t *snapshot
)
{
    return snapshot->mqtt_connected &&
           snapshot->puback_seen &&
           now_ms >= snapshot->last_puback_ms &&
           now_ms - snapshot->last_puback_ms <
               CONNECTIVITY_MQTT_UNHEALTHY_MS;
}

connectivity_decision_t connectivity_policy_evaluate(
    connectivity_policy_t *policy,
    uint64_t now_ms,
    const connectivity_snapshot_t *snapshot
)
{
    if (snapshot->ota_active) {
        connectivity_policy_cancel_attempt(policy);
        policy->stable_since_set = false;
        return CONNECTIVITY_DECISION_OTA_ACTIVE;
    }

    if (connectivity_policy_mqtt_healthy(now_ms, snapshot)) {
        if (!policy->stable_since_set) {
            policy->stable_since_set = true;
            policy->stable_since_ms = now_ms;
        }
        if (elapsed_at_least(
                now_ms, policy->stable_since_ms,
                CONNECTIVITY_STABLE_RECOVERY_MS)) {
            policy->failed_redials = 0;
        }
        return CONNECTIVITY_DECISION_HEALTHY;
    }
    policy->stable_since_set = false;

    if (snapshot->wifi_working) {
        return CONNECTIVITY_DECISION_WIFI_UPLINK_HEALTHY;
    }
    if (!snapshot->registered || !snapshot->packet_attached) {
        connectivity_policy_cancel_attempt(policy);
        return CONNECTIVITY_DECISION_WAITING_FOR_COVERAGE;
    }
    if (!snapshot->ppp_up) {
        return CONNECTIVITY_DECISION_WAITING_FOR_MQTT;
    }
    if (snapshot->puback_seen &&
        now_ms >= snapshot->last_puback_ms &&
        now_ms - snapshot->last_puback_ms <
            CONNECTIVITY_MQTT_UNHEALTHY_MS) {
        return CONNECTIVITY_DECISION_WAITING_FOR_MQTT;
    }
    if (policy->recovery_deadline_set &&
        now_ms < policy->recovery_deadline_ms) {
        return CONNECTIVITY_DECISION_WAITING_FOR_RECOVERY;
    }

    if (policy->failed_redials >=
            CONNECTIVITY_FAILED_REDIALS_BEFORE_RESTART &&
        snapshot->modem_responsive) {
        if (policy->last_restart_set &&
            !elapsed_at_least(
                now_ms, policy->last_restart_ms,
                CONNECTIVITY_MODEM_RESTART_COOLDOWN_MS)) {
            return CONNECTIVITY_DECISION_RESTART_COOLDOWN;
        }
        return CONNECTIVITY_DECISION_REQUEST_MODEM_RESTART;
    }

    policy->recovery_deadline_set = true;
    policy->recovery_deadline_ms =
        now_ms + CONNECTIVITY_RECOVERY_ATTEMPT_MS;
    return CONNECTIVITY_DECISION_REQUEST_REDIAL;
}

void connectivity_policy_note_redial_result(
    connectivity_policy_t *policy,
    uint64_t now_ms,
    bool recovered
)
{
    policy->recovery_deadline_set = false;
    if (recovered) {
        policy->stable_since_set = true;
        policy->stable_since_ms = now_ms;
    } else {
        policy->failed_redials++;
    }
}

void connectivity_policy_note_modem_restart(
    connectivity_policy_t *policy,
    uint64_t now_ms
)
{
    policy->last_restart_set = true;
    policy->last_restart_ms = now_ms;
    policy->recovery_deadline_set = true;
    policy->recovery_deadline_ms =
        now_ms + CONNECTIVITY_RECOVERY_ATTEMPT_MS;
}

void connectivity_policy_cancel_attempt(connectivity_policy_t *policy)
{
    policy->recovery_deadline_set = false;
}

const char *connectivity_decision_name(connectivity_decision_t decision)
{
    switch (decision) {
    case CONNECTIVITY_DECISION_HEALTHY:
        return "healthy";
    case CONNECTIVITY_DECISION_WIFI_UPLINK_HEALTHY:
        return "wifi_uplink_healthy";
    case CONNECTIVITY_DECISION_OTA_ACTIVE:
        return "ota_active";
    case CONNECTIVITY_DECISION_WAITING_FOR_COVERAGE:
        return "waiting_for_coverage";
    case CONNECTIVITY_DECISION_WAITING_FOR_MQTT:
        return "waiting_for_mqtt";
    case CONNECTIVITY_DECISION_REQUEST_REDIAL:
        return "request_redial";
    case CONNECTIVITY_DECISION_WAITING_FOR_RECOVERY:
        return "waiting_for_recovery";
    case CONNECTIVITY_DECISION_REQUEST_MODEM_RESTART:
        return "request_modem_restart";
    case CONNECTIVITY_DECISION_RESTART_COOLDOWN:
        return "restart_cooldown";
    default:
        return "unknown";
    }
}
