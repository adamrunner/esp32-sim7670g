#pragma once

#include "cJSON.h"


// Start the Phase 2 policy task. It evaluates live snapshots and records the
// actions it would request, but both clean redial and modem-reset escalation
// remain hard-disabled until separate hardware acceptance gates pass.
void connectivity_supervisor_init(void);

// Extend the existing additive "recovery" object in /api/status.
void connectivity_supervisor_status_json(cJSON *root);
