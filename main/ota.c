#include "ota.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "lwip/sockets.h"   // struct ifreq for esp_http_client if_name
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "modem.h"
#include "mqtt.h"
#include "event_journal.h"
#include "wifi.h"

static const char *TAG = "ota";

// Overridable at build time (idf.py -DOTA_MANIFEST_URL=https://...) so test
// builds can track a staging manifest without touching the code.
#ifndef OTA_MANIFEST_URL
#define OTA_MANIFEST_URL \
    "https://adamrunner.com/downloads/esp32-sim7670g/manifest.json"
#endif

// Hourly manifest poll; retry sooner when a check couldn't reach the server
// (e.g. boot raced the cellular attach).
#define CHECK_INTERVAL_MS       (60 * 60 * 1000)
#define CHECK_RETRY_MS          (5 * 60 * 1000)
#define FIRST_CHECK_DELAY_MS    (90 * 1000)

// Rollback self-test: the new image must reach the update server over HTTPS
// within this window or it reboots into rollback. Sized for a cold cellular
// attach (registration + PPP can take a couple of minutes).
#define SELFTEST_WINDOW_MS      (6 * 60 * 1000)
#define SELFTEST_RETRY_MS       (15 * 1000)

// Download tuning. Timeouts are generous for cellular: PPP at 460800 moves
// ~25-34 KB/s, so a 1 MB image takes ~40 s in the best case and each 128 KB
// range request ~4-6 s.
#define HTTP_TIMEOUT_MS         60000
#define RANGE_REQUEST_SIZE      (128 * 1024)
#define MANIFEST_ATTEMPTS       4
#define DOWNLOAD_ATTEMPTS       4
#define RETRY_DELAY_MS          20000
#define RESUME_SAVE_INTERVAL    (128 * 1024)
#define TRANSPORT_READY_MS      (3 * 60 * 1000)
#define TRANSPORT_DROP_MS       (30 * 1000)

#define NVS_NAMESPACE           "otares"    // download-resume state
#define NVS_KEY_VERSION         "ver"
#define NVS_KEY_SHA             "sha"
#define NVS_KEY_LEN             "len"
#define ATTEMPT_NVS_NAMESPACE   "otaattempt"
#define ATTEMPT_NVS_SOURCE      "source"
#define ATTEMPT_NVS_TARGET      "target"

typedef struct {
    char version[OTA_VERSION_MAX];
    char url[OTA_URL_MAX];
    char sha256[65];
    int size;
} manifest_t;

typedef struct {
    ota_check_opts_t opts;
} check_req_t;

static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_trigger;     // depth 1: at most one queued manual check
static ota_status_t s_st;
static bool s_busy;                 // a check/update cycle is running

