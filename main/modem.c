#include "modem.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_modem_api.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"
#include "nvs.h"

static const char *TAG = "modem";

// Waveshare ESP32-S3-SIM7670G-4G: SIM7670G on UART1
#define MODEM_UART      UART_NUM_1
#define MODEM_TX_PIN    18
#define MODEM_RX_PIN    17
#define MODEM_BAUD      115200
#define AT_RESP_MAX     2048    // keep in sync with CONFIG_ESP_MODEM_C_API_STR_MAX

// Blank = let the carrier assign the APN. The EIOTCLUB SIM roaming on
// Verizon requires a blank attach context (it hands out "globaldata");
// forcing e.g. "wbdata" into CGDCONT 1 makes the network reject the LTE
// attach outright after the next modem reboot. Override via the web UI
// only if a SIM genuinely needs a named APN.
#define DEFAULT_APN     ""

#define NVS_NAMESPACE   "cellcfg"
#define NVS_KEY_APN     "apn"

#define CONNECT_RETRY_MS 20000

// While PPP is up, every status/GNSS poll pauses the data stream for a
// couple of seconds ("+++" ... ATO), so pace the polls.
#define PPP_POLL_INTERVAL_MS 30000
#define PPP_POLL_GRACE_MS    10000  // let the post-connect connectivity check finish first

static esp_modem_dce_t *s_dce;
static esp_netif_t *s_ppp_netif;

static SemaphoreHandle_t s_at_mutex;      // serializes AT commands through esp_modem
static SemaphoreHandle_t s_status_mutex;
static SemaphoreHandle_t s_diag_mutex;
static modem_status_t s_status;
static char s_apn[MODEM_APN_MAX] = DEFAULT_APN;
static bool s_apn_dirty;

// Written by esp_event handlers, read by the modem task (s_status_mutex).
static bool s_ppp_up;
static char s_ppp_ip[40];

// The UART carries either AT commands or PPP data, never both, and CMUX is
// off the table (a failed negotiation wedges this modem until a reset — see
// README). esp_modem_pause_net() gives us a middle ground: pause lwIP, wait
// the 1 s guard time, escape to command mode with "+++", run AT commands,
// then resume the same PPP session with ATO. at_channel_acquire/_release
// wrap that dance; callers own the AT channel exclusively in between.
static bool s_net_paused;   // guarded by s_at_mutex

static bool ppp_is_up(void)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    bool up = s_ppp_up;
    xSemaphoreGive(s_status_mutex);
    return up;
}

// Blocks data traffic for ~1-2 s when PPP is up. Returns false if the modem
// refused to drop into command mode (channel not acquired).
static bool at_channel_acquire(void)
{
    xSemaphoreTake(s_at_mutex, portMAX_DELAY);
    if (!ppp_is_up()) {
        return true;    // UART is already carrying AT
    }
    if (esp_modem_pause_net(s_dce, true) != ESP_OK) {
        ESP_LOGW(TAG, "pausing PPP for an AT window failed");
        // Best effort un-pause; if the modem never left data mode the ATO
        // is just line noise PPP's framing discards.
        esp_modem_pause_net(s_dce, false);
        xSemaphoreGive(s_at_mutex);
        return false;
    }
    s_net_paused = true;
    return true;
}

static void at_channel_release(void)
{
    if (s_net_paused) {
        s_net_paused = false;
        if (esp_modem_pause_net(s_dce, false) != ESP_OK) {
            // ATO didn't bring the data flow back; mark the link down so the
            // modem task tears the session down and redials cleanly.
            ESP_LOGW(TAG, "resuming PPP after AT window failed; forcing redial");
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_ppp_up = false;
            s_ppp_ip[0] = '\0';
            xSemaphoreGive(s_status_mutex);
        }
    }
    xSemaphoreGive(s_at_mutex);
}

