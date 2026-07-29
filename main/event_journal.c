#include "event_journal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "event_journal_core.h"
#include "sdcard.h"


static const char *TAG = "events";

#define EVENT_SCHEMA_VERSION 1
#define EVENT_QUEUE_DEPTH 24
#define EVENT_RATE_SLOTS 24
#define EVENT_DIRECTORY SDCARD_MOUNT_POINT "/events"
#define EVENT_DETAILS_MAX 192

typedef struct {
    uint32_t sequence;
    uint64_t uptime_ms;
    int64_t wall_time;
    bool wall_valid;
    bool critical;
    event_severity_t severity;
    char time_source[8];
    char component[16];
    char event_name[32];
    char reason[48];
    char details[EVENT_DETAILS_MAX];
} event_record_t;

typedef struct {
    bool used;
    uint64_t last_ms;
    char component[16];
    char event_name[32];
} event_rate_slot_t;

static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_queue;
static event_record_t s_ring[EVENT_JOURNAL_RING_CAPACITY];
static event_record_t s_api_snapshot[EVENT_JOURNAL_RING_CAPACITY];
static size_t s_ring_head;
static size_t s_ring_count;
static event_rate_slot_t s_rate_slots[EVENT_RATE_SLOTS];
static event_journal_status_t s_status;
static uint32_t s_next_sequence;
static bool s_initialized;
static bool s_time_valid;
static char s_time_source[8];
// A schema-v1 event is bounded by EVENT_DETAILS_MAX and fits comfortably in
// this fixed buffer. Avoiding cJSON_PrintUnformatted() removes a second
// contiguous heap allocation from the low-priority persistence path.
static char s_journal_json[1024];


static const char *severity_string(event_severity_t severity)
{
    switch (severity) {
    case EVENT_SEVERITY_DEBUG:    return "debug";
    case EVENT_SEVERITY_WARN:     return "warn";
    case EVENT_SEVERITY_ERROR:    return "error";
    case EVENT_SEVERITY_CRITICAL: return "critical";
    default:                      return "info";
    }
}

static bool is_rate_limited_locked(
    const char *component,
    const char *event_name,
    uint64_t now_ms,
    uint32_t interval_ms
)
{
    if (interval_ms == 0) {
        return false;
    }
    event_rate_slot_t *available = NULL;
    for (size_t i = 0; i < EVENT_RATE_SLOTS; i++) {
        event_rate_slot_t *slot = &s_rate_slots[i];
        if (!slot->used && !available) {
            available = slot;
        }
        if (slot->used &&
            strcmp(slot->component, component) == 0 &&
            strcmp(slot->event_name, event_name) == 0) {
            if (event_core_rate_limited(
                    true, slot->last_ms, now_ms, interval_ms)) {
                return true;
            }
            slot->last_ms = now_ms;
            return false;
        }
    }
    if (!available) {
        // Deterministic bounded replacement; losing a limiter is preferable to
        // blocking an event producer or allocating memory.
        available = &s_rate_slots[s_next_sequence % EVENT_RATE_SLOTS];
    }
    available->used = true;
    available->last_ms = now_ms;
    strlcpy(available->component, component, sizeof(available->component));
    strlcpy(available->event_name, event_name, sizeof(available->event_name));
    return false;
}

static cJSON *record_json(const event_record_t *record)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddNumberToObject(root, "schema_version", EVENT_SCHEMA_VERSION);
    cJSON_AddStringToObject(root, "boot_id", s_status.boot_id);
    cJSON_AddNumberToObject(root, "event_sequence", record->sequence);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)record->uptime_ms);
    if (record->wall_valid) {
        cJSON_AddNumberToObject(root, "wall_time", (double)record->wall_time);
        cJSON_AddStringToObject(root, "time_source", record->time_source);
    } else {
        cJSON_AddNullToObject(root, "wall_time");
        cJSON_AddStringToObject(root, "time_source", "none");
    }
    cJSON_AddStringToObject(root, "component", record->component);
    cJSON_AddStringToObject(root, "event", record->event_name);
    cJSON_AddStringToObject(root, "severity", severity_string(record->severity));
    cJSON_AddStringToObject(root, "reason", record->reason);
    cJSON *details = cJSON_Parse(record->details);
    if (details && cJSON_IsObject(details)) {
        cJSON_AddItemToObject(root, "details", details);
    } else {
        cJSON_Delete(details);
        cJSON_AddObjectToObject(root, "details");
    }
    return root;
}