static void st_lock(void)   { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void st_unlock(void) { xSemaphoreGive(s_mutex); }

void ota_get_status(ota_status_t *out)
{
    if (!s_mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }
    st_lock();
    *out = s_st;
    st_unlock();
}

static void set_state(ota_state_t state)
{
    st_lock();
    ota_state_t previous = s_st.state;
    s_st.state = state;
    if (state != OTA_STATE_ERROR) {
        s_st.error[0] = '\0';
    }
    st_unlock();
    if (previous != state) {
        char details[64];
        snprintf(details, sizeof(details), "{\"from\":%d,\"to\":%d}",
                 (int)previous, (int)state);
        event_journal_emit(
            "ota", "state_changed",
            state == OTA_STATE_ERROR ? EVENT_SEVERITY_ERROR :
            state == OTA_STATE_WAIT_REBOOT ? EVENT_SEVERITY_WARN :
                                             EVENT_SEVERITY_INFO,
            "ota_transition", details,
            state == OTA_STATE_ERROR || state == OTA_STATE_WAIT_REBOOT, 0
        );
    }
}

static void __attribute__((format(printf, 1, 2))) set_error(const char *fmt, ...)
{
    char msg[OTA_ERRMSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    st_lock();
    s_st.state = OTA_STATE_ERROR;
    strlcpy(s_st.error, msg, sizeof(s_st.error));
    st_unlock();
    ESP_LOGE(TAG, "%s", msg);
    event_journal_emit(
        "ota", "failed", EVENT_SEVERITY_ERROR, "ota_error",
        "{\"details_redacted\":true}", true, 0
    );
}

static void set_progress(int bytes_read, int image_size)
{
    st_lock();
    s_st.bytes_read = bytes_read;
    s_st.image_size = image_size;
    s_st.progress_pct = image_size > 0 ? (int)((int64_t)bytes_read * 100 / image_size) : 0;
    st_unlock();
}

static void failure_snapshot(ota_failure_t *failure, const char *stage, esp_err_t err)
{
    if (!failure) {
        return;
    }
    memset(failure, 0, sizeof(*failure));
    strlcpy(failure->stage, stage, sizeof(failure->stage));
    failure->esp_err = err;
    failure->free_heap = esp_get_free_heap_size();
    failure->largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    failure->minimum_free_heap = esp_get_minimum_free_heap_size();
}

static void failure_capture_http(ota_failure_t *failure, const char *stage,
                                 esp_err_t err, esp_http_client_handle_t client)
{
    failure_snapshot(failure, stage, err);
    if (!failure || !client) {
        return;
    }

    int sock_errno = esp_http_client_get_errno(client);
    failure->sock_errno = sock_errno > 0 ? sock_errno : 0;

    int mbedtls_err = 0;
    int tls_flags = 0;
    esp_err_t tls_err =
        esp_http_client_get_and_clear_last_tls_error(client, &mbedtls_err, &tls_flags);
    failure->tls_err = tls_err == ESP_OK ? 0 : tls_err;
    failure->mbedtls_err = mbedtls_err;
    failure->tls_flags = tls_flags;
}

static void failure_restage(ota_failure_t *failure, const char *stage, esp_err_t err)
{
    if (!failure) {
        return;
    }
    strlcpy(failure->stage, stage, sizeof(failure->stage));
    failure->esp_err = err;
    failure->free_heap = esp_get_free_heap_size();
    failure->largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    failure->minimum_free_heap = esp_get_minimum_free_heap_size();
}

static void record_failure(const ota_failure_t *failure)
{
    if (!failure) {
        return;
    }
    st_lock();
    s_st.failure = *failure;
    st_unlock();
    ESP_LOGE(TAG,
             "%s failed: esp=%s tls=%s mbedtls=0x%x flags=0x%x errno=%d "
             "heap=%u largest=%u minimum=%u",
             failure->stage,
             esp_err_to_name(failure->esp_err),
             failure->tls_err ? esp_err_to_name(failure->tls_err) : "none",
             (unsigned)failure->mbedtls_err, (unsigned)failure->tls_flags,
             failure->sock_errno,
             (unsigned)failure->free_heap,
             (unsigned)failure->largest_free_block,
             (unsigned)failure->minimum_free_heap);
}

static void clear_failure(void)
{
    st_lock();
    memset(&s_st.failure, 0, sizeof(s_st.failure));
    st_unlock();
}

static void begin_cycle(void)
{
    st_lock();
    memset(&s_st.failure, 0, sizeof(s_st.failure));
    s_st.manifest_attempts = 0;
    s_st.download_attempts = 0;
    s_st.next_check_us = 0;
    s_st.last_check_ok = false;
    s_st.bytes_read = 0;
    s_st.image_size = 0;
    s_st.progress_pct = 0;
    st_unlock();
    set_state(OTA_STATE_CHECKING);
}

static void set_manifest_attempt(int attempt)
{
    st_lock();
    s_st.manifest_attempts = attempt;
    st_unlock();
}

static void set_download_attempt(int attempt)
{
    st_lock();
    s_st.download_attempts = attempt;
    st_unlock();
}

static esp_err_t ota_http_event(esp_http_client_event_t *event)
{
    ota_failure_t *failure = event->user_data;
    if (event->event_id == HTTP_EVENT_ERROR && failure && !failure->stage[0]) {
        failure_capture_http(failure, "download_http",
                             ESP_ERR_HTTP_CONNECT, event->client);
    }
    return ESP_OK;
}

esp_err_t ota_check_now(const ota_check_opts_t *opts)
{
    if (!s_trigger) {
        return ESP_ERR_INVALID_STATE;
    }
    st_lock();
    bool busy = s_busy;
    st_unlock();
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }
    check_req_t req = {0};
    if (opts) {
        req.opts = *opts;
    }
    return xQueueSend(s_trigger, &req, 0) == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
}

// ---------------------------------------------------------------------------
// Transport binding: normally the HTTP client follows the default route (WiFi
// when home, PPP otherwise). force_cellular pins a request to the PPP netif —
// used to exercise/validate OTA over 4G while WiFi is up. Note DNS still goes
// through the default route (lwIP's resolver is global); only the TCP/TLS
// data path is bound.
static bool cellular_ifreq(struct ifreq *ifr)
{
    esp_netif_t *ppp = modem_get_netif();
    if (!ppp || esp_netif_get_netif_impl_name(ppp, ifr->ifr_name) != ESP_OK) {
        return false;
    }
    return true;
}

static bool transport_ready(bool force_cellular)
{
    modem_status_t modem;
    modem_get_status(&modem);
    if (force_cellular) {
        return modem.ppp_up;
    }

    wifi_ui_status_t wifi;
    wifi_get_status(&wifi);
    return wifi.state == WIFI_UI_STA_CONNECTED || modem.ppp_up;
}

static bool wait_for_transport(bool force_cellular, uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        if (transport_ready(force_cellular)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

static void log_heap_state(const char *stage)
{
    ESP_LOGI(TAG, "%s: heap free=%u largest=%u minimum=%u", stage,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)esp_get_minimum_free_heap_size());
}

static bool recover_transport(bool force_cellular)
{
    wifi_ui_status_t wifi;
    wifi_get_status(&wifi);
    if (!force_cellular && wifi.state == WIFI_UI_STA_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        return true;
    }

    modem_request_redial();

    int64_t drop_deadline = esp_timer_get_time() + (int64_t)TRANSPORT_DROP_MS * 1000;
    while (esp_timer_get_time() < drop_deadline) {
        modem_status_t modem;
        modem_get_status(&modem);
        if (!modem.ppp_up) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return wait_for_transport(true, TRANSPORT_READY_MS);
}

// ---------------------------------------------------------------------------
// Manifest

// GET the manifest over HTTPS. Returns ESP_OK and fills m, or an error with a
// short description in errbuf. reach_only skips the body/parse and succeeds
// as soon as any HTTP response arrives (rollback self-test).
static esp_err_t fetch_manifest(const char *url, bool force_cellular, bool reach_only,
                                manifest_t *m, char *errbuf, size_t errlen,
                                ota_failure_t *failure, bool *retryable)
{
    static char body[1024];    // single OTA task; manifest is ~200 bytes

    if (failure) {
        memset(failure, 0, sizeof(*failure));
    }
    if (retryable) {
        *retryable = false;
    }

    struct ifreq ifr = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
    };
    if (force_cellular) {
        if (!cellular_ifreq(&ifr)) {
            snprintf(errbuf, errlen, "cellular interface unavailable");
            failure_snapshot(failure, "manifest_transport", ESP_ERR_INVALID_STATE);
            if (retryable) {
                *retryable = true;
            }
            return ESP_ERR_INVALID_STATE;
        }
        cfg.if_name = &ifr;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(errbuf, errlen, "http client init failed");
        failure_snapshot(failure, "manifest_init", ESP_ERR_NO_MEM);
        if (retryable) {
            *retryable = true;
        }
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        snprintf(errbuf, errlen, "connect failed: %s", esp_err_to_name(err));
        failure_capture_http(failure, "manifest_connect", err, client);
        if (retryable) {
            *retryable = true;
        }
        goto out;
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        snprintf(errbuf, errlen, "no HTTP response");
        err = ESP_ERR_HTTP_FETCH_HEADER;
        failure_capture_http(failure, "manifest_headers", err, client);
        if (retryable) {
            *retryable = true;
        }
        goto out;
    }
    int status = esp_http_client_get_status_code(client);
    if (reach_only) {
        // Any HTTP response proves DNS + TCP + TLS work end to end.
        err = ESP_OK;
        goto out;
    }
    if (status != 200) {
        snprintf(errbuf, errlen, "manifest HTTP %d", status);
        err = ESP_ERR_INVALID_RESPONSE;
        failure_snapshot(failure, "manifest_http", err);
        goto out;
    }

    int total = 0, n;
    while (total < (int)sizeof(body) - 1 &&
           (n = esp_http_client_read(client, body + total, sizeof(body) - 1 - total)) > 0) {
        total += n;
    }
    body[total] = '\0';
    if (total <= 0) {
        snprintf(errbuf, errlen, "empty manifest body");
        err = ESP_ERR_INVALID_RESPONSE;
        failure_capture_http(failure, "manifest_empty", err, client);
        if (retryable) {
            *retryable = true;
        }
        goto out;
    }

    cJSON *root = cJSON_Parse(body);
    const cJSON *ver = root ? cJSON_GetObjectItem(root, "version") : NULL;
    const cJSON *u = root ? cJSON_GetObjectItem(root, "url") : NULL;
    const cJSON *sha = root ? cJSON_GetObjectItem(root, "sha256") : NULL;
    const cJSON *size = root ? cJSON_GetObjectItem(root, "size") : NULL;
    if (!cJSON_IsString(ver) || !cJSON_IsString(u) || !cJSON_IsString(sha) ||
        !cJSON_IsNumber(size) ||
        strlen(ver->valuestring) >= OTA_VERSION_MAX ||
        strlen(u->valuestring) >= OTA_URL_MAX ||
        strlen(sha->valuestring) != 64 ||
        strncmp(u->valuestring, "https://", 8) != 0 ||
        size->valueint <= 0) {
        snprintf(errbuf, errlen, "manifest malformed");
        cJSON_Delete(root);
        err = ESP_ERR_INVALID_RESPONSE;
        failure_snapshot(failure, "manifest_parse", err);
        goto out;
    }
    strlcpy(m->version, ver->valuestring, sizeof(m->version));
    strlcpy(m->url, u->valuestring, sizeof(m->url));
    strlcpy(m->sha256, sha->valuestring, sizeof(m->sha256));
    m->size = size->valueint;
    cJSON_Delete(root);
    err = ESP_OK;

out:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

// ---------------------------------------------------------------------------
// Download-resume state: which image (version+sha) is partially written to
// the passive slot and how many bytes are already valid. Persisted so an
// interrupted download (link drop, reset) continues with a Range request
// instead of starting over; consulted only when it matches the current
// manifest exactly, and the final sha256 read-back guards the whole file
// either way.

static void resume_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void resume_save(const manifest_t *m, uint32_t len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_str(h, NVS_KEY_VERSION, m->version) == ESP_OK &&
        nvs_set_str(h, NVS_KEY_SHA, m->sha256) == ESP_OK &&
        nvs_set_u32(h, NVS_KEY_LEN, len) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static uint32_t resume_load(const manifest_t *m)
{
    nvs_handle_t h;
    uint32_t len = 0;
    char ver[OTA_VERSION_MAX] = "", sha[65] = "";
    size_t vlen = sizeof(ver), slen = sizeof(sha);
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    if (nvs_get_str(h, NVS_KEY_VERSION, ver, &vlen) != ESP_OK ||
        nvs_get_str(h, NVS_KEY_SHA, sha, &slen) != ESP_OK ||
        nvs_get_u32(h, NVS_KEY_LEN, &len) != ESP_OK) {
        len = 0;
    }
    nvs_close(h);
    if (len == 0 || strcmp(ver, m->version) != 0 || strcasecmp(sha, m->sha256) != 0 ||
        (int)len >= m->size) {
        return 0;
    }
    return len;
}

// ---------------------------------------------------------------------------
// OTA attempt evidence is separate from resumable-download state. It is
// written only after a complete image becomes bootable, survives the
// pending-verify boot, and is cleared after successful verification or after
// a rollback status event is acknowledged by the broker.

static esp_err_t attempt_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ATTEMPT_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t attempt_save(const char *source, const char *target)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ATTEMPT_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, ATTEMPT_NVS_SOURCE, source);
    if (err == ESP_OK) {
        err = nvs_set_str(h, ATTEMPT_NVS_TARGET, target);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static bool attempt_load(char source[OTA_VERSION_MAX],
                         char target[OTA_VERSION_MAX])
{
    nvs_handle_t h;
    size_t source_len = OTA_VERSION_MAX;
    size_t target_len = OTA_VERSION_MAX;
    if (nvs_open(ATTEMPT_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool found = nvs_get_str(h, ATTEMPT_NVS_SOURCE, source, &source_len) == ESP_OK &&
                 nvs_get_str(h, ATTEMPT_NVS_TARGET, target, &target_len) == ESP_OK;
    nvs_close(h);
    return found;
}

// ---------------------------------------------------------------------------
// sha256 read-back of the written slot. esp_https_ota validates the image
// structure but not the manifest hash; reading the flash back verifies what
// was actually written end to end.

static esp_err_t slot_sha256(const esp_partition_t *part, size_t len, uint8_t out[32])
{
    static uint8_t buf[4096];   // single OTA task
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }
    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < len; off += sizeof(buf)) {
        size_t chunk = len - off < sizeof(buf) ? len - off : sizeof(buf);
        err = esp_partition_read(part, off, buf, chunk);
        if (err != ESP_OK) {
            break;
        }
        if (mbedtls_sha256_update(&ctx, buf, chunk) != 0) {
            err = ESP_FAIL;
            break;
        }
    }
    if (err == ESP_OK && mbedtls_sha256_finish(&ctx, out) != 0) {
        err = ESP_FAIL;
    }
    mbedtls_sha256_free(&ctx);
    return err;
}

static bool sha256_matches(const uint8_t digest[32], const char *hex)
{
    char hexbuf[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hexbuf + 2 * i, 3, "%02x", digest[i]);
    }
    return strcasecmp(hexbuf, hex) == 0;
}

// ---------------------------------------------------------------------------
// Update install

// One esp_https_ota attempt. Returns ESP_OK when the image is written,
// verified and marked bootable (caller reboots).
static esp_err_t attempt_update(const manifest_t *m, bool force_cellular,
                                char *errbuf, size_t errlen, ota_failure_t *failure)
{
    if (failure) {
        memset(failure, 0, sizeof(*failure));
    }

    struct ifreq ifr = {0};
    esp_http_client_config_t http = {
        .url = m->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .keep_alive_enable = true,
        .event_handler = ota_http_event,
        .user_data = failure,
    };
    if (force_cellular) {
        if (!cellular_ifreq(&ifr)) {
            snprintf(errbuf, errlen, "cellular interface unavailable");
            failure_snapshot(failure, "download_transport", ESP_ERR_INVALID_STATE);
            return ESP_ERR_INVALID_STATE;
        }
        http.if_name = &ifr;
    }

    uint32_t resume_len = resume_load(m);
    esp_https_ota_config_t cfg = {
        .http_config = &http,
        .partial_http_download = true,      // ranged chunks; a drop loses at most one chunk
        .max_http_request_size = RANGE_REQUEST_SIZE,
        .ota_resumption = resume_len > 0,   // continue a previously interrupted download
        .ota_image_bytes_written = resume_len,
    };
    if (resume_len > 0) {
        ESP_LOGI(TAG, "resuming download at %u/%d bytes", (unsigned)resume_len, m->size);
    }

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &handle);
    if (err != ESP_OK) {
        snprintf(errbuf, errlen, "ota begin failed: %s", esp_err_to_name(err));
        if (failure && failure->stage[0]) {
            failure_restage(failure, "download_begin", err);
        } else {
            failure_snapshot(failure, "download_begin", err);
        }
        return err;
    }

    int image_size = esp_https_ota_get_image_size(handle);
    if (image_size != m->size) {
        snprintf(errbuf, errlen, "size mismatch: server %d, manifest %d", image_size, m->size);
        failure_snapshot(failure, "download_size", ESP_ERR_INVALID_RESPONSE);
        esp_https_ota_abort(handle);
        resume_clear();     // server content changed; partial data is stale
        return ESP_FAIL;
    }
    set_progress((int)resume_len, image_size);

    int last_saved = resume_len;
    int64_t t0 = esp_timer_get_time();
    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        int got = esp_https_ota_get_image_len_read(handle);
        set_progress(got, image_size);
        if (got - last_saved >= RESUME_SAVE_INTERVAL) {
            resume_save(m, got);    // a hard reset here resumes from `got`
            last_saved = got;
        }
    }

    int total = esp_https_ota_get_image_len_read(handle);
    set_progress(total, image_size);

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        snprintf(errbuf, errlen, "download failed at %d/%d: %s",
                 total, image_size, esp_err_to_name(err));
        if (failure && failure->stage[0]) {
            failure_restage(failure, "download_transfer",
                            err != ESP_OK ? err : ESP_FAIL);
        } else {
            failure_snapshot(failure, "download_transfer",
                             err != ESP_OK ? err : ESP_FAIL);
        }
        esp_https_ota_abort(handle);
        if (total > 0) {
            resume_save(m, total);  // next attempt continues from here
        }
        return err != ESP_OK ? err : ESP_FAIL;
    }

    int64_t dl_ms = (esp_timer_get_time() - t0) / 1000;
    int session = total - (int)resume_len;      // bytes fetched this attempt
    ESP_LOGI(TAG, "downloaded %d bytes (%d resumed) in %lld ms (%lld KB/s)",
             total, (int)resume_len, dl_ms,
             dl_ms > 0 ? (int64_t)session / dl_ms : 0);

    // Verify the manifest hash against the bytes actually in flash before
    // making anything bootable.
    set_state(OTA_STATE_VERIFYING);
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    uint8_t digest[32];
    if (!slot || slot_sha256(slot, m->size, digest) != ESP_OK) {
        snprintf(errbuf, errlen, "sha256 read-back failed");
        failure_snapshot(failure, "download_verify", ESP_FAIL);
        esp_https_ota_abort(handle);
        return ESP_FAIL;
    }
    if (!sha256_matches(digest, m->sha256)) {
        snprintf(errbuf, errlen, "sha256 mismatch — rejecting image");
        failure_snapshot(failure, "download_verify", ESP_ERR_INVALID_CRC);
        esp_https_ota_abort(handle);
        resume_clear();     // written data is not the manifest's image
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "sha256 verified against manifest (%s partition)", slot->label);

    err = esp_https_ota_finish(handle);     // validates structure, sets boot partition
    if (err != ESP_OK) {
        snprintf(errbuf, errlen, "ota finish failed: %s", esp_err_to_name(err));
        failure_snapshot(failure, "download_finish", err);
        return err;
    }
    resume_clear();
    return ESP_OK;
}