// Send one AT command and capture the raw response (echo of URCs included).
// Caller must hold the AT channel (at_channel_acquire).
// Returns ESP_OK on "OK", ESP_FAIL on ERROR/+CME/+CMS ERROR, ESP_ERR_TIMEOUT otherwise.
static esp_err_t at_cmd(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms)
{
    static char buf[AT_RESP_MAX];   // guarded by s_at_mutex
    char full[300];

    if (resp && resp_len) {
        resp[0] = '\0';
    }
    if (snprintf(full, sizeof(full), "%s\r", cmd) >= (int)sizeof(full)) {
        return ESP_ERR_INVALID_ARG;
    }

    buf[0] = '\0';
    // "+CME ERROR"/"+CMS ERROR" both contain "ERROR", so one fail phrase covers all
    esp_err_t result = esp_modem_at_raw(s_dce, full, buf, "OK", "ERROR", timeout_ms);
    if (resp && resp_len) {
        snprintf(resp, resp_len, "%s", buf);
    }
    return result;
}

esp_err_t modem_send_at(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms)
{
    if (!at_channel_acquire()) {
        if (resp && resp_len) {
            snprintf(resp, resp_len, "unavailable: could not pause the PPP data stream\r\n");
        }
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = at_cmd(cmd, resp, resp_len, timeout_ms);
    at_channel_release();
    return err;
}

// ---------------------------------------------------------------------------
// Network diagnostics: DNS + ICMP ping through the ESP32's own lwIP stack,
// i.e. this actually exercises the PPP link, not the modem's internal stack.

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

static void __attribute__((format(printf, 2, 3)))
diag_line(modem_netdiag_t *out, const char *fmt, ...)
{
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    strlcat(out->raw, line, sizeof(out->raw));
}

typedef struct {
    modem_netdiag_t *out;
    SemaphoreHandle_t done;
    uint32_t sum_ms;
} ping_ctx_t;

static void ping_on_success(esp_ping_handle_t h, void *args)
{
    ping_ctx_t *ctx = args;
    modem_netdiag_t *out = ctx->out;
    uint16_t seq = 0;
    uint32_t elapsed = 0, bytes = 0;
    ip_addr_t target = {0};
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    esp_ping_get_profile(h, ESP_PING_PROF_SIZE, &bytes, sizeof(bytes));
    esp_ping_get_profile(h, ESP_PING_PROF_IPADDR, &target, sizeof(target));

    if (out->received == 0 || (int)elapsed < out->min_ms) {
        out->min_ms = elapsed;
    }
    if ((int)elapsed > out->max_ms) {
        out->max_ms = elapsed;
    }
    ctx->sum_ms += elapsed;
    out->received++;
    diag_line(out, "%u bytes from %s: icmp_seq=%u time=%u ms\n",
              (unsigned)bytes, ipaddr_ntoa(&target), seq, (unsigned)elapsed);
}

static void ping_on_timeout(esp_ping_handle_t h, void *args)
{
    ping_ctx_t *ctx = args;
    uint16_t seq = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    diag_line(ctx->out, "request timeout for icmp_seq %u\n", seq);
}

static void ping_on_end(esp_ping_handle_t h, void *args)
{
    ping_ctx_t *ctx = args;
    modem_netdiag_t *out = ctx->out;
    uint32_t sent = 0, received = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(h, ESP_PING_PROF_REPLY, &received, sizeof(received));
    out->sent = sent;
    out->received = received;
    out->lost = sent - received;
    out->avg_ms = received ? (int)(ctx->sum_ms / received) : 0;
    out->ping_ok = true;
    xSemaphoreGive(ctx->done);
}

esp_err_t modem_ping_host(const char *host, modem_netdiag_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!valid_hostname(host)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_diag_mutex, portMAX_DELAY);

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    bool up = s_ppp_up;
    xSemaphoreGive(s_status_mutex);
    if (!up) {
        diag_line(out, "cellular data link is down; nothing to ping through\n");
        xSemaphoreGive(s_diag_mutex);
        return ESP_OK;
    }

    // DNS via the resolvers the carrier handed us during PPP negotiation
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || !res) {
        out->dns_err = rc;
        diag_line(out, "DNS lookup for %s failed (getaddrinfo error %d)\n", host, rc);
        if (res) {
            freeaddrinfo(res);
        }
        xSemaphoreGive(s_diag_mutex);
        return ESP_OK;
    }
    for (struct addrinfo *ai = res; ai && out->num_ips < MODEM_PING_MAX_IPS; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET) {
            continue;
        }
        struct sockaddr_in *sa = (struct sockaddr_in *)ai->ai_addr;
        char ipstr[16];
        if (!inet_ntop(AF_INET, &sa->sin_addr, ipstr, sizeof(ipstr))) {
            continue;
        }
        bool dup = false;
        for (int i = 0; i < out->num_ips; i++) {
            if (strcmp(out->ips[i], ipstr) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            strlcpy(out->ips[out->num_ips++], ipstr, sizeof(out->ips[0]));
        }
    }
    freeaddrinfo(res);
    out->dns_ok = out->num_ips > 0;
    if (!out->dns_ok) {
        diag_line(out, "DNS returned no IPv4 addresses for %s\n", host);
        xSemaphoreGive(s_diag_mutex);
        return ESP_OK;
    }

    ip_addr_t target;
    if (!ipaddr_aton(out->ips[0], &target)) {
        xSemaphoreGive(s_diag_mutex);
        return ESP_OK;
    }

    ping_ctx_t ctx = { .out = out, .done = xSemaphoreCreateBinary() };
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = 4;
    cfg.interval_ms = 1000;
    cfg.timeout_ms = 3000;
    cfg.data_size = 56;
    esp_ping_callbacks_t cbs = {
        .cb_args = &ctx,
        .on_ping_success = ping_on_success,
        .on_ping_timeout = ping_on_timeout,
        .on_ping_end = ping_on_end,
    };

    diag_line(out, "PING %s (%s): %u data bytes\n", host, out->ips[0], (unsigned)cfg.data_size);
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) == ESP_OK) {
        esp_ping_start(ping);
        if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(cfg.count * (cfg.interval_ms + cfg.timeout_ms) + 5000)) != pdTRUE) {
            esp_ping_stop(ping);
            diag_line(out, "ping did not complete\n");
        }
        esp_ping_delete_session(ping);
    } else {
        diag_line(out, "failed to start ping session\n");
    }
    vSemaphoreDelete(ctx.done);

    xSemaphoreGive(s_diag_mutex);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Status polling helpers (same AT parsing as before, now via esp_modem)

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