static void journal_task(void *argument)
{
    event_record_t record;
    int64_t last_failure_log_us = 0;
    while (true) {
        if (xQueueReceive(s_queue, &record, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        cJSON *root = record_json(&record);
        bool encoded =
            root && cJSON_PrintPreallocated(
                root, s_journal_json, sizeof(s_journal_json), false
            );
        cJSON_Delete(root);
        if (!encoded) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.journal_failures++;
            s_status.last_errno = ENOMEM;
            xSemaphoreGive(s_mutex);
            continue;
        }

        int result = -1;
        int saved_errno = ENODEV;
        if (sdcard_mounted()) {
            result = event_core_append(
                EVENT_DIRECTORY,
                s_journal_json,
                strlen(s_journal_json),
                record.critical,
                EVENT_JOURNAL_FILE_BYTES,
                EVENT_JOURNAL_FILE_COUNT
            );
            saved_errno = result == 0 ? 0 : errno;
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (result == 0) {
            s_status.journal_written++;
            s_status.last_write_uptime_ms = (uint32_t)record.uptime_ms;
            s_status.last_errno = 0;
        } else {
            s_status.journal_failures++;
            s_status.last_errno = saved_errno;
        }
        xSemaphoreGive(s_mutex);

        if (result != 0) {
            int64_t now_us = esp_timer_get_time();
            if (!last_failure_log_us ||
                now_us - last_failure_log_us >= 60LL * 1000000) {
                ESP_LOGW(
                    TAG,
                    "event journal append failed (%d); runtime continues",
                    saved_errno
                );
                last_failure_log_us = now_us;
            }
        }
    }
}

void event_journal_emit(
    const char *component,
    const char *event_name,
    event_severity_t severity,
    const char *reason,
    const char *details_json,
    bool critical,
    uint32_t interval_ms
)
{
    if (!s_initialized || !component || !event_name) {
        return;
    }

    event_record_t record = {0};
    record.uptime_ms = (uint64_t)esp_timer_get_time() / 1000U;
    record.severity = severity;
    record.critical = critical;
    strlcpy(record.component, component, sizeof(record.component));
    strlcpy(record.event_name, event_name, sizeof(record.event_name));
    strlcpy(record.reason, reason ? reason : "", sizeof(record.reason));
    const char *details = details_json && details_json[0] ? details_json : "{}";
    if (!event_core_redact_json(
            details, record.details, sizeof(record.details))) {
        strlcpy(record.details, "{\"redaction_error\":true}",
                sizeof(record.details));
    }

    // A zero-time event remains ordered by boot ID plus event sequence; wall
    // time is presentation metadata and is never used to order the ring.
    time_t wall_now = time(NULL);
    record.wall_valid = s_time_valid && wall_now > 1609459200;
    record.wall_time = record.wall_valid ? wall_now : 0;
    strlcpy(
        record.time_source,
        record.wall_valid ? s_time_source : "none",
        sizeof(record.time_source)
    );

    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        __atomic_fetch_add(&s_status.queue_dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    if (is_rate_limited_locked(
            record.component, record.event_name, record.uptime_ms,
            interval_ms)) {
        s_status.rate_limited++;
        xSemaphoreGive(s_mutex);
        return;
    }
    record.sequence = ++s_next_sequence;
    s_status.emitted++;
    s_ring[s_ring_head] = record;
    s_ring_head = (s_ring_head + 1) % EVENT_JOURNAL_RING_CAPACITY;
    if (s_ring_count < EVENT_JOURNAL_RING_CAPACITY) {
        s_ring_count++;
    }
    xSemaphoreGive(s_mutex);

    BaseType_t queued = xQueueSend(s_queue, &record, 0);
    if (queued != pdTRUE && critical) {
        // Preserve the newest critical transition under pressure by evicting
        // one older queued event. Both operations remain zero-wait.
        event_record_t discarded;
        if (xQueueReceive(s_queue, &discarded, 0) == pdTRUE) {
            __atomic_fetch_add(
                &s_status.queue_dropped, 1, __ATOMIC_RELAXED
            );
            queued = xQueueSend(s_queue, &record, 0);
        }
    }
    if (queued == pdTRUE) {
        __atomic_fetch_add(&s_status.queued, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&s_status.queue_dropped, 1, __ATOMIC_RELAXED);
    }
}

void event_journal_note_time_sync(const char *source)
{
    if (!s_initialized) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_time_valid = true;
    strlcpy(s_time_source, source ? source : "unknown", sizeof(s_time_source));
    xSemaphoreGive(s_mutex);
    char details[64];
    snprintf(details, sizeof(details), "{\"source\":\"%s\"}",
             source ? source : "unknown");
    event_journal_emit(
        "time", "synchronized", EVENT_SEVERITY_INFO, "clock_valid",
        details, true, 0
    );
}

void event_journal_get_status(event_journal_status_t *out)
{
    if (!out) {
        return;
    }
    if (!s_initialized) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mutex);
}

void event_journal_status_json(cJSON *root)
{
    event_journal_status_t status;
    event_journal_get_status(&status);
    cJSON *journal = cJSON_AddObjectToObject(root, "event_journal");
    cJSON_AddStringToObject(journal, "boot_id", status.boot_id);
    cJSON_AddStringToObject(journal, "directory", EVENT_DIRECTORY);
    cJSON_AddBoolToObject(journal, "sd_mounted", sdcard_mounted());
    cJSON_AddNumberToObject(journal, "ring_capacity",
                            EVENT_JOURNAL_RING_CAPACITY);
    cJSON_AddNumberToObject(journal, "file_bytes", EVENT_JOURNAL_FILE_BYTES);
    cJSON_AddNumberToObject(journal, "file_count", EVENT_JOURNAL_FILE_COUNT);
    cJSON_AddNumberToObject(journal, "emitted", status.emitted);
    cJSON_AddNumberToObject(journal, "queued", status.queued);
    cJSON_AddNumberToObject(journal, "queue_dropped", status.queue_dropped);
    cJSON_AddNumberToObject(journal, "rate_limited", status.rate_limited);
    cJSON_AddNumberToObject(journal, "written", status.journal_written);
    cJSON_AddNumberToObject(journal, "write_failures",
                            status.journal_failures);
    cJSON_AddNumberToObject(journal, "repaired_tails",
                            status.repaired_tails);
    cJSON_AddNumberToObject(journal, "last_errno", status.last_errno);
    cJSON_AddNumberToObject(journal, "last_write_uptime_ms",
                            status.last_write_uptime_ms);
}

bool event_journal_visit_events_json(
    size_t limit,
    event_journal_json_visitor_t visitor,
    void *context
)
{
    if (!visitor) {
        return false;
    }
    if (!s_initialized) {
        return true;
    }
    if (limit == 0 || limit > EVENT_JOURNAL_RING_CAPACITY) {
        limit = EVENT_JOURNAL_RING_CAPACITY;
    }
    size_t count;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    count = s_ring_count < limit ? s_ring_count : limit;
    size_t oldest = (s_ring_head + EVENT_JOURNAL_RING_CAPACITY - count) %
                    EVENT_JOURNAL_RING_CAPACITY;
    for (size_t i = 0; i < count; i++) {
        s_api_snapshot[i] =
            s_ring[(oldest + i) % EVENT_JOURNAL_RING_CAPACITY];
    }
    xSemaphoreGive(s_mutex);

    for (size_t i = 0; i < count; i++) {
        cJSON *event = record_json(&s_api_snapshot[i]);
        if (!event) {
            return false;
        }
        bool accepted = visitor(event, i, context);
        cJSON_Delete(event);
        if (!accepted) {
            return false;
        }
    }
    return true;
}

const char *event_journal_boot_id(void)
{
    return s_status.boot_id;
}

void event_journal_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(event_record_t));
    if (!s_mutex || !s_queue) {
        ESP_LOGE(TAG, "could not initialize event journal");
        return;
    }
    snprintf(
        s_status.boot_id, sizeof(s_status.boot_id),
        "%08" PRIx32 "%08" PRIx32, esp_random(), esp_random()
    );
    s_initialized = true;

    if (sdcard_mounted()) {
        char active_path[96];
        snprintf(active_path, sizeof(active_path), "%s/events.jsonl",
                 EVENT_DIRECTORY);
        int repaired = event_core_repair_tail(active_path);
        if (repaired > 0) {
            s_status.repaired_tails++;
        } else if (repaired < 0) {
            s_status.journal_failures++;
            s_status.last_errno = errno;
        }
    }
    xTaskCreate(journal_task, "event_journal", 4096, NULL, 2, NULL);

    char details[80];
    snprintf(
        details, sizeof(details),
        "{\"reset_reason\":%d,\"sd_mounted\":%s}",
        (int)esp_reset_reason(), sdcard_mounted() ? "true" : "false"
    );
    event_journal_emit(
        "system", "boot", EVENT_SEVERITY_INFO, "startup", details, true, 0
    );
}