static void run_update(const manifest_t *m, bool force_cellular)
{
    ESP_LOGI(TAG, "updating %s -> %s (%d bytes) over %s", s_st.running_version,
             m->version, m->size, force_cellular ? "cellular (bound)" : "default route");
    log_heap_state("before download");
    set_state(OTA_STATE_DOWNLOADING);

    char errbuf[OTA_ERRMSG_MAX] = "";
    ota_failure_t failure = {0};
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= DOWNLOAD_ATTEMPTS; attempt++) {
        set_download_attempt(attempt);
        if (attempt > 1) {
            ESP_LOGW(TAG, "recovering transport before download attempt %d/%d: %s",
                     attempt, DOWNLOAD_ATTEMPTS, errbuf);
            if (!recover_transport(force_cellular)) {
                snprintf(errbuf, sizeof(errbuf),
                         "transport did not recover before attempt %d", attempt);
                err = ESP_ERR_TIMEOUT;
                failure_snapshot(&failure, "download_recovery", err);
                record_failure(&failure);
                break;
            }
            set_state(OTA_STATE_DOWNLOADING);
        }
        err = attempt_update(m, force_cellular, errbuf, sizeof(errbuf), &failure);
        if (err == ESP_OK) {
            clear_failure();
            break;
        }
        record_failure(&failure);
        log_heap_state("after failed download attempt");
    }

    if (err != ESP_OK) {
        set_error("%s", errbuf[0] ? errbuf : "update failed");
        return;
    }

    err = attempt_save(s_st.running_version, m->version);
    if (err != ESP_OK) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (running) {
            esp_ota_set_boot_partition(running);
        }
        failure_snapshot(&failure, "attempt_persist", err);
        record_failure(&failure);
        set_error("could not persist OTA attempt: %s", esp_err_to_name(err));
        return;
    }

    log_heap_state("after download");
    ESP_LOGI(TAG, "update installed; rebooting into %s in 3 s", m->version);
    set_state(OTA_STATE_WAIT_REBOOT);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

