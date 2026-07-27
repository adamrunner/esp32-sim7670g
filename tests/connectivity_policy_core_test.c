#include <assert.h>
#include <stdio.h>

#include "connectivity_policy_core.h"


static connectivity_snapshot_t default_snapshot(void)
{
    return (connectivity_snapshot_t) {
        .registered = true,
        .packet_attached = true,
        .ppp_up = true,
        .modem_responsive = true,
    };
}

static void test_no_coverage_and_wifi_suppress_actions(void)
{
    connectivity_policy_t policy;
    connectivity_policy_init(&policy);
    connectivity_snapshot_t snapshot = default_snapshot();
    snapshot.registered = false;
    snapshot.packet_attached = false;
    snapshot.ppp_up = false;
    assert(connectivity_policy_evaluate(&policy, 1000, &snapshot) ==
           CONNECTIVITY_DECISION_WAITING_FOR_COVERAGE);
    assert(policy.failed_redials == 0);

    snapshot = default_snapshot();
    snapshot.wifi_working = true;
    assert(connectivity_policy_evaluate(&policy, 2000, &snapshot) ==
           CONNECTIVITY_DECISION_WIFI_UPLINK_HEALTHY);
}

static void test_stale_path_redial_escalation_and_cooldown(void)
{
    connectivity_policy_t policy;
    connectivity_policy_init(&policy);
    connectivity_snapshot_t snapshot = default_snapshot();

    assert(connectivity_policy_evaluate(&policy, 1000000, &snapshot) ==
           CONNECTIVITY_DECISION_REQUEST_REDIAL);
    assert(connectivity_policy_evaluate(&policy, 1010000, &snapshot) ==
           CONNECTIVITY_DECISION_WAITING_FOR_RECOVERY);

    connectivity_policy_note_redial_result(&policy, 1090000, false);
    assert(connectivity_policy_evaluate(&policy, 1090000, &snapshot) ==
           CONNECTIVITY_DECISION_REQUEST_REDIAL);
    connectivity_policy_note_redial_result(&policy, 1180000, false);
    assert(connectivity_policy_evaluate(&policy, 1180000, &snapshot) ==
           CONNECTIVITY_DECISION_REQUEST_MODEM_RESTART);

    connectivity_policy_note_modem_restart(&policy, 1180000);
    assert(connectivity_policy_evaluate(
               &policy,
               1180000 + CONNECTIVITY_RECOVERY_ATTEMPT_MS,
               &snapshot) == CONNECTIVITY_DECISION_RESTART_COOLDOWN);
    assert(connectivity_policy_evaluate(
               &policy,
               1180000 + CONNECTIVITY_MODEM_RESTART_COOLDOWN_MS,
               &snapshot) == CONNECTIVITY_DECISION_REQUEST_MODEM_RESTART);
}

static void test_stable_recovery_and_ota_hold(void)
{
    connectivity_policy_t policy;
    connectivity_policy_init(&policy);
    policy.failed_redials = 2;
    connectivity_snapshot_t snapshot = default_snapshot();
    snapshot.mqtt_connected = true;
    snapshot.puback_seen = true;
    snapshot.last_puback_ms = 1000000;

    assert(connectivity_policy_evaluate(&policy, 1000000, &snapshot) ==
           CONNECTIVITY_DECISION_HEALTHY);
    assert(policy.failed_redials == 2);
    snapshot.last_puback_ms = 1000000 + CONNECTIVITY_STABLE_RECOVERY_MS;
    assert(connectivity_policy_evaluate(
               &policy,
               1000000 + CONNECTIVITY_STABLE_RECOVERY_MS,
               &snapshot) == CONNECTIVITY_DECISION_HEALTHY);
    assert(policy.failed_redials == 0);

    snapshot.ota_active = true;
    assert(connectivity_policy_evaluate(
               &policy, 2000000, &snapshot) ==
           CONNECTIVITY_DECISION_OTA_ACTIVE);
}

int main(void)
{
    test_no_coverage_and_wifi_suppress_actions();
    test_stale_path_redial_escalation_and_cooldown();
    test_stable_recovery_and_ota_hold();
    puts("connectivity policy core tests: ok");
    return 0;
}
