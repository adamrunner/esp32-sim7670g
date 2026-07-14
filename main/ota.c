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
#define HTTP_TIMEOUT_MS         30000
#define RANGE_REQUEST_SIZE      (128 * 1024)
#define DOWNLOAD_ATTEMPTS       4
#define RETRY_DELAY_MS          20000
#define RESUME_SAVE_INTERVAL    (128 * 1024)

#define NVS_NAMESPACE           "otares"    // download-resume state
#define NVS_KEY_VERSION         "ver"
#define NVS_KEY_SHA             "sha"
#define NVS_KEY_LEN             "len"

typedef struct {
    char version[OTA_VERSION_MAX];
    char url[OTA_URL_MAX];
    char sha256[65];
    int size;
} manifest_t;

typedef struct {
    bool manual;
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
    s_st.state = state;
    if (state != OTA_STATE_ERROR) {
        s_st.error[0] = '\0';
    }
    st_unlock();
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
}

static void set_progress(int bytes_read, int image_size)
{
    st_lock();
    s_st.bytes_read = bytes_read;
    s_st.image_size = image_size;
    s_st.progress_pct = image_size > 0 ? (int)((int64_t)bytes_read * 100 / image_size) : 0;
    st_unlock();
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
    check_req_t req = { .manual = true };
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

// ---------------------------------------------------------------------------
// Manifest

// GET the manifest over HTTPS. Returns ESP_OK and fills m, or an error with a
// short description in errbuf. reach_only skips the body/parse and succeeds
// as soon as any HTTP response arrives (rollback self-test).
static esp_err_t fetch_manifest(const char *url, bool force_cellular, bool reach_only,
                                manifest_t *m, char *errbuf, size_t errlen)
{
    static char body[1024];    // single OTA task; manifest is ~200 bytes

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
            return ESP_ERR_INVALID_STATE;
        }
        cfg.if_name = &ifr;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(errbuf, errlen, "http client init failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        snprintf(errbuf, errlen, "connect failed: %s", esp_err_to_name(err));
        goto out;
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        snprintf(errbuf, errlen, "no HTTP response");
        err = ESP_FAIL;
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
        err = ESP_FAIL;
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
        err = ESP_FAIL;
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
        err = ESP_FAIL;
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
                                char *errbuf, size_t errlen)
{
    struct ifreq ifr = {0};
    esp_http_client_config_t http = {
        .url = m->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .keep_alive_enable = true,
    };
    if (force_cellular) {
        if (!cellular_ifreq(&ifr)) {
            snprintf(errbuf, errlen, "cellular interface unavailable");
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
        return err;
    }

    int image_size = esp_https_ota_get_image_size(handle);
    if (image_size != m->size) {
        snprintf(errbuf, errlen, "size mismatch: server %d, manifest %d", image_size, m->size);
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
        esp_https_ota_abort(handle);
        return ESP_FAIL;
    }
    if (!sha256_matches(digest, m->sha256)) {
        snprintf(errbuf, errlen, "sha256 mismatch — rejecting image");
        esp_https_ota_abort(handle);
        resume_clear();     // written data is not the manifest's image
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "sha256 verified against manifest (%s partition)", slot->label);

    err = esp_https_ota_finish(handle);     // validates structure, sets boot partition
    if (err != ESP_OK) {
        snprintf(errbuf, errlen, "ota finish failed: %s", esp_err_to_name(err));
        return err;
    }
    resume_clear();
    return ESP_OK;
}

static void run_update(const manifest_t *m, bool force_cellular)
{
    ESP_LOGI(TAG, "updating %s -> %s (%d bytes) over %s", s_st.running_version,
             m->version, m->size, force_cellular ? "cellular (bound)" : "default route");
    ESP_LOGI(TAG, "free heap before download: %u", (unsigned)esp_get_free_heap_size());

    // Hold off the modem task's status/GNSS polls: each one pauses PPP for
    // ~2 s ("+++"/ATO), which would stall — or in the worst case drop — a
    // multi-minute download running over the cellular link.
    modem_suspend_polls(true);
    set_state(OTA_STATE_DOWNLOADING);

    char errbuf[OTA_ERRMSG_MAX] = "";
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= DOWNLOAD_ATTEMPTS; attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "retrying download (attempt %d/%d) in %d s: %s",
                     attempt, DOWNLOAD_ATTEMPTS, RETRY_DELAY_MS / 1000, errbuf);
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            set_state(OTA_STATE_DOWNLOADING);
        }
        err = attempt_update(m, force_cellular, errbuf, sizeof(errbuf));
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGI(TAG, "free heap after failed attempt: %u", (unsigned)esp_get_free_heap_size());
    }

    modem_suspend_polls(false);     // resumes on every exit path

    if (err != ESP_OK) {
        set_error("%s", errbuf[0] ? errbuf : "update failed");
        return;
    }

    ESP_LOGI(TAG, "free heap after download: %u", (unsigned)esp_get_free_heap_size());
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
    char errbuf[OTA_ERRMSG_MAX];
    esp_err_t err = fetch_manifest(url, cell, false, &m, errbuf, sizeof(errbuf));

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
        if (fetch_manifest(OTA_MANIFEST_URL, false, true, NULL,
                           errbuf, sizeof(errbuf)) == ESP_OK) {
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "self-test passed (HTTPS reachable); image marked valid (%s)",
                     esp_err_to_name(err));
            st_lock();
            s_st.pending_verify = false;
            st_unlock();
            return;
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
        check_req_t req = {0};
        if (xQueueReceive(s_trigger, &req, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
            req.manual = false;     // timer-driven check, defaults
        }

        st_lock();
        s_busy = true;
        st_unlock();

        run_check(&req);

        st_lock();
        s_busy = false;
        bool ok = s_st.last_check_ok;
        st_unlock();

        // Unreachable server on an automatic check: retry sooner than hourly.
        wait_ms = (!ok && !req.manual) ? CHECK_RETRY_MS : CHECK_INTERVAL_MS;
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

    ESP_LOGI(TAG, "running %s from %s%s", s_st.running_version, s_st.running_slot,
             s_st.pending_verify ? " (pending verify)" : "");

    // TLS handshake + esp_https_ota want real stack: 12 KB is comfortable.
    xTaskCreate(ota_task, "ota", 12288, NULL, 4, NULL);
}
