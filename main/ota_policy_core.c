#include "ota_policy_core.h"


ota_policy_decision_t ota_policy_evaluate(
    const ota_policy_snapshot_t *snapshot
)
{
    if (!snapshot || !snapshot->routine_check) {
        return OTA_POLICY_PROCEED;
    }
    if (snapshot->last_http_request_uptime_ms > 0) {
        if (snapshot->now_uptime_ms <
            snapshot->last_http_request_uptime_ms) {
            return OTA_POLICY_DEFER_HTTP_QUIET;
        }
        if (snapshot->now_uptime_ms -
                snapshot->last_http_request_uptime_ms <
            OTA_HTTP_QUIET_PERIOD_MS) {
            return OTA_POLICY_DEFER_HTTP_QUIET;
        }
    }
    if (!snapshot->softap_active) {
        return OTA_POLICY_PROCEED;
    }
    if (snapshot->ap_client_count > 0) {
        return OTA_POLICY_DEFER_AP_CLIENT;
    }
    if (snapshot->last_ap_association_uptime_ms == 0) {
        return OTA_POLICY_PROCEED;
    }

    // A missing/out-of-order disassociation timestamp is treated
    // conservatively as an active control-plane session.
    if (snapshot->last_ap_disassociation_uptime_ms <
        snapshot->last_ap_association_uptime_ms) {
        return OTA_POLICY_DEFER_AP_QUIET;
    }
    if (snapshot->now_uptime_ms <
        snapshot->last_ap_disassociation_uptime_ms) {
        return OTA_POLICY_DEFER_AP_QUIET;
    }
    if (snapshot->now_uptime_ms -
            snapshot->last_ap_disassociation_uptime_ms <
        OTA_AP_QUIET_PERIOD_MS) {
        return OTA_POLICY_DEFER_AP_QUIET;
    }
    return OTA_POLICY_PROCEED;
}
