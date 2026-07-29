#pragma once

#include <stdbool.h>
#include <stdint.h>


#define OTA_AP_QUIET_PERIOD_MS (5U * 60U * 1000U)

typedef enum {
    OTA_POLICY_PROCEED = 0,
    OTA_POLICY_DEFER_AP_CLIENT,
    OTA_POLICY_DEFER_AP_QUIET,
} ota_policy_decision_t;

typedef struct {
    bool routine_check;
    bool softap_active;
    uint32_t ap_client_count;
    uint64_t last_ap_association_uptime_ms;
    uint64_t last_ap_disassociation_uptime_ms;
    uint64_t now_uptime_ms;
} ota_policy_snapshot_t;

// Protect the local control plane from background TLS/OTA work. Explicit
// user-requested checks are never deferred. Routine checks wait while a
// SoftAP client is present and for a bounded quiet period after the last
// client leaves.
ota_policy_decision_t ota_policy_evaluate(
    const ota_policy_snapshot_t *snapshot
);
