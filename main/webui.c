#include "webui.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "modem.h"
#include "wifi.h"

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

static const char *reg_status_str(int stat)
{
    switch (stat) {
    case 0: return "not registered";
    case 1: return "registered (home)";
    case 2: return "searching";
    case 3: return "registration denied";
    case 4: return "unknown";
    case 5: return "registered (roaming)";
    default: return "?";
    }
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    modem_status_t st;
    modem_get_status(&st);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "modem_ok", st.at_ok);
    cJSON_AddBoolToObject(root, "sim_ready", st.sim_ready);
    cJSON_AddNumberToObject(root, "reg_status", st.reg_status);
    cJSON_AddStringToObject(root, "reg_text", reg_status_str(st.reg_status));
    cJSON_AddBoolToObject(root, "pdp_active", st.pdp_active);
    cJSON_AddBoolToObject(root, "ppp_up", st.ppp_up);
    cJSON_AddNumberToObject(root, "rssi_dbm", st.rssi_dbm);
    cJSON_AddStringToObject(root, "model", st.model);
    cJSON_AddStringToObject(root, "fw_rev", st.fw_rev);
    cJSON_AddStringToObject(root, "imei", st.imei);
    cJSON_AddStringToObject(root, "iccid", st.iccid);
    cJSON_AddStringToObject(root, "operator", st.operator_name);
    cJSON_AddStringToObject(root, "rat", st.rat);
    cJSON_AddStringToObject(root, "band", st.band);
    cJSON_AddNumberToObject(root, "uart_baud", st.uart_baud);
    cJSON_AddStringToObject(root, "ip", st.ip_addr);
    cJSON_AddStringToObject(root, "apn", st.apn);

    modem_gnss_t g;
    modem_get_gnss(&g);
    cJSON *gnss = cJSON_AddObjectToObject(root, "gnss");
    cJSON_AddBoolToObject(gnss, "powered", g.powered);
    cJSON_AddBoolToObject(gnss, "fix", g.has_fix);
    cJSON_AddNumberToObject(gnss, "sats", g.sats);
    cJSON_AddNumberToObject(gnss, "sats_used", g.sats_used);
    cJSON_AddNumberToObject(gnss, "hdop", g.hdop);
    if (g.fix_time_us) {    // last known position, even if the current poll lost the fix
        cJSON_AddNumberToObject(gnss, "lat", g.lat);
        cJSON_AddNumberToObject(gnss, "lon", g.lon);
        cJSON_AddNumberToObject(gnss, "alt_m", g.alt_m);
        cJSON_AddNumberToObject(gnss, "speed_kmh", g.speed_kmh);
        cJSON_AddNumberToObject(gnss, "course_deg", g.course_deg);
        cJSON_AddStringToObject(gnss, "utc", g.utc);
        int64_t fix_age_s = (esp_timer_get_time() - g.fix_time_us) / 1000000;
        cJSON_AddNumberToObject(gnss, "fix_age_s", (double)fix_age_s);
    }

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

void webui_init(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
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
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }
}
