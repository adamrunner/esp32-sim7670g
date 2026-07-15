#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "cJSON.h"

// Wall-clock time for timestamped telemetry. Two sources, best wins:
//  - SNTP over whichever link is up (WiFi STA or cellular PPP), started as
//    soon as a default route appears and refreshed on lwIP's usual schedule.
//  - GNSS UTC from the modem's +CGNSSINFO fix, used to seed the clock while
//    SNTP hasn't landed yet (good to a couple of seconds, plenty for logs).
// Until either source lands, time(NULL) is meaningless and timesync_valid()
// is false; telemetry rows carry timestamp 0 in that window.

typedef enum {
    TIMESYNC_NONE = 0,  // clock not set since boot
    TIMESYNC_GNSS,      // seeded from a GNSS fix
    TIMESYNC_SNTP,      // set by SNTP (authoritative)
} timesync_source_t;

typedef struct {
    timesync_source_t source;
    time_t synced_at;       // wall-clock time of the last sync event
    int64_t synced_at_us;   // esp_timer time of the last sync event
} timesync_status_t;

// Start the background task. Requires esp_netif + default event loop.
void timesync_init(void);

// True once the system clock has been set from any source since boot.
bool timesync_valid(void);

void timesync_get_status(timesync_status_t *out);

// Append the "time" object to the shared /api/status response.
void timesync_status_json(cJSON *root);
