#include "webui.h"

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
#include "network_pause_gate.h"
#include "ota.h"
#include "timesync.h"
#include "wifi.h"

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

typedef struct {
    uint32_t request_count;
    uint32_t failure_count;
    uint32_t slow_request_count;
    uint64_t last_request_uptime_ms;
    uint64_t last_success_uptime_ms;
    uint64_t last_error_uptime_ms;
    uint32_t last_duration_ms;
    int last_error;
    char last_uri[48];
} webui_observability_t;

static SemaphoreHandle_t s_http_mutex;
static webui_observability_t s_http_status;

static void webui_status_json(cJSON *root)
{
    webui_observability_t status;
    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    status = s_http_status;
    xSemaphoreGive(s_http_mutex);
    cJSON *http = cJSON_AddObjectToObject(root, "http");
    cJSON_AddNumberToObject(http, "request_count", status.request_count);
    cJSON_AddNumberToObject(http, "failure_count", status.failure_count);
    cJSON_AddNumberToObject(http, "slow_request_count",
                            status.slow_request_count);
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
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "reboot already pending");
    }

    s_reboot_pending = true;
    if (xTaskCreate(reboot_task, "web_reboot", 2048, NULL, 5, NULL) != pdPASS) {
        s_reboot_pending = false;
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "could not schedule reboot");
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
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "cannot restart modem while OTA is active");
    }
    if (modem_request_restart() != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "modem restart already active");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"state\":\"requested\"}");
}

// Serialize `root` as the JSON response, then free both the printed string and
// the tree. Takes ownership of `root` (freed even on error), so no handler has
// to repeat the print/send/free/delete dance or risk leaking a branch.
static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

// Aggregate every module's status into one JSON document. Each module owns the
// serialization of its own fields (and the domain knowledge behind them); this
// handler just stitches the pieces together and ships the result.
static esp_err_t status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    modem_status_json(root);      // modem fields + "gnss"
    board_battery_status_json(root); // "internal_battery"
    bms_status_json(root);        // "bms"
    mqtt_status_json(root);       // "mqtt"
    network_pause_gate_status_json(root); // "network_pause"
    datalog_status_json(root);    // "datalog"
    timesync_status_json(root);   // "time"
    wifi_status_json(root);       // "wifi"
    webui_status_json(root);      // "http"
    event_journal_status_json(root); // "event_journal"
    return send_json(req, root);
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
                return httpd_resp_send_err(
                    req, HTTPD_400_BAD_REQUEST, "limit must be 1..48"
                );
            }
            limit = (size_t)requested;
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "ordering",
                            "boot_id,event_sequence");
    event_journal_events_json(root, limit);
    return send_json(req, root);
}

// Read and null-terminate a small JSON request body.
static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    if (req->content_len >= buf_len) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, buf + received, req->content_len - received);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
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
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected {\"apn\":\"...\"}");
    }

    esp_err_t err = modem_set_apn(apn->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
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
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected {\"cmd\":\"AT...\"}");
    }

    static char resp[2048];
    esp_err_t at_err = modem_send_at(cmd->valuestring, resp, sizeof(resp), 10000);
    cJSON_Delete(root);

    cJSON *out = cJSON_CreateObject();
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
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected {\"host\":\"...\"}");
    }

    static modem_netdiag_t diag;  // ~1 KB; httpd serves requests one at a time
    esp_err_t err = modem_ping_host(host->valuestring, &diag);
    cJSON_Delete(root);
    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid hostname");
    }

    cJSON *out = cJSON_CreateObject();
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
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                           "url must be https:// and short");
            }
            strlcpy(opts.url, url->valuestring, sizeof(opts.url));
        }
        if (cJSON_IsString(transport) && strcmp(transport->valuestring, "cell") == 0) {
            opts.force_cellular = true;
        }
        cJSON_Delete(root);
    }

    if (ota_check_now(&opts) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "a check or update is already running");
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
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "expected {\"ssid\":\"...\",\"password\":\"...\"}");
    }

    esp_err_t err = wifi_set_credentials(ssid->valuestring,
                                         pass ? pass->valuestring : "");
    cJSON_Delete(root);
    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid/password too long");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
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
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "expected {\"enabled\":true,\"sim\":false,\"tx_pin\":1,\"rx_pin\":2}");
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
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "tx_pin/rx_pin must be distinct GPIOs in 0-48");
    }

    if (bms_set_options(new_enabled, new_sim, new_tx, new_rx) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t mqtt_get_handler(httpd_req_t *req)
{
    mqtt_config_t cfg;
    mqtt_get_config(&cfg);

    cJSON *root = cJSON_CreateObject();
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
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
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
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "uri must be mqtt:// or mqtts://");
        }
        strlcpy(cfg.uri, uri->valuestring, sizeof(cfg.uri));
    }
    if (cJSON_IsString(user) && strlen(user->valuestring) >= MQTT_USER_MAX) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "username must be at most 63 characters");
    }
    if (cJSON_IsString(user)) {
        strlcpy(cfg.username, user->valuestring, sizeof(cfg.username));
    }
    if (cJSON_IsString(pass) && strlen(pass->valuestring) >= MQTT_PASS_MAX) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "password must be at most 63 characters");
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

    if (mqtt_set_config(&cfg) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

typedef esp_err_t (*webui_handler_t)(httpd_req_t *req);

static esp_err_t observed_handler(httpd_req_t *req)
{
    int64_t started_us = esp_timer_get_time();
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
    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    s_http_status.last_duration_ms = duration_ms;
    if (result == ESP_OK) {
        s_http_status.last_success_uptime_ms =
            (uint64_t)esp_timer_get_time() / 1000U;
    } else {
        s_http_status.failure_count++;
        s_http_status.last_error = result;
        s_http_status.last_error_uptime_ms =
            (uint64_t)esp_timer_get_time() / 1000U;
    }
    if (slow) {
        s_http_status.slow_request_count++;
    }
    xSemaphoreGive(s_http_mutex);

    if (result != ESP_OK || slow) {
        char details[128];
        snprintf(
            details, sizeof(details),
            "{\"uri\":\"%.47s\",\"duration_ms\":%lu,\"error\":%d}",
            req->uri, (unsigned long)duration_ms, result
        );
        event_journal_emit(
            "http", result == ESP_OK ? "slow_request" : "request_failed",
            result == ESP_OK ? EVENT_SEVERITY_WARN : EVENT_SEVERITY_ERROR,
            result == ESP_OK ? "duration_threshold" : "handler_error",
            details, result != ESP_OK, 30000
        );
    }
    return result;
}

void webui_init(void)
{
    s_http_mutex = xSemaphoreCreateMutex();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
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
