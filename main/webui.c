#include "webui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "bms.h"
#include "board_battery.h"
#include "datalog.h"
#include "event_journal.h"
#include "modem.h"
#include "mqtt.h"
#include "ota.h"
#include "timesync.h"
#include "webui_json_stream.h"
#include "wifi.h"

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// Every "heap" quantity reported here is the *internal* pool. With PSRAM
// enabled, esp_get_free_heap_size() and MALLOC_CAP_8BIT queries span both
// pools, so an 8 MB external heap would mask the internal-RAM pressure these
// counters exist to measure and break continuity with the pre-PSRAM field
// baseline recorded in docs/LOCAL_WEBUI_STABILITY_PLAN.md. PSRAM is reported
// as its own fields instead. With PSRAM disabled these queries return exactly
// what the previous ones did, so the recorded baseline stays comparable.
#define WEBUI_INTERNAL_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

typedef struct {
    uint32_t request_count;
    uint32_t failure_count;
    uint32_t response_error_count;
    uint32_t serialization_failure_count;
    uint32_t stream_failure_count;
    uint32_t slow_request_count;
    uint32_t min_task_stack_free;
    uint32_t min_free_heap;
    uint32_t min_largest_free_block;
    uint32_t min_free_psram;
    uint32_t min_largest_free_psram_block;
    uint64_t last_request_uptime_ms;
    uint64_t last_success_uptime_ms;
    uint64_t last_error_uptime_ms;
    uint32_t last_duration_ms;
    int last_error;
    char last_uri[48];
    // Session/socket-layer evidence (LOCAL_WEBUI_STABILITY_PLAN.md Phase A).
    uint32_t sessions_open;
    uint32_t sessions_opened_total;
    uint32_t sessions_closed_total;
    uint32_t session_high_water;
    uint32_t session_table_full_count;
    // HTTP_SERVER_EVENT_ERROR fires for every 4xx/5xx the server sends
    // (its payload is an httpd_err_code_t), so it measures error *responses*
    // — mostly 404s for browser probes like /favicon.ico — not socket trouble.
    uint32_t error_response_count;
    int last_error_response_code;
    // Genuine transport failures: a response send() that failed because the
    // client vanished or stalled past send_wait_timeout.
    uint32_t transport_error_count;
    uint64_t last_transport_error_uptime_ms;
    uint32_t send_stall_count;
    uint32_t max_chunk_send_ms;
    uint32_t request_during_ota_count;
    uint32_t duration_le_100ms;
    uint32_t duration_le_500ms;
    uint32_t duration_le_1000ms;
    uint32_t duration_le_2500ms;
    uint32_t duration_gt_2500ms;
    // Web-service watchdog: liveness of the single server task, measured by
    // how long a queued heartbeat work item waits before it runs.
    uint32_t heartbeat_count;
    uint32_t heartbeat_max_latency_ms;
    uint32_t heartbeat_queue_failure_count;
    uint32_t heartbeat_stall_count;
    uint64_t last_heartbeat_uptime_ms;
    bool heartbeat_pending;
    bool heartbeat_stalled;
    int64_t heartbeat_queued_us;
} webui_observability_t;

// Client sessions the HTTP server may hold at once; the lwIP reservation
// arithmetic behind this value is documented at the httpd_config below.
#define WEBUI_MAX_CLIENT_SESSIONS 8
// TCP keepalive on accepted sessions: a phone that walks away or locks its
// screen without closing is probed after 5 s idle and dropped after three
// unanswered probes 2 s apart (~11 s), instead of holding its socket until
// LRU pressure reclaims it.
#define WEBUI_KEEPALIVE_IDLE_S 5
#define WEBUI_KEEPALIVE_INTERVAL_S 2
#define WEBUI_KEEPALIVE_COUNT 3
// Watchdog cadence and stall threshold. The threshold sits well above the
// slowest legitimate handler (/api/ping: DNS plus four 1 s + 3 s timeout
// probes and a 5 s margin, ~40 s worst case with mutex waits), so only a
// genuinely wedged server task trips it.
#define WEBUI_WATCHDOG_PERIOD_MS 10000
#define WEBUI_WATCHDOG_STALL_MS 60000
// One response chunk blocking in send() at least this long counts as a
// send stall (a slow, dead, or RF-degraded client pinning the HTTP task).
#define WEBUI_SEND_STALL_THRESHOLD_MS 500

static SemaphoreHandle_t s_http_mutex;
static webui_observability_t s_http_status;
// ESP-IDF's HTTP server invokes handlers on one server task. This per-request
// marker lets observed_handler distinguish a deliberately sent HTTP error
// from a successful application response; httpd_resp_send_err() itself
// returns ESP_OK when it successfully transmits a 4xx/5xx response.
static bool s_current_response_error;
static int s_current_http_status;
static esp_err_t s_current_response_error_code;
// Per-request send()-blocking accounting, same single-server-task pattern.
static uint32_t s_current_max_chunk_send_ms;

// Session lifecycle hooks. open_fn/close_fn run on the server task around
// accept() and session teardown, so they see every socket the server holds —
// including ones LRU purge reclaims. A full table is the precondition for
// purging a live browser connection, which is one suspected cause of field
// UI instability, so it is counted and journaled distinctly.
static esp_err_t session_open_fn(httpd_handle_t server, int sockfd)
{
    uint32_t open_count;
    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    s_http_status.sessions_open++;
    s_http_status.sessions_opened_total++;
    if (s_http_status.sessions_open > s_http_status.session_high_water) {
        s_http_status.session_high_water = s_http_status.sessions_open;
    }
    open_count = s_http_status.sessions_open;
    if (open_count >= WEBUI_MAX_CLIENT_SESSIONS) {
        s_http_status.session_table_full_count++;
    }
    xSemaphoreGive(s_http_mutex);

    if (open_count >= WEBUI_MAX_CLIENT_SESSIONS) {
        char details[96];
        snprintf(details, sizeof(details),
                 "{\"open\":%lu,\"max\":%d,\"heap\":%lu}",
                 (unsigned long)open_count, WEBUI_MAX_CLIENT_SESSIONS,
                 (unsigned long)heap_caps_get_free_size(
                     WEBUI_INTERNAL_CAPS));
        event_journal_emit(
            "http", "session_table_full", EVENT_SEVERITY_WARN,
            "lru_purge_possible", details, false, 30000
        );
    }
    return ESP_OK;
}

