#include "wifi.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "wifi";

// SoftAP fallback (also the out-of-box default when no home creds are stored)
#define AP_SSID     "ESP32-SIM7670G"
#define AP_PASSWORD "waveshare"
#define AP_CHANNEL  6
#define AP_MAX_CONN 4

#define NVS_NAMESPACE "wificfg"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

// STA join policy. Attempts are mostly event-driven (a bad password disconnects
// immediately); the timeout only bounds the "nothing happened" case.
#define CONNECT_TIMEOUT_MS 15000
#define BOOT_MAX_ATTEMPTS  5    // at boot, before falling back to SoftAP
#define STEADY_MAX_ATTEMPTS 10  // after a live drop, before falling back
#define BACKOFF_MIN_MS     1000
#define BACKOFF_MAX_MS     30000

// Event-group bits the supervisor task waits on.
#define BIT_GOT_IP       BIT0  // IP_EVENT_STA_GOT_IP
#define BIT_DISCONNECTED BIT1  // WIFI_EVENT_STA_DISCONNECTED
#define BIT_RECONFIG     BIT2  // credentials changed; re-evaluate now

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_mutex;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static bool s_wifi_running;  // esp_wifi_start() issued and not yet stopped

// Credentials cache (guarded by s_mutex).
static char s_ssid[WIFI_SSID_MAX];
static char s_pass[WIFI_PASS_MAX];

// Status snapshot (guarded by s_mutex).
static wifi_ui_status_t s_status;

static void set_state(wifi_ui_state_t state)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.state = state;
    xSemaphoreGive(s_mutex);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Kick off the first association; reconnect attempts are driven by the
        // supervisor task so backoff stays in one place.
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "STA disconnected (reason %d)", d->reason);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.disconnect_count++;
        s_status.ip[0] = '\0';
        s_status.rssi_dbm = 0;
        xSemaphoreGive(s_mutex);
        // Clear GOT_IP so a stale bit can't fool the next connect wait.
        xEventGroupClearBits(s_events, BIT_GOT_IP);
        xEventGroupSetBits(s_events, BIT_DISCONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&e->ip_info.ip));
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "STA got IP %s", s_status.ip);
        xEventGroupClearBits(s_events, BIT_DISCONNECTED);
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

static void nvs_load_creds(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_ssid);
        if (nvs_get_str(h, NVS_KEY_SSID, s_ssid, &n) != ESP_OK) {
            s_ssid[0] = '\0';
        }
        n = sizeof(s_pass);
        if (nvs_get_str(h, NVS_KEY_PASS, s_pass, &n) != ESP_OK) {
            s_pass[0] = '\0';
        }
        nvs_close(h);
    }
    s_status.sta_configured = s_ssid[0] != '\0';
    strlcpy(s_status.ssid, s_ssid, sizeof(s_status.ssid));
    xSemaphoreGive(s_mutex);
}

// Stop WiFi only if it is actually running (esp_wifi_stop()'s behavior when
// never started isn't contractually defined, so we don't rely on it).
static void wifi_stop_if_running(void)
{
    if (s_wifi_running) {
        ESP_ERROR_CHECK(esp_wifi_stop());
        s_wifi_running = false;
    }
}

// Bring up the SoftAP (mode switch handles STA -> AP transitions too).
static void start_ap(void)
{
    wifi_stop_if_running();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .password = AP_PASSWORD,
            .channel = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_running = true;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.state = WIFI_UI_AP;
    strlcpy(s_status.ip, "192.168.4.1", sizeof(s_status.ip));
    s_status.rssi_dbm = 0;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "SoftAP \"%s\" up — http://192.168.4.1/", AP_SSID);
}

// Configure and start STA with the cached credentials. Emits STA_START, whose
// handler issues the first esp_wifi_connect().
static void start_sta(void)
{
    wifi_stop_if_running();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t sta_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .capable = true, .required = false },
        },
    };
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy((char *)sta_cfg.sta.ssid, s_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, s_pass, sizeof(sta_cfg.sta.password));
    xSemaphoreGive(s_mutex);

    // An open home network won't advertise WPA2; don't gate on the threshold.
    if (s_pass[0] == '\0') {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_DISCONNECTED);
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_running = true;
}