// One-time identity queries once the modem answers.
static void read_identity(modem_status_t *st)
{
    char resp[AT_RESP_MAX];

    if (at_cmd("AT+CGMM", resp, sizeof(resp), 3000) == ESP_OK) {
        extract_bare_line(resp, st->model, sizeof(st->model));
    }
    if (at_cmd("AT+CGMR", resp, sizeof(resp), 3000) == ESP_OK) {
        if (!extract_line_after(resp, "+CGMR:", st->fw_rev, sizeof(st->fw_rev))) {
            extract_bare_line(resp, st->fw_rev, sizeof(st->fw_rev));
        }
    }
    if (at_cmd("AT+CGSN", resp, sizeof(resp), 3000) == ESP_OK) {
        extract_bare_line(resp, st->imei, sizeof(st->imei));
    }
    if (at_cmd("AT+CICCID", resp, sizeof(resp), 3000) == ESP_OK) {
        extract_line_after(resp, "+ICCID:", st->iccid, sizeof(st->iccid));
    }
}

// Returns how many poll commands succeeded, so the caller can notice a dead
// AT channel (0 successes) and force a re-sync.
static int poll_once(modem_status_t *st)
{
    char resp[AT_RESP_MAX];
    char line[96];
    int ok = 0;

    // SIM state (keep the last known value if the query itself fails, e.g.
    // "+CME ERROR: SIM busy" right after a modem reboot)
    if (at_cmd("AT+CPIN?", resp, sizeof(resp), 3000) == ESP_OK) {
        ok++;
        st->sim_ready = strstr(resp, "READY") != NULL;
    }

    // Signal: +CSQ: <rssi>,<ber>  (dBm = -113 + 2*rssi, 99 = unknown)
    st->rssi_dbm = 0;
    if (at_cmd("AT+CSQ", resp, sizeof(resp), 3000) == ESP_OK) {
        ok++;
        if (extract_line_after(resp, "+CSQ:", line, sizeof(line))) {
            int rssi = atoi(line);
            if (rssi >= 0 && rssi <= 31) {
                st->rssi_dbm = -113 + 2 * rssi;
            }
        }
    }

    // LTE registration: +CEREG: <n>,<stat>
    st->reg_status = 0;
    if (at_cmd("AT+CEREG?", resp, sizeof(resp), 3000) == ESP_OK) {
        ok++;
        if (extract_line_after(resp, "+CEREG:", line, sizeof(line))) {
            char *comma = strchr(line, ',');
            if (comma) {
                st->reg_status = atoi(comma + 1);
            }
        }
    }

    // Operator: +COPS: 0,0,"T-Mobile",7
    st->operator_name[0] = '\0';
    if (at_cmd("AT+COPS?", resp, sizeof(resp), 5000) == ESP_OK) {
        ok++;
        if (extract_line_after(resp, "+COPS:", line, sizeof(line))) {
            char *q1 = strchr(line, '"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '"');
                if (q2) {
                    *q2 = '\0';
                    strlcpy(st->operator_name, q1 + 1, sizeof(st->operator_name));
                }
            }
        }
    }

    // System info: +CPSI: LTE,Online,310-260,0x...,...,EUTRAN-BAND2,...
    st->rat[0] = '\0';
    st->band[0] = '\0';
    if (at_cmd("AT+CPSI?", resp, sizeof(resp), 5000) == ESP_OK) {
        ok++;
        if (extract_line_after(resp, "+CPSI:", line, sizeof(line))) {
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
    }
    return ok;
}

// ---------------------------------------------------------------------------
// GNSS: the receiver runs inside the SIM7670G independently of the cellular
// stack. We poll AT+CGNSSINFO instead of enabling the NMEA stream
// (AT+CGNSSTST=1) — streamed NMEA would interleave with PPP frames on the
// shared UART.

static modem_gnss_t s_gnss;     // guarded by s_status_mutex

void modem_get_gnss(modem_gnss_t *out)
{
    if (!s_status_mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    *out = s_gnss;
    xSemaphoreGive(s_status_mutex);
}

// Power the GNSS engine on (idempotent). OK arrives immediately; the engine
// needs a few more seconds before CGNSSINFO starts answering, and minutes
// under open sky for a first fix. Caller must hold the AT channel.
static bool gnss_power_on(void)
{
    char resp[AT_RESP_MAX];
    if (at_cmd("AT+CGNSSPWR?", resp, sizeof(resp), 3000) != ESP_OK ||
        !strstr(resp, "+CGNSSPWR: 1")) {
        if (at_cmd("AT+CGNSSPWR=1", NULL, 0, 10000) != ESP_OK) {
            return false;
        }
    }
    at_cmd("AT+CGNSSTST=0", NULL, 0, 3000);    // keep NMEA off the shared UART
    return true;
}

// +CGNSSINFO lat/lon arrive NMEA-style ("4736.806687" = ddmm.mmmmmm) or as
// plain decimal degrees ("47.613445"), depending on firmware —
// 2374B01SIM767XM5A sends decimal degrees. NMEA is zero-padded to 4 (lat)
// or 5 (lon) integer digits while decimal degrees never exceed 3, so the
// integer-digit count distinguishes them.
static double coord_to_deg(const char *s)
{
    int digits = 0;
    while (s[digits] >= '0' && s[digits] <= '9') {
        digits++;
    }
    double v = atof(s);
    if (digits < 4) {
        return v;
    }
    int deg = (int)(v / 100.0);
    return deg + (v - deg * 100.0) / 60.0;
}

// Parse one +CGNSSINFO payload, e.g.
//   2,09,05,00,4736.806687,N,12233.864103,W,270524,043754.0,16.9,0.0,,1.7,1.3,1.1
// or (2374B01SIM767XM5A: Galileo SV count, decimal degrees, <NoSV> tail)
//   3,11,07,10,14,45.391761,N,122.797858,W,140726,161102.000,54.9,0.01,215.75,0.82,0.43,0.70,36
// The field count varies between firmwares, so anchor on the single-letter
// N/S hemisphere field and index the rest relative to it. Leaves the
// previous position in place when there is no fix.
static void gnss_parse_info(char *line, modem_gnss_t *g)
{
    char *fields[24];
    int n = 0;
    char *p = line;
    while (n < 24) {
        fields[n++] = p;
        char *c = strchr(p, ',');
        if (!c) {
            break;
        }
        *c = '\0';
        p = c + 1;
    }

    int ns = -1;
    for (int i = 1; i < n - 1; i++) {
        if ((fields[i][0] == 'N' || fields[i][0] == 'S') && fields[i][1] == '\0') {
            ns = i;
            break;
        }
    }
    g->has_fix = false;
    g->sats = 0;
    g->sats_used = 0;
    if (n > 1) {  // SV counts are reported even without a fix
        int upto = (ns >= 2) ? ns - 2 : n - 1;
        for (int i = 1; i <= upto && i < n; i++) {
            g->sats += atoi(fields[i]);
        }
    }
    // Some firmwares also shorten the line (9 fields observed on
    // 2374B01SIM767XM5A with no fix), so only require through <course>
    // and bounds-check the DOP tail.
    if (ns < 2 || ns + 7 >= n) {
        return;     // ",,,,,,,," — engine on, no fix yet
    }

    double lat = coord_to_deg(fields[ns - 1]);
    double lon = coord_to_deg(fields[ns + 1]);
    g->lat = (fields[ns][0] == 'S') ? -lat : lat;
    g->lon = (fields[ns + 2][0] == 'W') ? -lon : lon;
    g->alt_m = atof(fields[ns + 5]);
    g->speed_kmh = atof(fields[ns + 6]) * 1.852f;   // knots -> km/h
    g->course_deg = atof(fields[ns + 7]);
    g->hdop = (ns + 9 < n) ? atof(fields[ns + 9]) : 0;
    g->sats_used = (ns + 11 < n) ? atoi(fields[ns + 11]) : 0;  // <NoSV>, after VDOP

    const char *date = fields[ns + 3];  // ddmmyy
    const char *tim = fields[ns + 4];   // hhmmss.s
    if (strlen(date) == 6 && strlen(tim) >= 6) {
        snprintf(g->utc, sizeof(g->utc), "20%.2s-%.2s-%.2s %.2s:%.2s:%.2s",
                 date + 4, date + 2, date, tim, tim + 2, tim + 4);
    } else {
        g->utc[0] = '\0';
    }

    g->has_fix = true;
    g->fix_time_us = esp_timer_get_time();
}

// Poll the receiver once and publish the result. Caller must hold the AT channel.
static void gnss_poll(void)
{
    char resp[AT_RESP_MAX];
    char line[256];
    if (at_cmd("AT+CGNSSINFO", resp, sizeof(resp), 5000) != ESP_OK ||
        !extract_line_after(resp, "+CGNSSINFO:", line, sizeof(line))) {
        return;
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    modem_gnss_t g = s_gnss;
    xSemaphoreGive(s_status_mutex);

    gnss_parse_info(line, &g);
    g.poll_time_us = esp_timer_get_time();

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_gnss = g;
    xSemaphoreGive(s_status_mutex);
}

// ---------------------------------------------------------------------------

// The PS domain must be attached before a data call will succeed; dialing
// too early (right after boot/registration) fails and destabilizes esp_modem.
static bool ps_attached(void)
{
    char resp[AT_RESP_MAX];
    return at_cmd("AT+CGATT?", resp, sizeof(resp), 5000) == ESP_OK &&
           strstr(resp, "+CGATT: 1");
}

// ---------------------------------------------------------------------------
// PPP lifecycle

static void on_ip_event(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_ppp_up = true;
        snprintf(s_ppp_ip, sizeof(s_ppp_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        xSemaphoreGive(s_status_mutex);

        esp_netif_dns_info_t dns = {0};
        esp_netif_get_dns_info(ev->esp_netif, ESP_NETIF_DNS_MAIN, &dns);
        ESP_LOGI(TAG, "PPP up: ip " IPSTR " gw " IPSTR " dns " IPSTR,
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw),
                 IP2STR(&dns.ip.u_addr.ip4));
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_ppp_up = false;
        s_ppp_ip[0] = '\0';
        xSemaphoreGive(s_status_mutex);
        ESP_LOGW(TAG, "PPP lost IP");
    }
}

static void on_ppp_status(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    // Events below the phase offset are lwIP PPP errors (peer hangup, LCP
    // failure, ...); treat any of them as "link down" so the task redials.
    if (event_id > NETIF_PPP_ERRORNONE && event_id < NETIF_PP_PHASE_OFFSET) {
        ESP_LOGW(TAG, "PPP error event %d — link down", (int)event_id);
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_ppp_up = false;
        s_ppp_ip[0] = '\0';
        xSemaphoreGive(s_status_mutex);
    }
}

// After a failed mode transition esp_modem can be left in an undefined
// state where every command fails instantly; force it back to command mode.
static void restore_command_mode(void)
{
    esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);  // may no-op, that's fine
    esp_modem_sync(s_dce);
}

// Dial the PPP session: the whole UART switches to PPP data. Holds the AT
// mutex so a mode transition can't race an open AT window.
static bool data_connect(void)
{
    xSemaphoreTake(s_at_mutex, portMAX_DELAY);
    bool ok = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA) == ESP_OK;
    if (!ok) {
        ESP_LOGW(TAG, "entering data mode failed");
        restore_command_mode();
    }
    xSemaphoreGive(s_at_mutex);
    return ok;
}

// Hang up / leave data mode ("+++" escape) and return to command mode.
static void data_disconnect(void)
{
    xSemaphoreTake(s_at_mutex, portMAX_DELAY);
    if (esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND) != ESP_OK) {
        ESP_LOGW(TAG, "hangup failed; forcing recovery on next cycle");
    }
    esp_modem_sync(s_dce);
    xSemaphoreGive(s_at_mutex);
}