static void session_close_fn(httpd_handle_t server, int sockfd)
{
    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    if (s_http_status.sessions_open > 0) {
        s_http_status.sessions_open--;
    }
    s_http_status.sessions_closed_total++;
    xSemaphoreGive(s_http_mutex);
    // A custom close_fn owns the descriptor; the server will not close it.
    if (sockfd >= 0) {
        close(sockfd);
    }
}

static void http_server_event_handler(
    void *arg, esp_event_base_t base, int32_t id, void *data
)
{
    if (id != HTTP_SERVER_EVENT_ERROR) {
        return;
    }
    int code = data ? (int)*(httpd_err_code_t *)data : -1;
    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    s_http_status.error_response_count++;
    s_http_status.last_error_response_code = code;
    xSemaphoreGive(s_http_mutex);
    char details[48];
    snprintf(details, sizeof(details), "{\"err_code\":%d}", code);
    // Informational: handler failures already journal request_failed with
    // full resource detail; this mostly records probe 404s.
    event_journal_emit(
        "http", "error_response", EVENT_SEVERITY_INFO,
        "httpd_error_status", details, false, 30000
    );
}

uint64_t webui_last_request_uptime_ms(void)
{
    if (!s_http_mutex) {
        return 0;
    }
    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    uint64_t last_request = s_http_status.last_request_uptime_ms;
    xSemaphoreGive(s_http_mutex);
    return last_request;
}

static void update_resource_minimum(uint32_t *minimum, uint32_t value)
{
    if (*minimum == 0 || value < *minimum) {
        *minimum = value;
    }
}

static esp_err_t send_http_error(
    httpd_req_t *req,
    httpd_err_code_t status,
    int status_code,
    const char *message,
    esp_err_t diagnostic_error
)
{
    s_current_response_error = true;
    s_current_http_status = status_code;
    s_current_response_error_code = diagnostic_error;
    return httpd_resp_send_err(req, status, message);
}

static esp_err_t send_bad_request(httpd_req_t *req, const char *message)
{
    return send_http_error(
        req, HTTPD_400_BAD_REQUEST, 400, message, ESP_ERR_INVALID_ARG
    );
}

static esp_err_t send_internal_error(
    httpd_req_t *req,
    const char *message,
    esp_err_t diagnostic_error
)
{
    return send_http_error(
        req, HTTPD_500_INTERNAL_SERVER_ERROR, 500, message,
        diagnostic_error
    );
}

static void webui_status_json(cJSON *root)
{
    webui_observability_t status;
    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    status = s_http_status;
    xSemaphoreGive(s_http_mutex);
    cJSON *http = cJSON_AddObjectToObject(root, "http");
    cJSON_AddNumberToObject(http, "request_count", status.request_count);
    cJSON_AddNumberToObject(http, "failure_count", status.failure_count);
    cJSON_AddNumberToObject(http, "response_error_count",
                            status.response_error_count);
    cJSON_AddNumberToObject(http, "serialization_failure_count",
                            status.serialization_failure_count);
    cJSON_AddNumberToObject(http, "stream_failure_count",
                            status.stream_failure_count);
    cJSON_AddNumberToObject(http, "slow_request_count",
                            status.slow_request_count);
    cJSON_AddNumberToObject(http, "min_task_stack_free",
                            status.min_task_stack_free);
    cJSON_AddNumberToObject(http, "min_free_heap",
                            status.min_free_heap);
    cJSON_AddNumberToObject(http, "min_largest_free_block",
                            status.min_largest_free_block);
    // All-time internal low-water since boot, not just at request
    // boundaries: the number the Phase A capture could only recover from a
    // journaled failure detail (840 bytes on bba2413).
    cJSON_AddNumberToObject(
        http, "min_free_heap_all_time",
        (double)heap_caps_get_minimum_free_size(WEBUI_INTERNAL_CAPS)
    );
    // External pool. Zero on a build with PSRAM disabled.
    cJSON_AddNumberToObject(
        http, "psram_total",
        (double)heap_caps_get_total_size(MALLOC_CAP_SPIRAM)
    );
    cJSON_AddNumberToObject(
        http, "psram_free",
        (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
    );
    cJSON_AddNumberToObject(http, "min_free_psram", status.min_free_psram);
    cJSON_AddNumberToObject(http, "min_largest_free_psram_block",
                            status.min_largest_free_psram_block);
    cJSON_AddNumberToObject(
        http, "min_free_psram_all_time",
        (double)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)
    );
    cJSON_AddNumberToObject(http, "last_request_uptime_ms",
                            (double)status.last_request_uptime_ms);
    cJSON_AddNumberToObject(http, "last_success_uptime_ms",
                            (double)status.last_success_uptime_ms);
    cJSON_AddNumberToObject(http, "last_error_uptime_ms",
                            (double)status.last_error_uptime_ms);
    cJSON_AddNumberToObject(http, "last_duration_ms",
                            status.last_duration_ms);
    cJSON_AddNumberToObject(http, "last_error", status.last_error);
    cJSON_AddStringToObject(http, "last_uri", status.last_uri);
    cJSON_AddNumberToObject(http, "sessions_open", status.sessions_open);
    cJSON_AddNumberToObject(http, "sessions_opened_total",
                            status.sessions_opened_total);
    cJSON_AddNumberToObject(http, "sessions_closed_total",
                            status.sessions_closed_total);
    cJSON_AddNumberToObject(http, "session_high_water",
                            status.session_high_water);
    cJSON_AddNumberToObject(http, "session_table_full_count",
                            status.session_table_full_count);
    cJSON_AddNumberToObject(http, "error_response_count",
                            status.error_response_count);
    cJSON_AddNumberToObject(http, "last_error_response_code",
                            status.last_error_response_code);
    cJSON_AddNumberToObject(http, "transport_error_count",
                            status.transport_error_count);
    cJSON_AddNumberToObject(http, "last_transport_error_uptime_ms",
                            (double)status.last_transport_error_uptime_ms);
    cJSON_AddNumberToObject(http, "send_stall_count",
                            status.send_stall_count);
    cJSON_AddNumberToObject(http, "max_chunk_send_ms",
                            status.max_chunk_send_ms);
    cJSON_AddNumberToObject(http, "request_during_ota_count",
                            status.request_during_ota_count);
    cJSON *durations =
        cJSON_AddObjectToObject(http, "request_duration_counts");
    cJSON_AddNumberToObject(durations, "le_100ms",
                            status.duration_le_100ms);
    cJSON_AddNumberToObject(durations, "le_500ms",
                            status.duration_le_500ms);
    cJSON_AddNumberToObject(durations, "le_1000ms",
                            status.duration_le_1000ms);
    cJSON_AddNumberToObject(durations, "le_2500ms",
                            status.duration_le_2500ms);
    cJSON_AddNumberToObject(durations, "gt_2500ms",
                            status.duration_gt_2500ms);
    cJSON *watchdog = cJSON_AddObjectToObject(http, "watchdog");
    cJSON_AddNumberToObject(watchdog, "heartbeat_count",
                            status.heartbeat_count);
    cJSON_AddNumberToObject(watchdog, "heartbeat_max_latency_ms",
                            status.heartbeat_max_latency_ms);
    cJSON_AddNumberToObject(watchdog, "heartbeat_queue_failure_count",
                            status.heartbeat_queue_failure_count);
    cJSON_AddNumberToObject(watchdog, "stall_count",
                            status.heartbeat_stall_count);
    cJSON_AddBoolToObject(watchdog, "stalled", status.heartbeat_stalled);
    cJSON_AddNumberToObject(watchdog, "last_heartbeat_uptime_ms",
                            (double)status.last_heartbeat_uptime_ms);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

static bool s_reboot_pending;

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));  // let the HTTP response reach the browser
    esp_restart();
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    if (s_reboot_pending) {
        return send_bad_request(req, "reboot already pending");
    }

    s_reboot_pending = true;
    if (xTaskCreate(reboot_task, "web_reboot", 2048, NULL, 5, NULL) != pdPASS) {
        s_reboot_pending = false;
        return send_internal_error(
            req, "could not schedule reboot", ESP_ERR_NO_MEM
        );
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"reboot_in_ms\":1000}");
}

