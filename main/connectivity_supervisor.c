#include "connectivity_supervisor.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "connectivity_policy_core.h"
#include "event_journal.h"
#include "modem.h"
#include "mqtt.h"
#include "ota.h"
#include "wifi.h"


#define SUPERVISOR_TICK_MS 1000
#define CLEAN_REDIAL_ENABLED 0
#define MODEM_RESET_ESCALATION_ENABLED 0

typedef struct {
    connectivity_decision_t decision;
    connectivity_snapshot_t snapshot;
    bool initialized;
    bool dry_run_attempt_pending;
    uint32_t evaluation_count;
    uint32_t transition_count;
    uint32_t dry_run_redial_count;
    uint32_t dry_run_restart_count;
    uint64_t last_evaluation_uptime_ms;
    uint64_t last_transition_uptime_ms;
    uint64_t last_would_action_uptime_ms;
    char would_action[24];
} supervisor_status_t;

static SemaphoreHandle_t s_mutex;
static connectivity_policy_t s_policy;
static supervisor_status_t s_status;

static uint64_t uptime_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static bool ota_is_active(ota_state_t state)
{
    return state == OTA_STATE_CHECKING ||
           state == OTA_STATE_DOWNLOADING ||
           state == OTA_STATE_VERIFYING ||
           state == OTA_STATE_WAIT_REBOOT;
}

static connectivity_snapshot_t take_snapshot(void)
{
    wifi_ui_status_t wifi;
    modem_status_t modem;
    mqtt_status_t mqtt;
    ota_status_t ota;
    wifi_get_status(&wifi);
    modem_get_status(&modem);
    mqtt_get_status(&mqtt);
    ota_get_status(&ota);

    return (connectivity_snapshot_t) {
        .wifi_working = wifi.state == WIFI_UI_STA_CONNECTED,
        .registered = modem.reg_status == 1 || modem.reg_status == 5,
        .packet_attached = modem.packet_attached || modem.ppp_up,
        .ppp_up = modem.ppp_up,
        .mqtt_connected = mqtt.state == MQTT_UI_CONNECTED,
        .puback_seen = mqtt.last_puback_uptime_ms != 0,
        .last_puback_ms = mqtt.last_puback_uptime_ms,
        .modem_responsive = modem.at_ok,
        .ota_active = ota_is_active(ota.state),
    };
}

static void supervisor_task(void *arg)
{
    (void)arg;
    while (true) {
        connectivity_snapshot_t snapshot = take_snapshot();
        uint64_t now_ms = uptime_ms();
        bool emit_transition = false;
        bool emit_action = false;
        char action[24] = "";
        connectivity_decision_t decision;

        xSemaphoreTake(s_mutex, portMAX_DELAY);

        bool mqtt_healthy =
            connectivity_policy_mqtt_healthy(now_ms, &snapshot);
        bool cancel_attempt =
            snapshot.ota_active ||
            snapshot.wifi_working ||
            !snapshot.registered ||
            !snapshot.packet_attached;

        if (s_status.dry_run_attempt_pending) {
            if (mqtt_healthy) {
                connectivity_policy_note_redial_result(
                    &s_policy, now_ms, true);
                s_status.dry_run_attempt_pending = false;
            } else if (cancel_attempt) {
                connectivity_policy_cancel_attempt(&s_policy);
                s_status.dry_run_attempt_pending = false;
            } else if (s_policy.recovery_deadline_set &&
                       now_ms >= s_policy.recovery_deadline_ms) {
                connectivity_policy_note_redial_result(
                    &s_policy, now_ms, false);
                s_status.dry_run_attempt_pending = false;
            }
        }

        decision =
            connectivity_policy_evaluate(&s_policy, now_ms, &snapshot);
        if (!s_status.initialized || decision != s_status.decision) {
            s_status.transition_count++;
            s_status.last_transition_uptime_ms = now_ms;
            emit_transition = true;
        }

        if (decision == CONNECTIVITY_DECISION_REQUEST_REDIAL) {
            s_status.dry_run_attempt_pending = true;
            s_status.dry_run_redial_count++;
            s_status.last_would_action_uptime_ms = now_ms;
            strlcpy(s_status.would_action, "clean_redial",
                    sizeof(s_status.would_action));
            strlcpy(action, s_status.would_action, sizeof(action));
            emit_action = true;
        } else if (decision ==
                   CONNECTIVITY_DECISION_REQUEST_MODEM_RESTART) {
            s_status.dry_run_restart_count++;
            s_status.last_would_action_uptime_ms = now_ms;
            strlcpy(s_status.would_action, "modem_restart",
                    sizeof(s_status.would_action));
            strlcpy(action, s_status.would_action, sizeof(action));
            connectivity_policy_note_modem_restart(&s_policy, now_ms);
            emit_action = true;
        }

        s_status.decision = decision;
        s_status.snapshot = snapshot;
        s_status.initialized = true;
        s_status.evaluation_count++;
        s_status.last_evaluation_uptime_ms = now_ms;
        xSemaphoreGive(s_mutex);

        const char *decision_name = connectivity_decision_name(decision);
        if (emit_transition) {
            char details[96];
            snprintf(details, sizeof(details),
                     "{\"dry_run\":true,\"decision\":\"%s\"}",
                     decision_name);
            event_journal_emit(
                "recovery", "decision", EVENT_SEVERITY_INFO,
                decision_name, details, true, 0
            );
        }
        if (emit_action) {
            char details[96];
            snprintf(details, sizeof(details),
                     "{\"dry_run\":true,\"action\":\"%s\"}", action);
            event_journal_emit(
                "recovery", "would_request", EVENT_SEVERITY_WARN,
                action, details, true, 0
            );
        }

        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_TICK_MS));
    }
}