// Fetch the manifest and, on a version mismatch, download + install + reboot.
static void run_check(const check_req_t *req)
{
    const char *url = req->opts.url[0] ? req->opts.url : OTA_MANIFEST_URL;
    bool cell = req->opts.force_cellular;

    set_state(OTA_STATE_CHECKING);
    ESP_LOGI(TAG, "checking %s%s", url, cell ? " (bound to cellular)" : "");

    manifest_t m;
    char errbuf[OTA_ERRMSG_MAX] = "";
    ota_failure_t failure = {0};
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= MANIFEST_ATTEMPTS; attempt++) {
        set_manifest_attempt(attempt);
        bool retryable = false;
        err = fetch_manifest(url, cell, false, &m, errbuf, sizeof(errbuf),
                             &failure, &retryable);
        if (err == ESP_OK) {
            clear_failure();
            break;
        }

        record_failure(&failure);
        if (!retryable || attempt == MANIFEST_ATTEMPTS) {
            break;
        }

        ESP_LOGW(TAG, "recovering transport before manifest attempt %d/%d: %s",
                 attempt + 1, MANIFEST_ATTEMPTS, errbuf);
        if (!recover_transport(cell)) {
            err = ESP_ERR_TIMEOUT;
            snprintf(errbuf, sizeof(errbuf),
                     "transport did not recover before manifest attempt %d",
                     attempt + 1);
            failure_snapshot(&failure, "manifest_recovery", err);
            record_failure(&failure);
            break;
        }
        set_state(OTA_STATE_CHECKING);
    }

    st_lock();
    s_st.last_check_us = esp_timer_get_time();
    s_st.last_check_ok = err == ESP_OK;
    st_unlock();

    if (err != ESP_OK) {
        set_error("check failed: %s", errbuf);
        return;
    }

    bool same = strcmp(m.version, s_st.running_version) == 0;
    st_lock();
    s_st.update_available = !same;
    strlcpy(s_st.available_version, same ? "" : m.version, sizeof(s_st.available_version));
    bool pending = s_st.pending_verify;
    st_unlock();

    if (same) {
        ESP_LOGI(TAG, "up to date (%s)", s_st.running_version);
        set_state(OTA_STATE_IDLE);
        return;
    }

    // Don't reinstall a version that just failed its self-test: after a
    // rollback the passive slot still holds the bad image with its otadata
    // entry marked ABORTED. Without this check the hourly poll would loop
    // download -> crash -> rollback until the manifest changes.
    const esp_partition_t *passive = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t passive_state;
    esp_app_desc_t passive_desc;
    if (passive &&
        esp_ota_get_state_partition(passive, &passive_state) == ESP_OK &&
        passive_state == ESP_OTA_IMG_ABORTED &&
        esp_ota_get_partition_description(passive, &passive_desc) == ESP_OK &&
        strcmp(passive_desc.version, m.version) == 0) {
        set_error("update %s was rolled back (failed self-test); not retrying", m.version);
        return;
    }

    if (pending) {
        // Never stack an update on an image that hasn't passed its own
        // self-test; the pending image must confirm or roll back first.
        ESP_LOGW(TAG, "update %s available but running image is pending-verify; skipping",
                 m.version);
        set_state(OTA_STATE_IDLE);
        return;
    }

    run_update(&m, cell);
}