static bool ota_busy(void)
{
    ota_status_t st;
    ota_get_status(&st);
    return st.state == OTA_STATE_CHECKING ||
           st.state == OTA_STATE_DOWNLOADING ||
           st.state == OTA_STATE_VERIFYING ||
           st.state == OTA_STATE_WAIT_REBOOT;
}

static esp_err_t modem_restart_post_handler(httpd_req_t *req)
{
    if (ota_busy()) {
        return send_bad_request(
            req, "cannot restart modem while OTA is active"
        );
    }
    if (modem_request_restart() != ESP_OK) {
        return send_bad_request(req, "modem restart already active");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"state\":\"requested\"}");
}

typedef struct {
    httpd_req_t *req;
    esp_err_t error;
    bool sent;
    webui_json_stream_t encoder;
} json_stream_t;

static void record_serialization_failure(void)
{
    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    s_http_status.serialization_failure_count++;
    xSemaphoreGive(s_http_mutex);
}

static bool send_json_chunk(
    void *context,
    const char *data,
    size_t length
)
{
    json_stream_t *stream = context;
    if (stream->error != ESP_OK) {
        return false;
    }
    int64_t send_started_us = esp_timer_get_time();
    stream->error = httpd_resp_send_chunk(
        stream->req, data, length
    );
    uint32_t send_ms =
        (uint32_t)((esp_timer_get_time() - send_started_us) / 1000);
    if (send_ms > s_current_max_chunk_send_ms) {
        s_current_max_chunk_send_ms = send_ms;
    }
    if (stream->error == ESP_OK) {
        stream->sent = true;
    }
    return stream->error == ESP_OK;
}

static void json_stream_begin(json_stream_t *stream, httpd_req_t *req)
{
    memset(stream, 0, sizeof(*stream));
    stream->req = req;
    stream->error = ESP_OK;
    webui_json_stream_init(&stream->encoder, send_json_chunk, stream);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static bool json_stream_literal(json_stream_t *stream, const char *literal)
{
    return webui_json_stream_literal(&stream->encoder, literal);
}

static bool json_stream_value(json_stream_t *stream, const cJSON *item)
{
    return webui_json_stream_value(&stream->encoder, item);
}

static esp_err_t json_stream_finish(
    json_stream_t *stream,
    bool encoded
)
{
    if (encoded) {
        encoded = webui_json_stream_flush(&stream->encoder);
    }
    if (encoded) {
        stream->error = httpd_resp_send_chunk(stream->req, NULL, 0);
        encoded = stream->error == ESP_OK;
    }
    if (encoded) {
        return ESP_OK;
    }

    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    s_http_status.stream_failure_count++;
    xSemaphoreGive(s_http_mutex);
    esp_err_t diagnostic_error =
        stream->error == ESP_OK ? ESP_FAIL : stream->error;
    if (!stream->sent) {
        return send_internal_error(
            stream->req, "json encode failed", diagnostic_error
        );
    }
    s_current_response_error = true;
    s_current_http_status = 0;
    s_current_response_error_code = diagnostic_error;
    return diagnostic_error;
}

// Stream `root` in bounded chunks and then free the tree. Unlike
// cJSON_PrintUnformatted(), this never needs a second response-sized
// contiguous allocation. The schema and JSON types remain unchanged.
static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    if (!root) {
        record_serialization_failure();
        return send_http_error(
            req, HTTPD_500_INTERNAL_SERVER_ERROR, 500,
            "json object allocation failed", ESP_ERR_NO_MEM
        );
    }
    json_stream_t stream;
    json_stream_begin(&stream, req);
    bool encoded = json_stream_value(&stream, root);
    cJSON_Delete(root);
    return json_stream_finish(&stream, encoded);
}

// Each module still owns the schema and types of its status fields, but only
// one module fragment exists at a time. This preserves the aggregate response
// while bounding peak cJSON heap independently of the total document size.
static esp_err_t status_get_handler(httpd_req_t *req)
{
    static const webui_json_fragment_builder_t builders[] = {
        modem_status_json,
        board_battery_status_json,
        bms_status_json,
        mqtt_status_json,
        datalog_status_json,
        timesync_status_json,
        wifi_status_json,
        webui_status_json,
        event_journal_status_json,
    };
    json_stream_t stream;
    json_stream_begin(&stream, req);
    bool encoded = webui_json_stream_object_fragments(
        &stream.encoder, builders, sizeof(builders) / sizeof(builders[0])
    );
    if (stream.encoder.allocation_failed) {
        record_serialization_failure();
        stream.error = ESP_ERR_NO_MEM;
    }
    return json_stream_finish(&stream, encoded);
}

