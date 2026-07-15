#include "timesync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "modem.h"

static const char *TAG = "timesync";

#define TIMESYNC_POLL_MS      10000
#define TIMESYNC_NTP_SERVER   "pool.ntp.org"
// A GNSS fix older than this isn't a clock source: utc[] captures the moment
// of the fix, not "now".
#define TIMESYNC_GNSS_MAX_AGE_US (30LL * 1000000)

static SemaphoreHandle_t s_mutex;
static timesync_status_t s_status;
static bool s_sntp_started;

static void set_synced(timesync_source_t source)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.source = source;
    s_status.synced_at = time(NULL);
    s_status.synced_at_us = esp_timer_get_time();
    xSemaphoreGive(s_mutex);
}

bool timesync_valid(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool valid = s_status.source != TIMESYNC_NONE;
    xSemaphoreGive(s_mutex);
    return valid;
}

void timesync_get_status(timesync_status_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mutex);
}

// Append the "time" object to the shared /api/status response.
void timesync_status_json(cJSON *root)
{
    timesync_status_t ts;
    timesync_get_status(&ts);
    cJSON *tm = cJSON_AddObjectToObject(root, "time");
    cJSON_AddBoolToObject(tm, "valid", ts.source != TIMESYNC_NONE);
    cJSON_AddStringToObject(tm, "source", ts.source == TIMESYNC_SNTP ? "sntp"
                                        : ts.source == TIMESYNC_GNSS ? "gnss"
                                                                     : "none");
    if (ts.source != TIMESYNC_NONE) {
        cJSON_AddNumberToObject(tm, "epoch", (double)time(NULL));
    }
}

static void on_sntp_sync(struct timeval *tv)
{
    set_synced(TIMESYNC_SNTP);
    ESP_LOGI(TAG, "SNTP sync: %lld", (long long)tv->tv_sec);
}

static bool default_route_up(void)
{
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (!netif) {
        return false;
    }
    esp_netif_ip_info_t ip;
    return esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

// Seed the clock from the modem's GNSS fix while SNTP hasn't landed. TZ is
// never set in this firmware, so mktime() interprets the UTC fields as UTC.
static void try_gnss_seed(void)
{
    modem_gnss_t g;
    modem_get_gnss(&g);
    if (!g.has_fix || g.fix_time_us == 0 ||
        esp_timer_get_time() - g.fix_time_us > TIMESYNC_GNSS_MAX_AGE_US) {
        return;
    }

    struct tm tm = {0};
    if (sscanf(g.utc, "%d-%d-%d %d:%d:%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6 || tm.tm_year < 2020) {
        return;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;

    struct timeval tv = { .tv_sec = mktime(&tm) };
    settimeofday(&tv, NULL);
    set_synced(TIMESYNC_GNSS);
    ESP_LOGI(TAG, "clock seeded from GNSS: %s UTC", g.utc);
}

static void timesync_task(void *arg)
{
    while (true) {
        // SNTP client starts once a link exists and keeps itself refreshed;
        // starting it with no route would just burn its retry budget.
        if (!s_sntp_started && default_route_up()) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(TIMESYNC_NTP_SERVER);
            cfg.sync_cb = on_sntp_sync;
            if (esp_netif_sntp_init(&cfg) == ESP_OK) {
                s_sntp_started = true;
                ESP_LOGI(TAG, "SNTP started (%s)", TIMESYNC_NTP_SERVER);
            }
        }

        if (!timesync_valid()) {
            try_gnss_seed();
        }

        vTaskDelay(pdMS_TO_TICKS(TIMESYNC_POLL_MS));
    }
}

void timesync_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    xTaskCreate(timesync_task, "timesync", 3072, NULL, 3, NULL);
}
