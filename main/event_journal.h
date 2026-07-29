#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"


#define EVENT_JOURNAL_RING_CAPACITY 48
#define EVENT_JOURNAL_FILE_BYTES (128U * 1024U)
#define EVENT_JOURNAL_FILE_COUNT 4U

typedef enum {
    EVENT_SEVERITY_DEBUG = 0,
    EVENT_SEVERITY_INFO,
    EVENT_SEVERITY_WARN,
    EVENT_SEVERITY_ERROR,
    EVENT_SEVERITY_CRITICAL,
} event_severity_t;

typedef struct {
    uint32_t emitted;
    uint32_t queued;
    uint32_t queue_dropped;
    uint32_t rate_limited;
    uint32_t journal_written;
    uint32_t journal_failures;
    uint32_t repaired_tails;
    int last_errno;
    uint32_t last_write_uptime_ms;
    char boot_id[17];
} event_journal_status_t;

typedef bool (*event_journal_json_visitor_t)(
    const cJSON *event,
    size_t index,
    void *context
);

// Start the non-blocking RAM ring and the dedicated SD writer. Call after
// sdcard_init(). Failure to mount or write the SD journal is non-fatal.
void event_journal_init(void);

// Add an event to the RAM ring and queue it for the SD worker without waiting.
// details_json must be a small JSON object; sensitive keys are redacted before
// either sink sees it. interval_ms=0 disables rate limiting for this event.
void event_journal_emit(
    const char *component,
    const char *event_name,
    event_severity_t severity,
    const char *reason,
    const char *details_json,
    bool critical,
    uint32_t interval_ms
);

// Record that wall time became valid and name the source ("sntp" or "gnss").
void event_journal_note_time_sync(const char *source);

void event_journal_get_status(event_journal_status_t *out);
void event_journal_status_json(cJSON *root);

// Visit up to `limit` chronological RAM-ring events. Each cJSON event remains
// valid only for the duration of the callback and is freed before the next
// event is built, bounding peak heap independently of the requested limit.
// Returns false if an event allocation or the visitor fails.
bool event_journal_visit_events_json(
    size_t limit,
    event_journal_json_visitor_t visitor,
    void *context
);

const char *event_journal_boot_id(void);
