#include "modem.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "modem";

// Waveshare ESP32-S3-SIM7670G-4G: SIM7670G on UART1
#define MODEM_UART      UART_NUM_1
#define MODEM_TX_PIN    18
#define MODEM_RX_PIN    17
#define MODEM_BAUD      115200
#define UART_BUF_SIZE   2048
#define AT_RESP_MAX     2048

// Default APN for the EIOTCLUB SIM; change via the web UI if it doesn't attach.
#define DEFAULT_APN     "wbdata"

#define NVS_NAMESPACE   "cellcfg"
#define NVS_KEY_APN     "apn"

static SemaphoreHandle_t s_uart_mutex;
static SemaphoreHandle_t s_status_mutex;
static SemaphoreHandle_t s_diag_mutex;
static modem_status_t s_status;
static char s_apn[MODEM_APN_MAX] = DEFAULT_APN;
static bool s_apn_dirty;

static void uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate = MODEM_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(MODEM_UART, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(MODEM_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(MODEM_UART, MODEM_TX_PIN, MODEM_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    uart_flush_input(MODEM_UART);
}

// Send one AT command and read until a final result code or timeout.
// Returns ESP_OK on "OK", ESP_FAIL on ERROR/+CME/+CMS ERROR, ESP_ERR_TIMEOUT otherwise.
static esp_err_t at_transact(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms)
{
    if (resp && resp_len) {
        resp[0] = '\0';
    }
    xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    uart_flush_input(MODEM_UART);
    uart_write_bytes(MODEM_UART, cmd, strlen(cmd));
    uart_write_bytes(MODEM_UART, "\r\n", 2);

    static char buf[AT_RESP_MAX];
    size_t used = 0;
    esp_err_t result = ESP_ERR_TIMEOUT;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (esp_timer_get_time() < deadline) {
        int n = uart_read_bytes(MODEM_UART, (uint8_t *)buf + used,
                                (AT_RESP_MAX - 1) - used, pdMS_TO_TICKS(50));
        if (n > 0) {
            used += n;
            buf[used] = '\0';
            if (strstr(buf, "\r\nOK\r\n")) {
                result = ESP_OK;
                break;
            }
            if (strstr(buf, "\r\nERROR\r\n") || strstr(buf, "+CME ERROR") ||
                strstr(buf, "+CMS ERROR")) {
                result = ESP_FAIL;
                break;
            }
            if (used >= AT_RESP_MAX - 1) {
                break;
            }
        }
    }

    if (resp && resp_len) {
        snprintf(resp, resp_len, "%s", buf);
    }
    xSemaphoreGive(s_uart_mutex);
    return result;
}

esp_err_t modem_send_at(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms)
{
    return at_transact(cmd, resp, resp_len, timeout_ms);
}

// Like at_transact, but for commands whose real result is an unsolicited
// line arriving after OK (e.g. +CPING). Collects into `resp` until the
// line containing `urc_done` is complete, ERROR, or timeout.
static esp_err_t at_transact_urc(const char *cmd, const char *urc_done,
                                 char *resp, size_t resp_len, uint32_t timeout_ms)
{
    xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    uart_flush_input(MODEM_UART);
    uart_write_bytes(MODEM_UART, cmd, strlen(cmd));
    uart_write_bytes(MODEM_UART, "\r\n", 2);

    size_t used = 0;
    esp_err_t result = ESP_ERR_TIMEOUT;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    resp[0] = '\0';

    while (esp_timer_get_time() < deadline && used < resp_len - 1) {
        int n = uart_read_bytes(MODEM_UART, (uint8_t *)resp + used,
                                (resp_len - 1) - used, pdMS_TO_TICKS(100));
        if (n <= 0) {
            continue;
        }
        used += n;
        resp[used] = '\0';
        char *tok = strstr(resp, urc_done);
        if (tok && strchr(tok, '\n')) {
            result = ESP_OK;
            break;
        }
        if (strstr(resp, "\r\nERROR\r\n") || strstr(resp, "+CME ERROR")) {
            result = ESP_FAIL;
            break;
        }
    }
    xSemaphoreGive(s_uart_mutex);
    return result;
}

static bool valid_hostname(const char *h)
{
    size_t len = strlen(h);
    if (len == 0 || len > 128) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = h[i];
        if (!isalnum((unsigned char)c) && c != '.' && c != '-') {
            return false;
        }
    }
    return true;
}

esp_err_t modem_ping_host(const char *host, modem_netdiag_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!valid_hostname(host)) {
        return ESP_ERR_INVALID_ARG;
    }

    char cmd[160];
    static char resp[1024];  // guarded by s_diag_mutex
    xSemaphoreTake(s_diag_mutex, portMAX_DELAY);

    // DNS: one "+CDNSGIP: <n>,"host","ip"" line per address (before OK);
    // "+CDNSGIP: 0,<err>" followed by ERROR on failure.
    snprintf(cmd, sizeof(cmd), "AT+CDNSGIP=\"%s\"", host);
    at_transact(cmd, resp, sizeof(resp), 20000);
    strlcat(out->raw, resp, sizeof(out->raw));

    const char *p = resp;
    while ((p = strstr(p, "+CDNSGIP:")) != NULL) {
        p += strlen("+CDNSGIP:");
        if (atoi(p) == 0) {
            const char *comma = strchr(p, ',');
            out->dns_err = comma ? atoi(comma + 1) : -1;
        } else if (out->num_ips < MODEM_PING_MAX_IPS) {
            // IP is the second quoted string on the line
            const char *q[4] = {0};
            const char *scan = p;
            for (int i = 0; i < 4; i++) {
                q[i] = strchr(scan, '"');
                if (!q[i] || (i > 0 && strchr(scan, '\n') && strchr(scan, '\n') < q[i])) {
                    q[i] = NULL;
                    break;
                }
                scan = q[i] + 1;
            }
            if (q[3]) {
                size_t len = q[3] - (q[2] + 1);
                if (len > 0 && len < sizeof(out->ips[0])) {
                    memcpy(out->ips[out->num_ips], q[2] + 1, len);
                    out->ips[out->num_ips][len] = '\0';
                    out->num_ips++;
                }
            }
        }
    }
    out->dns_ok = out->num_ips > 0;
    if (!out->dns_ok) {
        xSemaphoreGive(s_diag_mutex);
        return ESP_OK;
    }

    // Ping: 4 packets, 64 bytes, 1 s interval, 10 s per-packet timeout
    // (10000 is the modem's minimum). Summary URC: +CPING: 3,...
    snprintf(cmd, sizeof(cmd), "AT+CPING=\"%s\",1,4,64,1000,10000,64", host);
    esp_err_t err = at_transact_urc(cmd, "+CPING: 3", resp, sizeof(resp), 55000);
    strlcat(out->raw, resp, sizeof(out->raw));

    if (err == ESP_OK) {
        const char *s = strstr(resp, "+CPING: 3");
        if (s && sscanf(s + strlen("+CPING: 3"), ",%d,%d,%d,%d,%d,%d",
                        &out->sent, &out->received, &out->lost,
                        &out->min_ms, &out->max_ms, &out->avg_ms) == 6) {
            out->ping_ok = true;
        }
    }
    xSemaphoreGive(s_diag_mutex);
    return ESP_OK;
}