typedef struct {
    json_stream_t *stream;
    bool first;
} event_json_stream_context_t;

static bool stream_event_json(
    const cJSON *event,
    size_t index,
    void *context
)
{
    event_json_stream_context_t *event_stream = context;
    if ((index > 0 || !event_stream->first) &&
        !json_stream_literal(event_stream->stream, ",")) {
        return false;
    }
    event_stream->first = false;
    return json_stream_value(event_stream->stream, event);
}

static esp_err_t events_get_handler(httpd_req_t *req)
{
    size_t limit = EVENT_JOURNAL_RING_CAPACITY;
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0 && query_len < 64) {
        char query[64];
        char value[12];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
            httpd_query_key_value(
                query, "limit", value, sizeof(value)) == ESP_OK) {
            char *end = NULL;
            long requested = strtol(value, &end, 10);
            if (!end || *end != '\0' || requested < 1 ||
                requested > EVENT_JOURNAL_RING_CAPACITY) {
                return send_bad_request(req, "limit must be 1..48");
            }
            limit = (size_t)requested;
        }
    }

    json_stream_t stream;
    json_stream_begin(&stream, req);
    bool encoded = json_stream_literal(
        &stream,
        "{\"schema_version\":1,"
        "\"ordering\":\"boot_id,event_sequence\",\"events\":["
    );
    event_json_stream_context_t context = {
        .stream = &stream,
        .first = true,
    };
    if (encoded) {
        encoded = event_journal_visit_events_json(
            limit, stream_event_json, &context
        );
        if (!encoded && stream.error == ESP_OK) {
            record_serialization_failure();
            stream.error = ESP_ERR_NO_MEM;
        }
    }
    if (encoded) {
        encoded = json_stream_literal(&stream, "]}");
    }
    return json_stream_finish(&stream, encoded);
}

// Read and null-terminate a small JSON request body.
static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    if (req->content_len >= buf_len) {
        send_bad_request(req, "body too large");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, buf + received, req->content_len - received);
        if (n <= 0) {
            send_bad_request(req, "recv failed");
            return ESP_FAIL;
        }
        received += n;
    }
    buf[received] = '\0';
    return ESP_OK;
}