void connectivity_supervisor_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);
    connectivity_policy_init(&s_policy);
    strlcpy(s_status.would_action, "none",
            sizeof(s_status.would_action));
    xTaskCreate(
        supervisor_task, "connectivity", 4096, NULL, 3, NULL
    );
}

void connectivity_supervisor_status_json(cJSON *root)
{
    supervisor_status_t status = {0};
    connectivity_policy_t policy = {0};
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        status = s_status;
        policy = s_policy;
        xSemaphoreGive(s_mutex);
    }

    cJSON *recovery =
        cJSON_GetObjectItemCaseSensitive(root, "recovery");
    if (!cJSON_IsObject(recovery)) {
        recovery = cJSON_AddObjectToObject(root, "recovery");
        cJSON_AddBoolToObject(
            recovery, "automatic_actions_enabled", false);
    }

    const char *decision = status.initialized
                         ? connectivity_decision_name(status.decision)
                         : "initializing";
    if (cJSON_HasObjectItem(recovery, "state")) {
        cJSON_ReplaceItemInObject(
            recovery, "state", cJSON_CreateString(decision));
    } else {
        cJSON_AddStringToObject(recovery, "state", decision);
    }
    cJSON_AddStringToObject(recovery, "mode", "dry_run");
    cJSON_AddBoolToObject(
        recovery, "clean_redial_enabled", CLEAN_REDIAL_ENABLED);
    cJSON_AddBoolToObject(
        recovery, "modem_reset_escalation_enabled",
        MODEM_RESET_ESCALATION_ENABLED);
    cJSON_AddStringToObject(
        recovery, "would_action", status.would_action);
    cJSON_AddBoolToObject(
        recovery, "dry_run_attempt_pending",
        status.dry_run_attempt_pending);
    cJSON_AddNumberToObject(
        recovery, "evaluation_count", status.evaluation_count);
    cJSON_AddNumberToObject(
        recovery, "transition_count", status.transition_count);
    cJSON_AddNumberToObject(
        recovery, "dry_run_redial_count",
        status.dry_run_redial_count);
    cJSON_AddNumberToObject(
        recovery, "dry_run_restart_count",
        status.dry_run_restart_count);
    cJSON_AddNumberToObject(
        recovery, "failed_redials", policy.failed_redials);
    cJSON_AddNumberToObject(
        recovery, "last_evaluation_uptime_ms",
        (double)status.last_evaluation_uptime_ms);
    cJSON_AddNumberToObject(
        recovery, "last_transition_uptime_ms",
        (double)status.last_transition_uptime_ms);
    cJSON_AddNumberToObject(
        recovery, "last_would_action_uptime_ms",
        (double)status.last_would_action_uptime_ms);
    cJSON_AddNumberToObject(
        recovery, "recovery_deadline_uptime_ms",
        policy.recovery_deadline_set
            ? (double)policy.recovery_deadline_ms : 0);
    cJSON_AddNumberToObject(
        recovery, "last_restart_uptime_ms",
        policy.last_restart_set ? (double)policy.last_restart_ms : 0);

    cJSON *snapshot =
        cJSON_AddObjectToObject(recovery, "snapshot");
    cJSON_AddBoolToObject(
        snapshot, "wifi_working", status.snapshot.wifi_working);
    cJSON_AddBoolToObject(
        snapshot, "registered", status.snapshot.registered);
    cJSON_AddBoolToObject(
        snapshot, "packet_attached", status.snapshot.packet_attached);
    cJSON_AddBoolToObject(
        snapshot, "ppp_up", status.snapshot.ppp_up);
    cJSON_AddBoolToObject(
        snapshot, "mqtt_connected", status.snapshot.mqtt_connected);
    cJSON_AddBoolToObject(
        snapshot, "puback_seen", status.snapshot.puback_seen);
    cJSON_AddNumberToObject(
        snapshot, "last_puback_uptime_ms",
        (double)status.snapshot.last_puback_ms);
    cJSON_AddBoolToObject(
        snapshot, "modem_responsive",
        status.snapshot.modem_responsive);
    cJSON_AddBoolToObject(
        snapshot, "ota_active", status.snapshot.ota_active);
}