// Copy the rest of the line following `prefix` in `resp` into out (trimmed).
static bool extract_line_after(const char *resp, const char *prefix, char *out, size_t out_len)
{
    const char *p = strstr(resp, prefix);
    if (!p) {
        return false;
    }
    p += strlen(prefix);
    while (*p == ' ') {
        p++;
    }
    size_t i = 0;
    while (*p && *p != '\r' && *p != '\n' && i < out_len - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

// Extract the first bare line (no '+' prefix, not echo) — for AT+CGSN etc.
static bool extract_bare_line(const char *resp, char *out, size_t out_len)
{
    const char *p = resp;
    while (*p) {
        while (*p == '\r' || *p == '\n') {
            p++;
        }
        const char *end = p;
        while (*end && *end != '\r' && *end != '\n') {
            end++;
        }
        size_t len = end - p;
        if (len > 0 && *p != '+' && strncmp(p, "AT", 2) != 0 &&
            strncmp(p, "OK", 2) != 0 && strncmp(p, "ERROR", 5) != 0) {
            if (len >= out_len) {
                len = out_len - 1;
            }
            memcpy(out, p, len);
            out[len] = '\0';
            return true;
        }
        p = end;
    }
    return false;
}

static void nvs_load_apn(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_apn);
        if (nvs_get_str(h, NVS_KEY_APN, s_apn, &len) != ESP_OK) {
            strlcpy(s_apn, DEFAULT_APN, sizeof(s_apn));
        }
        nvs_close(h);
    }
}