static void modem_task(void *arg)
{
    modem_status_t st = {0};
    bool identity_read = false;
    bool gnss_on = false;
    bool was_up = false;
    int64_t last_dial_us = 0;
    int64_t last_poll_us = 0;
    int64_t ppp_up_since_us = 0;
    int denied_polls = 0;
    int dead_polls = 0;
    int healthy_polls = 0;

    while (1) {
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        bool ppp_up = s_ppp_up;
        strlcpy(st.ip_addr, s_ppp_ip, sizeof(st.ip_addr));
        char apn[MODEM_APN_MAX];
        strlcpy(apn, s_apn, sizeof(apn));
        bool apn_dirty = s_apn_dirty;
        xSemaphoreGive(s_status_mutex);

        st.ppp_up = ppp_up;
        st.pdp_active = ppp_up;  // LED turns blue when the data link is up

        // PPP just dropped (carrier hangup, LCP failure): leave data mode so
        // the AT channel comes back, then the loop below polls and redials.
        if (was_up && !ppp_up) {
            ESP_LOGW(TAG, "PPP link lost; returning to command mode");
            data_disconnect();
            healthy_polls = 0;
        }

        // One-shot connectivity check after each connect: proves DNS + ICMP
        // actually flow through the link, not just that IPCP negotiated.
        if (!was_up && ppp_up) {
            ppp_up_since_us = esp_timer_get_time();
            static modem_netdiag_t diag;  // ~1 KB, keep off the task stack
            modem_ping_host("google.com", &diag);
            if (diag.ping_ok && diag.received > 0) {
                ESP_LOGI(TAG, "connectivity check: %s -> %s, %d/%d replies, avg %d ms",
                         "google.com", diag.ips[0], diag.received, diag.sent, diag.avg_ms);
            } else {
                ESP_LOGW(TAG, "connectivity check failed (dns_ok=%d received=%d)",
                         diag.dns_ok, diag.received);
            }
        }
        was_up = ppp_up;

        // Probe the modem until it answers AT
        if (!st.at_ok && !ppp_up && at_channel_acquire()) {
            if (esp_modem_sync(s_dce) == ESP_OK) {
                st.at_ok = true;
                at_cmd("ATE0", NULL, 0, 2000);         // echo off simplifies parsing
                at_cmd("AT+COPS=3,0", NULL, 0, 2000);  // long operator names
                ESP_LOGI(TAG, "modem is responding");
            } else {
                ESP_LOGW(TAG, "modem not responding to AT yet");
                // A previous boot may have left the modem in CMUX or PPP mode
                // (the modem doesn't reset when the ESP32 does). Detect the
                // leftover state and drop back to plain command mode.
                if (esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DETECT) == ESP_OK) {
                    ESP_LOGI(TAG, "detected leftover modem state; recovering to command mode");
                    esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
                }
            }
            at_channel_release();
        }

        // Poll status + GNSS. With PPP down this runs every cycle; with PPP
        // up it needs an AT window (pausing data), so it runs every
        // PPP_POLL_INTERVAL_MS — which also keeps signal/operator/GNSS
        // fresh while connected instead of freezing at dial time.
        int64_t now = esp_timer_get_time();
        bool poll_due = !ppp_up ||
                        (now - ppp_up_since_us > (int64_t)PPP_POLL_GRACE_MS * 1000 &&
                         now - last_poll_us > (int64_t)PPP_POLL_INTERVAL_MS * 1000);
        if (st.at_ok && poll_due && at_channel_acquire()) {
            last_poll_us = now;
            if (!identity_read) {
                read_identity(&st);
                identity_read = (st.imei[0] != '\0');
            }
            if (poll_once(&st) == 0) {
                // AT channel went dead (bad mode transition, modem reboot,
                // leftover PPP...) — force a fresh sync + recovery cycle
                healthy_polls = 0;
                if (++dead_polls >= 2) {
                    ESP_LOGW(TAG, "AT channel dead; forcing re-sync");
                    st.at_ok = false;
                    dead_polls = 0;
                    gnss_on = false;    // modem may have rebooted; re-enable
                }
            } else {
                dead_polls = 0;
                if (healthy_polls < 100) {  // just avoid overflow
                    healthy_polls++;
                }
            }

            if (st.at_ok && !gnss_on) {
                gnss_on = gnss_power_on();
                ESP_LOGI(TAG, "GNSS power-on %s", gnss_on ? "ok" : "failed; will retry");
                xSemaphoreTake(s_status_mutex, portMAX_DELAY);
                s_gnss.powered = gnss_on;
                xSemaphoreGive(s_status_mutex);
            }
            if (st.at_ok && gnss_on) {
                gnss_poll();
            }

            // Self-heal a rejected LTE attach: a named APN left in context 1
            // (by a previous dial) makes some networks deny registration
            // after a modem reboot. Blank the attach context and re-scan.
            if (st.reg_status == 3) {
                if (++denied_polls >= 3) {
                    denied_polls = 0;
                    ESP_LOGW(TAG, "registration denied; blanking attach APN and re-scanning");
                    at_cmd("AT+CGDCONT=1,\"IPV4V6\",\"\"", NULL, 0, 5000);
                    at_cmd("AT+COPS=2", NULL, 0, 15000);
                    at_cmd("AT+COPS=0", NULL, 0, 15000);
                }
            } else {
                denied_polls = 0;
            }

            at_channel_release();
        }

        if (apn_dirty && st.at_ok) {
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_apn_dirty = false;
            xSemaphoreGive(s_status_mutex);

            ESP_LOGI(TAG, "APN changed to \"%s\" — redialing", apn);
            if (ppp_up) {
                data_disconnect();
                ppp_up = false;
            }
            esp_modem_set_apn(s_dce, apn);   // takes effect on the next dial
            last_dial_us = 0;                // dial immediately below
        }

        // Dial once the modem has been stably responsive for 2+ poll cycles:
        // dialing right after modem boot (URC noise, PS attach pending)
        // fails and can destabilize the esp_modem state machine.
        bool registered = (st.reg_status == 1 || st.reg_status == 5);
        if (st.at_ok && registered && !ppp_up && healthy_polls >= 2 &&
            esp_timer_get_time() - last_dial_us > (int64_t)CONNECT_RETRY_MS * 1000) {
            last_dial_us = esp_timer_get_time();
            bool attached = false;
            if (at_channel_acquire()) {
                attached = ps_attached();
                at_channel_release();
            }
            if (attached) {
                ESP_LOGI(TAG, "dialing PPP, APN=\"%s\"", apn[0] ? apn : "(carrier default)");
                data_connect();
            } else {
                ESP_LOGI(TAG, "registered but PS not attached yet; delaying dial");
            }
        }

        if (st.at_ok) {
            st.last_update_us = esp_timer_get_time();
            ESP_LOGI(TAG, "sim=%d reg=%d rssi=%ddBm op=\"%s\" rat=%s ppp=%s",
                     st.sim_ready, st.reg_status, st.rssi_dbm,
                     st.operator_name, st.rat,
                     st.ppp_up ? st.ip_addr : "down");
        }

        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status = st;
        xSemaphoreGive(s_status_mutex);

        vTaskDelay(pdMS_TO_TICKS(st.at_ok ? 5000 : 2000));
    }
}

