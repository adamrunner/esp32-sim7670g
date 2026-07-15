#include "webui.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "bms.h"
#include "datalog.h"
#include "modem.h"
#include "mqtt.h"
#include "ota.h"
#include "timesync.h"
#include "wifi.h"

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

// Aggregate every module's status into one JSON document. Each module owns the
// serialization of its own fields (and the domain knowledge behind them); this
// handler just stitches the pieces together and ships the result.
static esp_err_t status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    modem_status_json(root);      // modem fields + "gnss"
    bms_status_json(root);        // "bms"
    mqtt_status_json(root);       // "mqtt"
    datalog_status_json(root);    // "datalog"
    timesync_status_json(root);   // "time"

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return err;
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
    char *json = cJSON_PrintUnformatted(out);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(out);
    return err;
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

    char *json = cJSON_PrintUnformatted(out);
    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(out);
    return send_err;
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
    if (st.last_check_us) {
        cJSON_AddNumberToObject(root, "last_check_age_s",
                                (double)((esp_timer_get_time() - st.last_check_us) / 1000000));
    }
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return err;
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

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return err;
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
                                   "expected {\"enabled\":true,\"sim\":false}");
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

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return err;
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
    if (cJSON_IsString(user) && strlen(user->valuestring) < MQTT_USER_MAX) {
        strlcpy(cfg.username, user->valuestring, sizeof(cfg.username));
    }
    if (cJSON_IsString(pass) && strlen(pass->valuestring) < MQTT_PASS_MAX) {
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

void webui_init(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 16;  // default 8; we register 12 routes
    cfg.stack_size = 8192;  // ping/DNS handler keeps sizeable buffers on the stack

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    static const httpd_uri_t routes[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = root_get_handler },
        { .uri = "/api/status", .method = HTTP_GET,  .handler = status_get_handler },
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
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }
}