void modem_get_apn(char *out, size_t out_len)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    strlcpy(out, s_apn, out_len);
    xSemaphoreGive(s_status_mutex);
}

esp_err_t modem_set_apn(const char *apn)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_APN, apn);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    strlcpy(s_apn, apn, sizeof(s_apn));
    s_apn_dirty = true;
    xSemaphoreGive(s_status_mutex);
    ESP_LOGI(TAG, "APN set to \"%s\"", apn);
    return ESP_OK;
}

void modem_get_status(modem_status_t *out)
{
    if (!s_status_mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    *out = s_status;
    strlcpy(out->apn, s_apn, sizeof(out->apn));
    xSemaphoreGive(s_status_mutex);
}

// Configure the PDP context with the current APN and activate it.
static void pdp_setup(const char *apn)
{
    char cmd[MODEM_APN_MAX + 32];
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", apn);
    at_transact(cmd, NULL, 0, 5000);
    at_transact("AT+CGATT=1", NULL, 0, 10000);
    at_transact("AT+CGACT=1,1", NULL, 0, 15000);
}

// One-time identity queries once the modem answers.
static void read_identity(modem_status_t *st)
{
    char resp[AT_RESP_MAX];

    if (at_transact("AT+CGMM", resp, sizeof(resp), 3000) == ESP_OK) {
        extract_bare_line(resp, st->model, sizeof(st->model));
    }
    if (at_transact("AT+CGMR", resp, sizeof(resp), 3000) == ESP_OK) {
        if (!extract_line_after(resp, "+CGMR:", st->fw_rev, sizeof(st->fw_rev))) {
            extract_bare_line(resp, st->fw_rev, sizeof(st->fw_rev));
        }
    }
    if (at_transact("AT+CGSN", resp, sizeof(resp), 3000) == ESP_OK) {
        extract_bare_line(resp, st->imei, sizeof(st->imei));
    }
    if (at_transact("AT+CICCID", resp, sizeof(resp), 3000) == ESP_OK) {
        extract_line_after(resp, "+ICCID:", st->iccid, sizeof(st->iccid));
    }
}

static void poll_once(modem_status_t *st)
{
    char resp[AT_RESP_MAX];
    char line[96];

    // SIM state
    st->sim_ready = false;
    if (at_transact("AT+CPIN?", resp, sizeof(resp), 3000) == ESP_OK &&
        strstr(resp, "READY")) {
        st->sim_ready = true;
    }

    // Signal: +CSQ: <rssi>,<ber>  (dBm = -113 + 2*rssi, 99 = unknown)
    st->rssi_dbm = 0;
    if (at_transact("AT+CSQ", resp, sizeof(resp), 3000) == ESP_OK &&
        extract_line_after(resp, "+CSQ:", line, sizeof(line))) {
        int rssi = atoi(line);
        if (rssi >= 0 && rssi <= 31) {
            st->rssi_dbm = -113 + 2 * rssi;
        }
    }

    // LTE registration: +CEREG: <n>,<stat>
    st->reg_status = 0;
    if (at_transact("AT+CEREG?", resp, sizeof(resp), 3000) == ESP_OK &&
        extract_line_after(resp, "+CEREG:", line, sizeof(line))) {
        char *comma = strchr(line, ',');
        if (comma) {
            st->reg_status = atoi(comma + 1);
        }
    }

    // Operator: +COPS: 0,0,"T-Mobile",7
    st->operator_name[0] = '\0';
    if (at_transact("AT+COPS?", resp, sizeof(resp), 5000) == ESP_OK &&
        extract_line_after(resp, "+COPS:", line, sizeof(line))) {
        char *q1 = strchr(line, '"');
        if (q1) {
            char *q2 = strchr(q1 + 1, '"');
            if (q2) {
                *q2 = '\0';
                strlcpy(st->operator_name, q1 + 1, sizeof(st->operator_name));
            }
        }
    }

    // System info: +CPSI: LTE,Online,310-260,0x...,...,EUTRAN-BAND2,...
    st->rat[0] = '\0';
    st->band[0] = '\0';
    if (at_transact("AT+CPSI?", resp, sizeof(resp), 5000) == ESP_OK &&
        extract_line_after(resp, "+CPSI:", line, sizeof(line))) {
        char *tok, *save = NULL;
        int idx = 0;
        for (tok = strtok_r(line, ",", &save); tok; tok = strtok_r(NULL, ",", &save), idx++) {
            if (idx == 0) {
                strlcpy(st->rat, tok, sizeof(st->rat));
            } else if (strncmp(tok, "EUTRAN", 6) == 0 || strncmp(tok, "NR", 2) == 0 ||
                       strstr(tok, "BAND")) {
                strlcpy(st->band, tok, sizeof(st->band));
            }
        }
    }

    // PDP address: +CGPADDR: 1,10.x.x.x
    st->pdp_active = false;
    st->ip_addr[0] = '\0';
    if (at_transact("AT+CGPADDR=1", resp, sizeof(resp), 5000) == ESP_OK &&
        extract_line_after(resp, "+CGPADDR:", line, sizeof(line))) {
        char *comma = strchr(line, ',');
        if (comma) {
            char *ip = comma + 1;
            // strip quotes if present
            if (*ip == '"') {
                ip++;
                char *q = strchr(ip, '"');
                if (q) {
                    *q = '\0';
                }
            }
            if (strlen(ip) > 0 && strcmp(ip, "0.0.0.0") != 0) {
                strlcpy(st->ip_addr, ip, sizeof(st->ip_addr));
                st->pdp_active = true;
            }
        }
    }
}

static void modem_task(void *arg)
{
    modem_status_t st = {0};
    bool identity_read = false;
    bool pdp_configured = false;

    while (1) {
        // Probe the modem until it answers AT
        if (!st.at_ok) {
            if (at_transact("AT", NULL, 0, 2000) == ESP_OK) {
                st.at_ok = true;
                at_transact("ATE0", NULL, 0, 2000);      // echo off simplifies parsing
                at_transact("AT+COPS=3,0", NULL, 0, 2000);  // long operator names
                ESP_LOGI(TAG, "modem is responding");
            } else {
                ESP_LOGW(TAG, "modem not responding to AT yet");
            }
        }

        if (st.at_ok) {
            if (!identity_read) {
                read_identity(&st);
                identity_read = (st.imei[0] != '\0');
            }

            poll_once(&st);

            // (Re)configure PDP when needed: first time registered, or APN changed
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            bool apn_dirty = s_apn_dirty;
            s_apn_dirty = false;
            char apn[MODEM_APN_MAX];
            strlcpy(apn, s_apn, sizeof(apn));
            xSemaphoreGive(s_status_mutex);

            bool registered = (st.reg_status == 1 || st.reg_status == 5);
            if (apn_dirty || (registered && !st.pdp_active && !pdp_configured)) {
                ESP_LOGI(TAG, "activating PDP context, APN=\"%s\"", apn);
                pdp_setup(apn);
                pdp_configured = true;
                poll_once(&st);  // refresh IP right away
            }
            if (apn_dirty) {
                pdp_configured = false;  // allow a retry cycle with the new APN
            }

            st.last_update_us = esp_timer_get_time();
            ESP_LOGI(TAG, "sim=%d reg=%d rssi=%ddBm op=\"%s\" rat=%s ip=%s",
                     st.sim_ready, st.reg_status, st.rssi_dbm,
                     st.operator_name, st.rat,
                     st.pdp_active ? st.ip_addr : "-");
        }

        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status = st;
        xSemaphoreGive(s_status_mutex);

        vTaskDelay(pdMS_TO_TICKS(st.at_ok ? 5000 : 2000));
    }
}

void modem_init(void)
{
    s_uart_mutex = xSemaphoreCreateMutex();
    s_status_mutex = xSemaphoreCreateMutex();
    s_diag_mutex = xSemaphoreCreateMutex();
    nvs_load_apn();
    uart_init();
    xTaskCreate(modem_task, "modem", 6144, NULL, 5, NULL);
}