// ---------------------------------------------------------------------------
// Rollback confirmation: a pending-verify image only becomes permanent once
// it proves the network stack works by reaching the update server over
// HTTPS. If that doesn't happen within SELFTEST_WINDOW_MS, reboot without
// marking valid — the bootloader then reverts to the previous image.

static void rollback_selftest(void)
{
    ESP_LOGW(TAG, "running image is pending verify; self-testing (window %d s)",
             SELFTEST_WINDOW_MS / 1000);
    int64_t deadline = esp_timer_get_time() + (int64_t)SELFTEST_WINDOW_MS * 1000;
    char errbuf[OTA_ERRMSG_MAX];

    while (esp_timer_get_time() < deadline) {
        bool ready = wait_for_transport(false, SELFTEST_RETRY_MS);
        modem_suspend_polls(true);
        esp_err_t fetch_err = ready
                                  ? fetch_manifest(OTA_MANIFEST_URL, false, true, NULL,
                                                   errbuf, sizeof(errbuf), NULL, NULL)
                                  : ESP_ERR_TIMEOUT;
        modem_suspend_polls(false);
        if (!ready) {
            snprintf(errbuf, sizeof(errbuf), "network not ready");
        }
        if (fetch_err == ESP_OK) {
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "self-test passed (HTTPS reachable); image marked valid");
                esp_err_t clear_err = attempt_clear();
                if (clear_err != ESP_OK) {
                    ESP_LOGW(TAG, "could not clear verified OTA attempt marker: %s",
                             esp_err_to_name(clear_err));
                }
                st_lock();
                s_st.pending_verify = false;
                st_unlock();
                mqtt_publish_status();
                return;
            }
            ESP_LOGW(TAG, "HTTPS self-test passed but image could not be marked valid (%s); "
                          "retrying", esp_err_to_name(err));
        }
        ESP_LOGW(TAG, "self-test: server not reachable yet (%s); retrying", errbuf);
        vTaskDelay(pdMS_TO_TICKS(SELFTEST_RETRY_MS));
    }

    ESP_LOGE(TAG, "self-test failed within %d s — rolling back to previous image",
             SELFTEST_WINDOW_MS / 1000);
    esp_ota_mark_app_invalid_rollback_and_reboot();
    // Not reached; if the rollback call itself failed, reboot anyway: the
    // bootloader sees pending-verify and reverts.
    esp_restart();
}

