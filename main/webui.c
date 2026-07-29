#include "webui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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
#include "wifi.h"

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

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
    uint64_t last_request_uptime_ms;
    uint64_t last_success_uptime_ms;
    uint64_t last_error_uptime_ms;
    uint32_t last_duration_ms;
    int last_error;
    char last_uri[48];
} webui_observability_t;

static SemaphoreHandle_t s_http_mutex;
static webui_observability_t s_http_status;
// ESP-IDF's HTTP server invokes handlers on one server task. This per-request
// marker lets observed_handler distinguish a deliberately sent HTTP error
// from a successful application response; httpd_resp_send_err() itself
// returns ESP_OK when it successfully transmits a 4xx/5xx response.
static bool s_current_response_error;
static int s_current_http_status;
static esp_err_t s_current_response_error_code;

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

#define JSON_STREAM_CHUNK_BYTES 512

typedef struct {
    httpd_req_t *req;
    char chunk[JSON_STREAM_CHUNK_BYTES];
    size_t used;
    esp_err_t error;
    bool sent;
} json_stream_t;

static void record_serialization_failure(void)
{
    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    s_http_status.serialization_failure_count++;
    xSemaphoreGive(s_http_mutex);
}

static void json_stream_begin(json_stream_t *stream, httpd_req_t *req)
{
    memset(stream, 0, sizeof(*stream));
    stream->req = req;
    stream->error = ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static bool json_stream_flush(json_stream_t *stream)
{
    if (stream->error != ESP_OK) {
        return false;
    }
    if (stream->used == 0) {
        return true;
    }
    stream->error = httpd_resp_send_chunk(
        stream->req, stream->chunk, stream->used
    );
    stream->used = 0;
    if (stream->error == ESP_OK) {
        stream->sent = true;
    }
    return stream->error == ESP_OK;
}

static bool json_stream_write(
    json_stream_t *stream,
    const char *data,
    size_t length
)
{
    while (length > 0) {
        size_t available = sizeof(stream->chunk) - stream->used;
        if (available == 0 && !json_stream_flush(stream)) {
            return false;
        }
        available = sizeof(stream->chunk) - stream->used;
        size_t count = length < available ? length : available;
        memcpy(stream->chunk + stream->used, data, count);
        stream->used += count;
        data += count;
        length -= count;
    }
    return true;
}

static bool json_stream_literal(json_stream_t *stream, const char *literal)
{
    return json_stream_write(stream, literal, strlen(literal));
}

static bool json_stream_string(json_stream_t *stream, const char *value)
{
    if (!json_stream_literal(stream, "\"")) {
        return false;
    }
    const unsigned char *cursor =
        (const unsigned char *)(value ? value : "");
    while (*cursor) {
        const char *escape = NULL;
        switch (*cursor) {
        case '"':  escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\b': escape = "\\b";  break;
        case '\f': escape = "\\f";  break;
        case '\n': escape = "\\n";  break;
        case '\r': escape = "\\r";  break;
        case '\t': escape = "\\t";  break;
        default: break;
        }
        if (escape) {
            if (!json_stream_literal(stream, escape)) {
                return false;
            }
        } else if (*cursor < 0x20) {
            char encoded[7];
            snprintf(encoded, sizeof(encoded), "\\u%04x", *cursor);
            if (!json_stream_literal(stream, encoded)) {
                return false;
            }
        } else {
            char byte = (char)*cursor;
            if (!json_stream_write(stream, &byte, 1)) {
                return false;
            }
        }
        cursor++;
    }
    return json_stream_literal(stream, "\"");
}

static bool json_stream_value(json_stream_t *stream, const cJSON *item)
{
    if (!item || cJSON_IsNull(item) || cJSON_IsInvalid(item)) {
        return json_stream_literal(stream, "null");
    }
    if (cJSON_IsFalse(item)) {
        return json_stream_literal(stream, "false");
    }
    if (cJSON_IsTrue(item)) {
        return json_stream_literal(stream, "true");
    }
    if (cJSON_IsNumber(item)) {
        if (!isfinite(item->valuedouble)) {
            return json_stream_literal(stream, "null");
        }
        char number[32];
        int length = snprintf(
            number, sizeof(number), "%.17g", item->valuedouble
        );
        return length > 0 && (size_t)length < sizeof(number) &&
               json_stream_write(stream, number, (size_t)length);
    }
    if (cJSON_IsString(item)) {
        return json_stream_string(stream, item->valuestring);
    }
    if (cJSON_IsRaw(item)) {
        return json_stream_literal(
            stream, item->valuestring ? item->valuestring : "null"
        );
    }
    if (cJSON_IsArray(item)) {
        if (!json_stream_literal(stream, "[")) {
            return false;
        }
        const cJSON *child = item->child;
        bool first = true;
        while (child) {
            if ((!first && !json_stream_literal(stream, ",")) ||
                !json_stream_value(stream, child)) {
                return false;
            }
            first = false;
            child = child->next;
        }
        return json_stream_literal(stream, "]");
    }
    if (cJSON_IsObject(item)) {
        if (!json_stream_literal(stream, "{")) {
            return false;
        }
        const cJSON *child = item->child;
        bool first = true;
        while (child) {
            if ((!first && !json_stream_literal(stream, ",")) ||
                !json_stream_string(stream, child->string) ||
                !json_stream_literal(stream, ":") ||
                !json_stream_value(stream, child)) {
                return false;
            }
            first = false;
            child = child->next;
        }
        return json_stream_literal(stream, "}");
    }
    return json_stream_literal(stream, "null");
}

static bool json_stream_object_members(
    json_stream_t *stream,
    const cJSON *object,
    bool *first
)
{
    const cJSON *child = object ? object->child : NULL;
    while (child) {
        if ((!*first && !json_stream_literal(stream, ",")) ||
            !json_stream_string(stream, child->string) ||
            !json_stream_literal(stream, ":") ||
            !json_stream_value(stream, child)) {
            return false;
        }
        *first = false;
        child = child->next;
    }
    return true;
}

static esp_err_t json_stream_finish(
    json_stream_t *stream,
    bool encoded
)
{
    if (encoded) {
        encoded = json_stream_flush(stream);
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

typedef void (*status_json_builder_t)(cJSON *root);

static bool json_stream_status_fragment(
    json_stream_t *stream,
    status_json_builder_t builder,
    bool *first
)
{
    cJSON *fragment = cJSON_CreateObject();
    if (!fragment) {
        record_serialization_failure();
        stream->error = ESP_ERR_NO_MEM;
        return false;
    }
    builder(fragment);
    bool encoded = json_stream_object_members(stream, fragment, first);
    cJSON_Delete(fragment);
    return encoded;
}

// Each module still owns the schema and types of its status fields, but only
// one module fragment exists at a time. This preserves the aggregate response
// while bounding peak cJSON heap independently of the total document size.
static esp_err_t status_get_handler(httpd_req_t *req)
{
    static const status_json_builder_t builders[] = {
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
    bool first = true;
    bool encoded = json_stream_literal(&stream, "{");
    for (size_t i = 0;
         encoded && i < sizeof(builders) / sizeof(builders[0]);
         i++) {
        encoded = json_stream_status_fragment(
            &stream, builders[i], &first
        );
    }
    if (encoded) {
        encoded = json_stream_literal(&stream, "}");
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
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "largest_free_block",
                            (double)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root, "minimum_free_heap",
                            (double)esp_get_minimum_free_heap_size());
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
    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    s_http_status.request_count++;
    s_http_status.last_request_uptime_ms = (uint64_t)started_us / 1000U;
    strlcpy(s_http_status.last_uri, req->uri,
            sizeof(s_http_status.last_uri));
    xSemaphoreGive(s_http_mutex);

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
    uint32_t free_heap = (uint32_t)esp_get_free_heap_size();
    uint32_t largest_free_block =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    s_http_status.last_duration_ms = duration_ms;
    update_resource_minimum(
        &s_http_status.min_task_stack_free, task_stack_free
    );
    update_resource_minimum(&s_http_status.min_free_heap, free_heap);
    update_resource_minimum(
        &s_http_status.min_largest_free_block, largest_free_block
    );
    if (!failed) {
        s_http_status.last_success_uptime_ms =
            (uint64_t)esp_timer_get_time() / 1000U;
    } else {
        s_http_status.failure_count++;
        if (s_current_response_error) {
            s_http_status.response_error_count++;
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
        char details[192];
        snprintf(
            details, sizeof(details),
            "{\"uri\":\"%.31s\",\"ms\":%lu,\"error\":%d,"
            "\"status\":%d,\"heap\":%lu,\"largest\":%lu,"
            "\"stack\":%lu,\"min_heap\":%lu}",
            req->uri, (unsigned long)duration_ms, diagnostic_error,
            s_current_http_status, (unsigned long)free_heap,
            (unsigned long)largest_free_block,
            (unsigned long)task_stack_free,
            (unsigned long)esp_get_minimum_free_heap_size()
        );
        event_journal_emit(
            "http", failed ? "request_failed" : "slow_request",
            failed ? EVENT_SEVERITY_ERROR : EVENT_SEVERITY_WARN,
            failed ? "response_or_handler_error" : "duration_threshold",
            details, failed, 30000
        );
    }
    return result;
}

void webui_init(void)
{
    s_http_mutex = xSemaphoreCreateMutex();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    // The server also owns three internal sockets. Four client sessions keep
    // its total at seven of the ten lwIP slots, reserving three for MQTT, OTA,
    // and transient outbound work.
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = 18;  // default 8; observability adds /api/events
    cfg.stack_size = 8192;  // ping/DNS handler keeps sizeable buffers on the stack

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

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
}