static esp_err_t apn_post_handler(httpd_req_t *req)
{
    char body[160];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    const cJSON *apn = root ? cJSON_GetObjectItem(root, "apn") : NULL;
    if (!cJSON_IsString(apn) || strlen(apn->valuestring) >= MODEM_APN_MAX) {
        cJSON_Delete(root);
        return send_bad_request(req, "expected {\"apn\":\"...\"}");
    }

    esp_err_t err = modem_set_apn(apn->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_internal_error(req, "NVS write failed", err);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t at_post_handler(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    const cJSON *cmd = root ? cJSON_GetObjectItem(root, "cmd") : NULL;
    if (!cJSON_IsString(cmd) || strlen(cmd->valuestring) == 0) {
        cJSON_Delete(root);
        return send_bad_request(req, "expected {\"cmd\":\"AT...\"}");
    }

    static char resp[2048];
    esp_err_t at_err = modem_send_at(cmd->valuestring, resp, sizeof(resp), 10000);
    cJSON_Delete(root);

    cJSON *out = cJSON_CreateObject();
    if (!out) {
        return send_json(req, out);
    }
    cJSON_AddBoolToObject(out, "ok", at_err == ESP_OK);
    cJSON_AddBoolToObject(out, "timeout", at_err == ESP_ERR_TIMEOUT);
    cJSON_AddStringToObject(out, "response", resp);
    return send_json(req, out);
}

static esp_err_t ping_post_handler(httpd_req_t *req)
{
    char body[192];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    const cJSON *host = root ? cJSON_GetObjectItem(root, "host") : NULL;
    if (!cJSON_IsString(host) || strlen(host->valuestring) == 0) {
        cJSON_Delete(root);
        return send_bad_request(req, "expected {\"host\":\"...\"}");
    }

    static modem_netdiag_t diag;  // ~1 KB; httpd serves requests one at a time
    esp_err_t err = modem_ping_host(host->valuestring, &diag);
    cJSON_Delete(root);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_bad_request(req, "invalid hostname");
    }

    cJSON *out = cJSON_CreateObject();
    if (!out) {
        return send_json(req, out);
    }
    cJSON_AddBoolToObject(out, "dns_ok", diag.dns_ok);
    cJSON_AddNumberToObject(out, "dns_err", diag.dns_err);
    cJSON *ips = cJSON_AddArrayToObject(out, "ips");
    for (int i = 0; i < diag.num_ips; i++) {
        cJSON_AddItemToArray(ips, cJSON_CreateString(diag.ips[i]));
    }
    cJSON_AddBoolToObject(out, "ping_ok", diag.ping_ok);
    cJSON_AddNumberToObject(out, "sent", diag.sent);
    cJSON_AddNumberToObject(out, "received", diag.received);
    cJSON_AddNumberToObject(out, "lost", diag.lost);
    cJSON_AddNumberToObject(out, "min_ms", diag.min_ms);
    cJSON_AddNumberToObject(out, "max_ms", diag.max_ms);
    cJSON_AddNumberToObject(out, "avg_ms", diag.avg_ms);
    cJSON_AddStringToObject(out, "raw", diag.raw);
    return send_json(req, out);
}

static const char *ota_state_str(ota_state_t s)
{
    switch (s) {
    case OTA_STATE_CHECKING:    return "checking";
    case OTA_STATE_DOWNLOADING: return "downloading";
    case OTA_STATE_VERIFYING:   return "verifying";
    case OTA_STATE_WAIT_REBOOT: return "wait_reboot";
    case OTA_STATE_ERROR:       return "error";
    default:                    return "idle";
    }
}

static esp_err_t ota_get_handler(httpd_req_t *req)
{
    ota_status_t st;
    ota_get_status(&st);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return send_json(req, root);
    }
    cJSON_AddStringToObject(root, "version", st.running_version);
    cJSON_AddStringToObject(root, "slot", st.running_slot);
    cJSON_AddStringToObject(root, "state", ota_state_str(st.state));
    cJSON_AddBoolToObject(root, "pending_verify", st.pending_verify);
    cJSON_AddBoolToObject(root, "update_available", st.update_available);
    cJSON_AddStringToObject(root, "available_version", st.available_version);
    cJSON_AddNumberToObject(root, "progress_pct", st.progress_pct);
    cJSON_AddNumberToObject(root, "bytes_read", st.bytes_read);
    cJSON_AddNumberToObject(root, "image_size", st.image_size);
    cJSON_AddStringToObject(root, "error", st.error);
    cJSON_AddBoolToObject(root, "last_check_ok", st.last_check_ok);
    cJSON_AddNumberToObject(root, "manifest_attempts", st.manifest_attempts);
    cJSON_AddNumberToObject(root, "download_attempts", st.download_attempts);
    cJSON_AddBoolToObject(
        root, "active_modem_recovery_enabled", false
    );
    cJSON_AddNumberToObject(
        root, "passive_retry_count", st.passive_retry_count
    );
    cJSON_AddNumberToObject(
        root, "control_plane_defer_count",
        st.control_plane_defer_count
    );
    cJSON_AddStringToObject(
        root, "last_control_plane_defer_reason",
        st.last_control_plane_defer_reason
    );
    if (st.last_control_plane_defer_us) {
        cJSON_AddNumberToObject(
            root, "last_control_plane_defer_age_s",
            (double)(
                (esp_timer_get_time() -
                 st.last_control_plane_defer_us) / 1000000
            )
        );
    }
    if (st.last_check_us) {
        cJSON_AddNumberToObject(root, "last_check_age_s",
                                (double)((esp_timer_get_time() - st.last_check_us) / 1000000));
    }
    if (st.next_check_us) {
        int64_t remaining_us = st.next_check_us - esp_timer_get_time();
        cJSON_AddNumberToObject(root, "next_check_in_s",
                                remaining_us > 0 ? (double)((remaining_us + 999999) / 1000000)
                                                 : 0);
    } else {
        cJSON_AddNullToObject(root, "next_check_in_s");
    }
    if (st.failure.stage[0]) {
        cJSON *failure = cJSON_AddObjectToObject(root, "failure");
        cJSON_AddStringToObject(failure, "stage", st.failure.stage);
        cJSON_AddNumberToObject(failure, "esp_err", st.failure.esp_err);
        cJSON_AddStringToObject(failure, "esp_err_name",
                               esp_err_to_name((esp_err_t)st.failure.esp_err));
        cJSON_AddNumberToObject(failure, "tls_err", st.failure.tls_err);
        cJSON_AddStringToObject(failure, "tls_err_name",
                               st.failure.tls_err
                                   ? esp_err_to_name((esp_err_t)st.failure.tls_err)
                                   : "");
        cJSON_AddNumberToObject(failure, "mbedtls_err", st.failure.mbedtls_err);
        cJSON_AddNumberToObject(failure, "tls_flags", st.failure.tls_flags);
        cJSON_AddNumberToObject(failure, "sock_errno", st.failure.sock_errno);
        cJSON_AddNumberToObject(failure, "free_heap", st.failure.free_heap);
        cJSON_AddNumberToObject(failure, "largest_free_block",
                               st.failure.largest_free_block);
        cJSON_AddNumberToObject(failure, "minimum_free_heap",
                               st.failure.minimum_free_heap);
    } else {
        cJSON_AddNullToObject(root, "failure");
    }
    cJSON_AddNumberToObject(root, "free_heap",
                            (double)heap_caps_get_free_size(WEBUI_INTERNAL_CAPS));
    cJSON_AddNumberToObject(root, "largest_free_block",
                            (double)heap_caps_get_largest_free_block(WEBUI_INTERNAL_CAPS));
    cJSON_AddNumberToObject(root, "minimum_free_heap",
                            (double)heap_caps_get_minimum_free_size(WEBUI_INTERNAL_CAPS));
    cJSON_AddNumberToObject(root, "free_psram",
                            (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return send_json(req, root);
}

static esp_err_t ota_check_post_handler(httpd_req_t *req)
{
    char body[384];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    // Body optional: {} or {"url":"https://...","transport":"cell"}.
    // url points the check at an alternate manifest; transport "cell" binds
    // the transfer to the PPP interface (both mainly for testing).
    ota_check_opts_t opts = {0};
    cJSON *root = body[0] ? cJSON_Parse(body) : NULL;
    if (root) {
        const cJSON *url = cJSON_GetObjectItem(root, "url");
        const cJSON *transport = cJSON_GetObjectItem(root, "transport");
        if (cJSON_IsString(url) && url->valuestring[0]) {
            if (strncmp(url->valuestring, "https://", 8) != 0 ||
                strlen(url->valuestring) >= OTA_URL_MAX) {
                cJSON_Delete(root);
                return send_bad_request(
                    req, "url must be https:// and short"
                );
            }
            strlcpy(opts.url, url->valuestring, sizeof(opts.url));
        }
        if (cJSON_IsString(transport) && strcmp(transport->valuestring, "cell") == 0) {
            opts.force_cellular = true;
        }
        cJSON_Delete(root);
    }

    if (ota_check_now(&opts) != ESP_OK) {
        return send_bad_request(
            req, "a check or update is already running"
        );
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static const char *wifi_state_str(wifi_ui_state_t s)
{
    switch (s) {
    case WIFI_UI_STA_CONNECTING: return "connecting";
    case WIFI_UI_STA_CONNECTED:  return "connected";
    case WIFI_UI_AP:             return "softap";
    default:                     return "booting";
    }
}

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    wifi_ui_status_t st;
    wifi_get_status(&st);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return send_json(req, root);
    }
    cJSON_AddStringToObject(root, "state", wifi_state_str(st.state));
    cJSON_AddBoolToObject(root, "sta_configured", st.sta_configured);
    cJSON_AddBoolToObject(root, "connected", st.state == WIFI_UI_STA_CONNECTED);
    cJSON_AddStringToObject(root, "ssid", st.ssid);
    cJSON_AddStringToObject(root, "ip", st.ip);
    cJSON_AddNumberToObject(root, "rssi_dbm", st.rssi_dbm);
    cJSON_AddStringToObject(root, "ap_ssid", st.ap_ssid);
    cJSON_AddNumberToObject(root, "disconnects", st.disconnect_count);
    return send_json(req, root);
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    const cJSON *ssid = root ? cJSON_GetObjectItem(root, "ssid") : NULL;
    const cJSON *pass = root ? cJSON_GetObjectItem(root, "password") : NULL;
    // ssid required (empty string clears creds); password optional (open nets)
    if (!cJSON_IsString(ssid) || (pass && !cJSON_IsString(pass))) {
        cJSON_Delete(root);
        return send_bad_request(
            req, "expected {\"ssid\":\"...\",\"password\":\"...\"}"
        );
    }

    esp_err_t err = wifi_set_credentials(ssid->valuestring,
                                         pass ? pass->valuestring : "");
    cJSON_Delete(root);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_bad_request(req, "ssid/password too long");
    }
    if (err != ESP_OK) {
        return send_internal_error(req, "NVS write failed", err);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// {"enabled":bool,"sim":bool,"tx_pin":int,"rx_pin":int} — all optional, missing
// keys keep their value. tx_pin/rx_pin let the UART be moved (or swapped, when
// the BMS is wired backwards) from the web UI.
static esp_err_t bms_post_handler(httpd_req_t *req)
{
    char body[128];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return send_bad_request(
            req,
            "expected {\"enabled\":true,\"sim\":false,"
            "\"tx_pin\":1,\"rx_pin\":2}"
        );
    }

    bms_status_t cur;
    bms_get_status(&cur);
    const cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    const cJSON *sim = cJSON_GetObjectItem(root, "sim");
    const cJSON *tx = cJSON_GetObjectItem(root, "tx_pin");
    const cJSON *rx = cJSON_GetObjectItem(root, "rx_pin");
    bool new_enabled = cJSON_IsBool(enabled) ? cJSON_IsTrue(enabled) : cur.enabled;
    bool new_sim = cJSON_IsBool(sim) ? cJSON_IsTrue(sim) : cur.sim;
    int new_tx = cJSON_IsNumber(tx) ? tx->valueint : cur.tx_pin;
    int new_rx = cJSON_IsNumber(rx) ? rx->valueint : cur.rx_pin;
    cJSON_Delete(root);

    // Valid GPIOs on the ESP32-S3 are 0-48; TX and RX must be distinct.
    if (new_tx < 0 || new_tx > 48 || new_rx < 0 || new_rx > 48 || new_tx == new_rx) {
        return send_bad_request(
            req, "tx_pin/rx_pin must be distinct GPIOs in 0-48"
        );
    }

    esp_err_t bms_err =
        bms_set_options(new_enabled, new_sim, new_tx, new_rx);
    if (bms_err != ESP_OK) {
        return send_internal_error(req, "NVS write failed", bms_err);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t mqtt_get_handler(httpd_req_t *req)
{
    mqtt_config_t cfg;
    mqtt_get_config(&cfg);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return send_json(req, root);
    }
    cJSON_AddBoolToObject(root, "enabled", cfg.enabled);
    cJSON_AddStringToObject(root, "uri", cfg.uri);
    cJSON_AddStringToObject(root, "username", cfg.username);
    cJSON_AddBoolToObject(root, "password_set", cfg.password[0] != '\0');
    cJSON_AddStringToObject(root, "base_topic", cfg.base_topic);
    return send_json(req, root);
}

// Partial update: only the keys present change; omitting "password" keeps
// the stored one (the GET never echoes it back).
static esp_err_t mqtt_post_handler(httpd_req_t *req)
{
    char body[384];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return send_bad_request(req, "invalid JSON");
    }

    mqtt_config_t cfg;
    mqtt_get_config(&cfg);

    const cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    const cJSON *uri = cJSON_GetObjectItem(root, "uri");
    const cJSON *user = cJSON_GetObjectItem(root, "username");
    const cJSON *pass = cJSON_GetObjectItem(root, "password");
    const cJSON *base = cJSON_GetObjectItem(root, "base_topic");

    if (cJSON_IsString(uri)) {
        if (strlen(uri->valuestring) >= MQTT_URI_MAX ||
            (uri->valuestring[0] && strncmp(uri->valuestring, "mqtt://", 7) != 0 &&
             strncmp(uri->valuestring, "mqtts://", 8) != 0)) {
            cJSON_Delete(root);
            return send_bad_request(
                req, "uri must be mqtt:// or mqtts://"
            );
        }
        strlcpy(cfg.uri, uri->valuestring, sizeof(cfg.uri));
    }
    if (cJSON_IsString(user) && strlen(user->valuestring) >= MQTT_USER_MAX) {
        cJSON_Delete(root);
        return send_bad_request(
            req, "username must be at most 63 characters"
        );
    }
    if (cJSON_IsString(user)) {
        strlcpy(cfg.username, user->valuestring, sizeof(cfg.username));
    }
    if (cJSON_IsString(pass) && strlen(pass->valuestring) >= MQTT_PASS_MAX) {
        cJSON_Delete(root);
        return send_bad_request(
            req, "password must be at most 63 characters"
        );
    }
    if (cJSON_IsString(pass)) {
        strlcpy(cfg.password, pass->valuestring, sizeof(cfg.password));
    }
    if (cJSON_IsString(base) && base->valuestring[0] &&
        strlen(base->valuestring) < MQTT_TOPIC_MAX) {
        strlcpy(cfg.base_topic, base->valuestring, sizeof(cfg.base_topic));
    }
    if (cJSON_IsBool(enabled)) {
        cfg.enabled = cJSON_IsTrue(enabled);
    }
    cJSON_Delete(root);

    esp_err_t mqtt_err = mqtt_set_config(&cfg);
    if (mqtt_err != ESP_OK) {
        return send_internal_error(req, "NVS write failed", mqtt_err);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

typedef esp_err_t (*webui_handler_t)(httpd_req_t *req);

static esp_err_t observed_handler(httpd_req_t *req)
{
    int64_t started_us = esp_timer_get_time();
    s_current_response_error = false;
    s_current_http_status = 200;
    s_current_response_error_code = ESP_OK;
    s_current_max_chunk_send_ms = 0;
    bool ota_transport_active = ota_busy();
    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    s_http_status.request_count++;
    s_http_status.last_request_uptime_ms = (uint64_t)started_us / 1000U;
    strlcpy(s_http_status.last_uri, req->uri,
            sizeof(s_http_status.last_uri));
    if (ota_transport_active) {
        s_http_status.request_during_ota_count++;
    }
    xSemaphoreGive(s_http_mutex);
    if (ota_transport_active) {
        // Field hypothesis under test: a routine OTA TLS transfer starts
        // during a quiet gap (e.g. phone screen lock), then the operator
        // resumes polling mid-transfer and both paths degrade. Record the
        // overlap so soak/field evidence can confirm or rule it out.
        char details[96];
        snprintf(details, sizeof(details),
                 "{\"uri\":\"%.31s\",\"heap\":%lu,\"largest\":%lu}",
                 req->uri,
                 (unsigned long)heap_caps_get_free_size(WEBUI_INTERNAL_CAPS),
                 (unsigned long)heap_caps_get_largest_free_block(
                     WEBUI_INTERNAL_CAPS));
        event_journal_emit(
            "http", "request_during_ota", EVENT_SEVERITY_INFO,
            "ota_transport_overlap", details, false, 60000
        );
    }

    webui_handler_t handler = (webui_handler_t)req->user_ctx;
    esp_err_t result = handler(req);
    uint32_t duration_ms =
        (uint32_t)((esp_timer_get_time() - started_us) / 1000);
    bool slow = duration_ms >= 2000;
    bool failed = result != ESP_OK || s_current_response_error;
    esp_err_t diagnostic_error =
        s_current_response_error ? s_current_response_error_code : result;
    uint32_t task_stack_free =
        (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    uint32_t free_heap =
        (uint32_t)heap_caps_get_free_size(WEBUI_INTERNAL_CAPS);
    uint32_t largest_free_block =
        (uint32_t)heap_caps_get_largest_free_block(WEBUI_INTERNAL_CAPS);
    uint32_t free_psram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t largest_free_psram_block =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    s_http_status.last_duration_ms = duration_ms;
    if (duration_ms <= 100) {
        s_http_status.duration_le_100ms++;
    } else if (duration_ms <= 500) {
        s_http_status.duration_le_500ms++;
    } else if (duration_ms <= 1000) {
        s_http_status.duration_le_1000ms++;
    } else if (duration_ms <= 2500) {
        s_http_status.duration_le_2500ms++;
    } else {
        s_http_status.duration_gt_2500ms++;
    }
    if (s_current_max_chunk_send_ms > s_http_status.max_chunk_send_ms) {
        s_http_status.max_chunk_send_ms = s_current_max_chunk_send_ms;
    }
    bool send_stalled =
        s_current_max_chunk_send_ms >= WEBUI_SEND_STALL_THRESHOLD_MS;
    if (send_stalled) {
        s_http_status.send_stall_count++;
    }
    update_resource_minimum(
        &s_http_status.min_task_stack_free, task_stack_free
    );
    update_resource_minimum(&s_http_status.min_free_heap, free_heap);
    update_resource_minimum(
        &s_http_status.min_largest_free_block, largest_free_block
    );
    // update_resource_minimum() treats zero as "never sampled", so on a
    // build without PSRAM these two simply stay zero.
    update_resource_minimum(&s_http_status.min_free_psram, free_psram);
    update_resource_minimum(
        &s_http_status.min_largest_free_psram_block, largest_free_psram_block
    );
    if (!failed) {
        s_http_status.last_success_uptime_ms =
            (uint64_t)esp_timer_get_time() / 1000U;
    } else {
        s_http_status.failure_count++;
        if (s_current_response_error) {
            s_http_status.response_error_count++;
        }
        if (diagnostic_error == ESP_ERR_HTTPD_RESP_SEND) {
            s_http_status.transport_error_count++;
            s_http_status.last_transport_error_uptime_ms =
                (uint64_t)esp_timer_get_time() / 1000U;
        }
        s_http_status.last_error = diagnostic_error;
        s_http_status.last_error_uptime_ms =
            (uint64_t)esp_timer_get_time() / 1000U;
    }
    if (slow) {
        s_http_status.slow_request_count++;
    }
    xSemaphoreGive(s_http_mutex);

    if (failed || slow) {
        // Larger than the widest expansion of the format below (189 bytes:
        // a 31-char URI plus eight decimals) so the detail is never
        // truncated into invalid JSON here, and still inside the journal's
        // own 192-byte EVENT_DETAILS_MAX — over that, event_journal_emit()
        // discards the whole detail as a redaction error, which would lose
        // exactly the resource evidence this event exists to carry.
        char details[256];
        snprintf(
            details, sizeof(details),
            "{\"uri\":\"%.31s\",\"ms\":%lu,\"error\":%d,"
            "\"status\":%d,\"heap\":%lu,\"largest\":%lu,"
            "\"stack\":%lu,\"min_heap\":%lu,\"psram\":%lu}",
            req->uri, (unsigned long)duration_ms, diagnostic_error,
            s_current_http_status, (unsigned long)free_heap,
            (unsigned long)largest_free_block,
            (unsigned long)task_stack_free,
            (unsigned long)heap_caps_get_minimum_free_size(
                WEBUI_INTERNAL_CAPS),
            (unsigned long)free_psram
        );
        event_journal_emit(
            "http", failed ? "request_failed" : "slow_request",
            failed ? EVENT_SEVERITY_ERROR : EVENT_SEVERITY_WARN,
            failed ? "response_or_handler_error" : "duration_threshold",
            details, failed, 30000
        );
    }
    if (send_stalled) {
        char stall_details[96];
        snprintf(
            stall_details, sizeof(stall_details),
            "{\"uri\":\"%.31s\",\"chunk_ms\":%lu,\"ms\":%lu}",
            req->uri, (unsigned long)s_current_max_chunk_send_ms,
            (unsigned long)duration_ms
        );
        event_journal_emit(
            "http", "send_stall", EVENT_SEVERITY_WARN,
            "slow_client_send", stall_details, false, 30000
        );
    }
    return result;
}

// Web-service watchdog. A heartbeat work item is queued onto the server
// task through its internal control socket (no extra lwIP socket), so it
// runs only when the task returns to its select() loop; a heartbeat still
// pending after WEBUI_WATCHDOG_STALL_MS means the task is wedged in a
// handler or send. Detection only: httpd_stop() signals the server task and
// then waits for it to exit, so it cannot recover a wedged task, and a
// reboot escalation is a field-safety decision this plan has not approved.
static httpd_handle_t s_server;

static void heartbeat_work(void *arg)
{
    int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    uint32_t latency_ms =
        (uint32_t)((now_us - s_http_status.heartbeat_queued_us) / 1000);
    if (latency_ms > s_http_status.heartbeat_max_latency_ms) {
        s_http_status.heartbeat_max_latency_ms = latency_ms;
    }
    s_http_status.heartbeat_count++;
    s_http_status.heartbeat_pending = false;
    s_http_status.last_heartbeat_uptime_ms = (uint64_t)now_us / 1000U;
    xSemaphoreGive(s_http_mutex);
}

static void watchdog_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WEBUI_WATCHDOG_PERIOD_MS));
        int64_t now_us = esp_timer_get_time();

        xSemaphoreTake(s_http_mutex, portMAX_DELAY);
        bool pending = s_http_status.heartbeat_pending;
        bool was_stalled = s_http_status.heartbeat_stalled;
        uint32_t waited_ms = pending
            ? (uint32_t)((now_us - s_http_status.heartbeat_queued_us) / 1000)
            : 0;
        bool stalled = pending && waited_ms >= WEBUI_WATCHDOG_STALL_MS;
        uint64_t last_request_ms = s_http_status.last_request_uptime_ms;
        uint32_t sessions_open = s_http_status.sessions_open;
        char last_uri[sizeof(s_http_status.last_uri)];
        strlcpy(last_uri, s_http_status.last_uri, sizeof(last_uri));
        if (stalled && !was_stalled) {
            s_http_status.heartbeat_stalled = true;
            s_http_status.heartbeat_stall_count++;
        }
        if (!pending) {
            s_http_status.heartbeat_stalled = false;
            s_http_status.heartbeat_pending = true;
            s_http_status.heartbeat_queued_us = now_us;
        }
        xSemaphoreGive(s_http_mutex);

        if (stalled && !was_stalled) {
            char details[160];
            snprintf(
                details, sizeof(details),
                "{\"waited_ms\":%lu,\"last_uri\":\"%.31s\","
                "\"last_request_age_ms\":%lu,\"sessions_open\":%lu}",
                (unsigned long)waited_ms, last_uri,
                (unsigned long)((uint64_t)now_us / 1000U - last_request_ms),
                (unsigned long)sessions_open
            );
            event_journal_emit(
                "http", "server_unresponsive", EVENT_SEVERITY_ERROR,
                "heartbeat_stalled", details, true, 0
            );
        } else if (!pending && was_stalled) {
            event_journal_emit(
                "http", "server_recovered", EVENT_SEVERITY_INFO,
                "heartbeat_completed", "{}", true, 0
            );
        }

        if (!pending &&
            httpd_queue_work(s_server, heartbeat_work, NULL) != ESP_OK) {
            xSemaphoreTake(s_http_mutex, portMAX_DELAY);
            s_http_status.heartbeat_pending = false;
            s_http_status.heartbeat_queue_failure_count++;
            xSemaphoreGive(s_http_mutex);
        }
    }
}

void webui_init(void)
{
    s_http_mutex = xSemaphoreCreateMutex();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    // The server also owns three internal sockets. Eight client sessions keep
    // its total at eleven of the sixteen lwIP slots, reserving five for MQTT,
    // OTA, and transient outbound work. Eight also covers the six parallel
    // connections one iOS Safari instance opens, so the browser's own
    // fan-out no longer forces LRU purge of the live poll socket.
    cfg.max_open_sockets = WEBUI_MAX_CLIENT_SESSIONS;
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = WEBUI_KEEPALIVE_IDLE_S;
    cfg.keep_alive_interval = WEBUI_KEEPALIVE_INTERVAL_S;
    cfg.keep_alive_count = WEBUI_KEEPALIVE_COUNT;
    cfg.max_uri_handlers = 18;  // default 8; observability adds /api/events
    cfg.stack_size = 8192;  // ping/DNS handler keeps sizeable buffers on the stack
    // A dead or RF-degraded client otherwise pins the single server task in
    // send()/recv() for the default 5 s per call while every other poll
    // request queues behind it and exceeds the browser's abort deadline.
    cfg.recv_wait_timeout = 2;
    cfg.send_wait_timeout = 2;
    cfg.open_fn = session_open_fn;
    cfg.close_fn = session_close_fn;

    ESP_ERROR_CHECK(esp_event_handler_register(
        ESP_HTTP_SERVER_EVENT, HTTP_SERVER_EVENT_ERROR,
        &http_server_event_handler, NULL
    ));

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    s_server = server;

    static const httpd_uri_t routes[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = root_get_handler },
        { .uri = "/api/status", .method = HTTP_GET,  .handler = status_get_handler },
        { .uri = "/api/events", .method = HTTP_GET,  .handler = events_get_handler },
        { .uri = "/api/wifi",   .method = HTTP_GET,  .handler = wifi_get_handler },
        { .uri = "/api/wifi",   .method = HTTP_POST, .handler = wifi_post_handler },
        { .uri = "/api/apn",    .method = HTTP_POST, .handler = apn_post_handler },
        { .uri = "/api/at",     .method = HTTP_POST, .handler = at_post_handler },
        { .uri = "/api/ping",   .method = HTTP_POST, .handler = ping_post_handler },
        { .uri = "/api/ota",       .method = HTTP_GET,  .handler = ota_get_handler },
        { .uri = "/api/ota/check", .method = HTTP_POST, .handler = ota_check_post_handler },
        { .uri = "/api/bms",    .method = HTTP_POST, .handler = bms_post_handler },
        { .uri = "/api/mqtt",   .method = HTTP_GET,  .handler = mqtt_get_handler },
        { .uri = "/api/mqtt",   .method = HTTP_POST, .handler = mqtt_post_handler },
        { .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post_handler },
        { .uri = "/api/modem/restart", .method = HTTP_POST,
          .handler = modem_restart_post_handler },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_uri_t observed = routes[i];
        observed.user_ctx = (void *)observed.handler;
        observed.handler = observed_handler;
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &observed));
    }
    event_journal_emit(
        "http", "server_started", EVENT_SEVERITY_INFO, "listener_ready",
        "{\"port\":80}", true, 0
    );
    if (xTaskCreate(watchdog_task, "web_wdog", 3072, NULL, 3, NULL) != pdPASS) {
        event_journal_emit(
            "http", "watchdog_start_failed", EVENT_SEVERITY_ERROR,
            "task_create_failed", "{}", true, 0
        );
    }
}