// Drive the STA association up to `max_attempts` tries with exponential
// backoff. Returns true once an IP is acquired. Sets *reconfig if new
// credentials arrived mid-attempt (caller should restart the decision).
static bool sta_connect_loop(int max_attempts, bool *reconfig)
{
    *reconfig = false;
    uint32_t backoff = BACKOFF_MIN_MS;

    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        EventBits_t bits = xEventGroupWaitBits(
            s_events, BIT_GOT_IP | BIT_DISCONNECTED | BIT_RECONFIG,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));

        if (bits & BIT_RECONFIG) {
            *reconfig = true;
            return false;
        }
        if (bits & BIT_GOT_IP) {
            return true;
        }

        // Disconnected or timed out: back off and retry the association.
        ESP_LOGW(TAG, "STA join attempt %d/%d failed; retrying in %lu ms",
                 attempt, max_attempts, (unsigned long)backoff);
        xEventGroupClearBits(s_events, BIT_DISCONNECTED);

        EventBits_t wait = xEventGroupWaitBits(s_events, BIT_RECONFIG, pdFALSE,
                                               pdFALSE, pdMS_TO_TICKS(backoff));
        if (wait & BIT_RECONFIG) {
            *reconfig = true;
            return false;
        }
        esp_wifi_connect();
        backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
    }
    return false;
}

static void wifi_task(void *arg)
{
    for (;;) {
        nvs_load_creds();
        xEventGroupClearBits(s_events, BIT_RECONFIG);

        bool have_creds;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        have_creds = s_ssid[0] != '\0';
        xSemaphoreGive(s_mutex);

        if (!have_creds) {
            ESP_LOGI(TAG, "no home-WiFi credentials stored; starting SoftAP");
            start_ap();
            xEventGroupWaitBits(s_events, BIT_RECONFIG, pdTRUE, pdFALSE, portMAX_DELAY);
            continue;  // credentials just changed — re-evaluate
        }

        ESP_LOGI(TAG, "joining home WiFi \"%s\"…", s_status.ssid);
        set_state(WIFI_UI_STA_CONNECTING);
        start_sta();

        bool reconfig = false;
        if (!sta_connect_loop(BOOT_MAX_ATTEMPTS, &reconfig)) {
            if (reconfig) {
                continue;
            }
            ESP_LOGW(TAG, "could not join \"%s\"; falling back to SoftAP", s_status.ssid);
            start_ap();
            xEventGroupWaitBits(s_events, BIT_RECONFIG, pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }

        // Connected. Cellular PPP stays dialed as backup; WiFi (route_prio 100)
        // is preferred over PPP (20) automatically while it holds an IP.
        set_state(WIFI_UI_STA_CONNECTED);
        ESP_LOGI(TAG, "joined \"%s\" — SoftAP off, WiFi is the preferred uplink",
                 s_status.ssid);

        // Steady state: sit until the link drops or credentials change.
        for (;;) {
            EventBits_t bits = xEventGroupWaitBits(
                s_events, BIT_DISCONNECTED | BIT_RECONFIG, pdTRUE, pdFALSE, portMAX_DELAY);
            if (bits & BIT_RECONFIG) {
                reconfig = true;
                break;
            }
            // Link dropped: try to get it back before surrendering to SoftAP.
            ESP_LOGW(TAG, "home WiFi dropped; attempting to reconnect");
            set_state(WIFI_UI_STA_CONNECTING);
            esp_wifi_connect();
            if (sta_connect_loop(STEADY_MAX_ATTEMPTS, &reconfig)) {
                set_state(WIFI_UI_STA_CONNECTED);
                continue;
            }
            break;  // reconfig or gave up
        }
        if (reconfig) {
            continue;
        }

        ESP_LOGW(TAG, "home WiFi unavailable; falling back to SoftAP");
        start_ap();
        xEventGroupWaitBits(s_events, BIT_RECONFIG, pdTRUE, pdFALSE, portMAX_DELAY);
    }
}

void wifi_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    strlcpy(s_status.ap_ssid, AP_SSID, sizeof(s_status.ap_ssid));

    // Both netifs exist up front; the supervisor just switches WiFi mode.
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    assert(s_sta_netif && s_ap_netif);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &event_handler, NULL));

    // The supervisor task sets the mode and starts WiFi (STA first, else AP).
    xTaskCreate(wifi_task, "wifi", 4096, NULL, 5, NULL);
}

void wifi_get_status(wifi_ui_status_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_status.state == WIFI_UI_STA_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_status.rssi_dbm = ap.rssi;
        }
    }
    *out = s_status;
    xSemaphoreGive(s_mutex);
}

esp_err_t wifi_set_credentials(const char *ssid, const char *pass)
{
    if (!ssid || !pass) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) >= WIFI_SSID_MAX || strlen(pass) >= WIFI_PASS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (ssid[0] == '\0') {
        // Empty SSID clears stored credentials (back to SoftAP).
        nvs_erase_key(h, NVS_KEY_SSID);
        nvs_erase_key(h, NVS_KEY_PASS);
    } else {
        err = nvs_set_str(h, NVS_KEY_SSID, ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(h, NVS_KEY_PASS, pass);
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }

    // Wake the supervisor to apply the change (join or return to SoftAP).
    xEventGroupSetBits(s_events, BIT_RECONFIG);
    ESP_LOGI(TAG, "credentials updated (ssid=\"%s\") — re-evaluating", ssid);
    return ESP_OK;
}
