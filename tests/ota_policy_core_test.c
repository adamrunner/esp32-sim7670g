#include <assert.h>
#include <stdio.h>

#include "ota_policy_core.h"


static ota_policy_snapshot_t routine_softap(void)
{
    ota_policy_snapshot_t snapshot = {
        .routine_check = true,
        .softap_active = true,
        .now_uptime_ms = 600000,
    };
    return snapshot;
}

static void test_explicit_check_is_never_deferred(void)
{
    ota_policy_snapshot_t snapshot = routine_softap();
    snapshot.routine_check = false;
    snapshot.ap_client_count = 1;
    assert(ota_policy_evaluate(&snapshot) == OTA_POLICY_PROCEED);
}

static void test_active_ap_client_defers_routine_check(void)
{
    ota_policy_snapshot_t snapshot = routine_softap();
    snapshot.ap_client_count = 1;
    assert(ota_policy_evaluate(&snapshot) ==
           OTA_POLICY_DEFER_AP_CLIENT);
}

static void test_recent_ap_client_defers_during_quiet_period(void)
{
    ota_policy_snapshot_t snapshot = routine_softap();
    snapshot.last_ap_association_uptime_ms = 100000;
    snapshot.last_ap_disassociation_uptime_ms = 110000;
    snapshot.now_uptime_ms =
        110000 + OTA_AP_QUIET_PERIOD_MS - 1;
    assert(ota_policy_evaluate(&snapshot) ==
           OTA_POLICY_DEFER_AP_QUIET);
}

static void test_routine_check_proceeds_after_quiet_period(void)
{
    ota_policy_snapshot_t snapshot = routine_softap();
    snapshot.last_ap_association_uptime_ms = 100000;
    snapshot.last_ap_disassociation_uptime_ms = 110000;
    snapshot.now_uptime_ms = 110000 + OTA_AP_QUIET_PERIOD_MS;
    assert(ota_policy_evaluate(&snapshot) == OTA_POLICY_PROCEED);
}

static void test_never_used_ap_does_not_block_ota(void)
{
    ota_policy_snapshot_t snapshot = routine_softap();
    assert(ota_policy_evaluate(&snapshot) == OTA_POLICY_PROCEED);
}

static void test_home_wifi_does_not_block_ota(void)
{
    ota_policy_snapshot_t snapshot = routine_softap();
    snapshot.softap_active = false;
    snapshot.ap_client_count = 1;
    assert(ota_policy_evaluate(&snapshot) == OTA_POLICY_PROCEED);
}

static void test_out_of_order_client_timestamps_defer(void)
{
    ota_policy_snapshot_t snapshot = routine_softap();
    snapshot.last_ap_association_uptime_ms = 200000;
    snapshot.last_ap_disassociation_uptime_ms = 190000;
    assert(ota_policy_evaluate(&snapshot) ==
           OTA_POLICY_DEFER_AP_QUIET);
}

int main(void)
{
    test_explicit_check_is_never_deferred();
    test_active_ap_client_defers_routine_check();
    test_recent_ap_client_defers_during_quiet_period();
    test_routine_check_proceeds_after_quiet_period();
    test_never_used_ap_does_not_block_ota();
    test_home_wifi_does_not_block_ota();
    test_out_of_order_client_timestamps_defer();
    puts("OTA policy core tests: ok");
    return 0;
}