// ---------------------------------------------------------------------------

static void ota_task(void *arg)
{
    if (s_st.pending_verify) {
        rollback_selftest();
    }

    uint32_t wait_ms = FIRST_CHECK_DELAY_MS;
    while (1) {
        st_lock();
        s_st.next_check_us = esp_timer_get_time() + (int64_t)wait_ms * 1000;
        st_unlock();

        check_req_t req = {0};
        if (xQueueReceive(s_trigger, &req, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
            memset(&req, 0, sizeof(req));  // timer-driven check, defaults
        }

        st_lock();
        s_busy = true;
        st_unlock();

        begin_cycle();
        if (!wait_for_transport(req.opts.force_cellular, TRANSPORT_READY_MS)) {
            ota_failure_t failure;
            failure_snapshot(&failure, "transport_ready", ESP_ERR_TIMEOUT);
            record_failure(&failure);
            set_error("network not ready for OTA check");
        } else {
            // Cover the manifest request as well as the binary transfer.
            // Otherwise a periodic SIM7670G AT/GNSS window can pause PPP
            // between those two HTTPS connections and leave the HTTP client
            // in a failed state while MQTT later recovers independently.
            modem_suspend_polls(true);
            run_check(&req);
            modem_suspend_polls(false);
        }

        st_lock();
        s_busy = false;
        bool ok = s_st.last_check_ok;
        bool failed = s_st.state == OTA_STATE_ERROR;
        st_unlock();

        // Any failed cycle gets another automatic chance sooner than the
        // normal hourly cadence, including a manually requested check.
        wait_ms = (!ok || failed) ? CHECK_RETRY_MS : CHECK_INTERVAL_MS;
    }
}

void ota_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_trigger = xQueueCreate(1, sizeof(check_req_t));

    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    strlcpy(s_st.running_version, desc->version, sizeof(s_st.running_version));
    strlcpy(s_st.running_slot, running ? running->label : "?", sizeof(s_st.running_slot));

    esp_ota_img_states_t img_state;
    if (running && esp_ota_get_state_partition(running, &img_state) == ESP_OK &&
        img_state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_st.pending_verify = true;
    }

    char attempt_source[OTA_VERSION_MAX] = "";
    char attempt_target[OTA_VERSION_MAX] = "";
    if (attempt_load(attempt_source, attempt_target)) {
        if (s_st.pending_verify &&
            strcmp(s_st.running_version, attempt_target) == 0) {
            ESP_LOGI(TAG, "OTA attempt %s -> %s is pending verification",
                     attempt_source, attempt_target);
        } else if (!s_st.pending_verify &&
                   strcmp(s_st.running_version, attempt_source) == 0 &&
                   strcmp(attempt_source, attempt_target) != 0) {
            s_st.rollback_detected = true;
            strlcpy(s_st.rollback_from_version, attempt_target,
                    sizeof(s_st.rollback_from_version));
            strlcpy(s_st.rollback_target_version, attempt_source,
                    sizeof(s_st.rollback_target_version));
            ESP_LOGE(TAG, "detected OTA rollback %s -> %s",
                     attempt_target, attempt_source);
        } else {
            ESP_LOGW(TAG, "discarding stale OTA attempt marker %s -> %s "
                          "(running %s%s)",
                     attempt_source, attempt_target, s_st.running_version,
                     s_st.pending_verify ? ", pending verify" : "");
            attempt_clear();
        }
    }

    ESP_LOGI(TAG, "running %s from %s%s", s_st.running_version, s_st.running_slot,
             s_st.pending_verify ? " (pending verify)" : "");

    // TLS handshake + esp_https_ota want real stack: 12 KB is comfortable.
    xTaskCreate(ota_task, "ota", 12288, NULL, 4, NULL);
}

esp_err_t ota_acknowledge_rollback_evidence(void)
{
    esp_err_t err = attempt_clear();
    if (err != ESP_OK) {
        return err;
    }
    st_lock();
    s_st.rollback_detected = false;
    s_st.rollback_from_version[0] = '\0';
    s_st.rollback_target_version[0] = '\0';
    st_unlock();
    return ESP_OK;
}