void modem_init(void)
{
    s_at_mutex = xSemaphoreCreateMutex();
    s_status_mutex = xSemaphoreCreateMutex();
    s_diag_mutex = xSemaphoreCreateMutex();
    nvs_load_apn();

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_PPP();
    s_ppp_netif = esp_netif_new(&netif_cfg);
    assert(s_ppp_netif);

    // Error events (peer hangup etc.) are off by default; the redial logic needs them
    esp_netif_ppp_config_t ppp_cfg;
    ESP_ERROR_CHECK(esp_netif_ppp_get_params(s_ppp_netif, &ppp_cfg));
    ppp_cfg.ppp_error_event_enabled = true;
    ESP_ERROR_CHECK(esp_netif_ppp_set_params(s_ppp_netif, &ppp_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, on_ppp_status, NULL));

    esp_modem_dte_config_t dte_cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_cfg.uart_config.port_num = MODEM_UART;
    dte_cfg.uart_config.tx_io_num = MODEM_TX_PIN;
    dte_cfg.uart_config.rx_io_num = MODEM_RX_PIN;
    dte_cfg.uart_config.baud_rate = MODEM_BAUD;
    dte_cfg.uart_config.rx_buffer_size = 4096;
    dte_cfg.uart_config.tx_buffer_size = 2048;
    dte_cfg.dte_buffer_size = 1024;
    dte_cfg.task_stack_size = 6144;

    // SIM7670G speaks the same AT/PPP dialect as the SIM7600 family
    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG(s_apn);
    s_dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_cfg, &dce_cfg, s_ppp_netif);
    assert(s_dce);

    xTaskCreate(modem_task, "modem", 6144, NULL, 5, NULL);
}
