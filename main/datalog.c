#include "datalog.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"

#include "mqtt.h"
#include "sdcard.h"
#include "timesync.h"

static const char *TAG = "datalog";

#define QUEUE_DEPTH        8
#define DL_LINE_MAX           640   // 23 cols + 16 cells + 8 temps fits well under this
#define SD_DIR             SDCARD_MOUNT_POINT "/bms"
#define SD_BUF_SIZE        8192
#define SD_FLUSH_MS        30000  // vehicle power can drop anytime; don't sit on data
#define SD_MIN_FREE_BYTES  (16ULL * 1024 * 1024)
#define SPOOL_DIR          SDCARD_MOUNT_POINT "/spool"
#define SPOOL_FILE         SPOOL_DIR "/bms.csv"
#define SPOOL_CURSOR_FILE  SPOOL_DIR "/bms.cursor"
#define SPOOL_MAX_BYTES    (50ULL * 1024 * 1024)
#define SPOOL_REPLAY_PER_TICK 2   // ticks are ~500 ms => ~4 msg/s ceiling
#define SPOOL_CURSOR_EVERY 16     // acked lines between cursor persists

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_mutex;
static datalog_status_t s_status;

// SD sink state (datalog task only)
static char s_sd_buf[SD_BUF_SIZE];
static size_t s_sd_buf_len;
static int64_t s_sd_buf_first_us;
static char s_sd_file[40];
static bool s_sd_full;

// Spool state (datalog task only)
static long s_spool_size = -1;    // -1 = not yet loaded from disk
static long s_spool_cursor;
static int s_spool_unsaved_acks;

void datalog_device_id(char *out, size_t out_len)
{
    static char cached[48];
    if (!cached[0]) {
        nvs_handle_t nvs;
        if (nvs_open("logcfg", NVS_READONLY, &nvs) == ESP_OK) {
            size_t len = sizeof(cached);
            nvs_get_str(nvs, "device_id", cached, &len);
            nvs_close(nvs);
        }
        if (!cached[0]) {
            uint8_t mac[6] = {0};
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            snprintf(cached, sizeof(cached), "gw-%02x%02x%02x", mac[3], mac[4], mac[5]);
        }
    }
    strlcpy(out, cached, out_len);
}

// ---------------------------------------------------------------------------
// CSV serialization — byte-compatible with esp32-bms-monitor / bms-dashboard.

static int serialize_csv(const bms_snapshot_t *s, char *out, size_t out_len)
{
    char device_id[48];
    datalog_device_id(device_id, sizeof(device_id));

    unsigned hours = s->elapsed_sec / 3600;
    unsigned minutes = (s->elapsed_sec % 3600) / 60;
    unsigned seconds = s->elapsed_sec % 60;

    int n = snprintf(out, out_len,
        "%s,%lld,%u,%02u:%02u:%02u,%.3f,%.2f,%.2f,%.1f,%.2f,%.2f,%.2f,%.2f,%d,%.3f,%d,%.3f,%d,%.3f,%d,%.1f,%.1f,%d,%d",
        device_id,
        (long long)s->real_timestamp,
        (unsigned)s->elapsed_sec, hours, minutes, seconds,
        s->total_energy_wh,
        s->pack_voltage_v, s->pack_current_a, s->soc_pct, s->power_w,
        s->full_capacity_ah, s->peak_current_a, s->peak_power_w, s->cell_count,
        s->min_cell_voltage_v, s->min_cell_num, s->max_cell_voltage_v,
        s->max_cell_num, s->cell_voltage_delta_v, s->temp_count,
        s->min_temp_c, s->max_temp_c, s->charging_enabled ? 1 : 0,
        s->discharging_enabled ? 1 : 0);

    int cells = s->cell_count < DATALOG_MAX_CELLS ? s->cell_count : DATALOG_MAX_CELLS;
    for (int i = 0; i < cells && n < (int)out_len; i++) {
        n += snprintf(out + n, out_len - n, ",%.3f", s->cell_v[i]);
    }
    int temps = s->temp_count < DATALOG_MAX_TEMPS ? s->temp_count : DATALOG_MAX_TEMPS;
    for (int i = 0; i < temps && n < (int)out_len; i++) {
        n += snprintf(out + n, out_len - n, ",%.1f", s->temp_c[i]);
    }
    return n;
}

// Header always advertises the full 16-cell/8-temp width (like the
// reference); data rows carry only cell_count/temp_count value columns.
static void write_csv_header(FILE *f)
{
    fputs("device_id,timestamp,elapsed_sec,hours:minutes:seconds,total_energy_wh,"
          "pack_voltage_v,pack_current_a,soc_pct,power_w,full_capacity_ah,"
          "peak_current_a,peak_power_w,cell_count,min_cell_voltage_v,min_cell_num,"
          "max_cell_voltage_v,max_cell_num,cell_voltage_delta_v,temp_count,"
          "min_temp_c,max_temp_c,charging_enabled,discharging_enabled", f);
    for (int i = 1; i <= DATALOG_MAX_CELLS; i++) {
        fprintf(f, ",cell_v_%d", i);
    }
    for (int i = 1; i <= DATALOG_MAX_TEMPS; i++) {
        fprintf(f, ",temp_c_%d", i);
    }
    fputc('\n', f);
}

// ---------------------------------------------------------------------------
// SD sink: RAM-buffered append, one fopen/fclose per flush so a power cut
// can only lose the current buffer, never corrupt an open file.

static void pick_sd_filename(char *out, size_t out_len)
{
    if (!timesync_valid()) {
        strlcpy(out, SD_DIR "/unsynced.csv", out_len);
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    snprintf(out, out_len, SD_DIR "/%04d-%02d-%02d.csv",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

static bool sd_check_space(void)
{
    static int64_t last_check_us;
    static bool last_ok = true;
    if (esp_timer_get_time() - last_check_us > 600LL * 1000000 || !last_check_us) {
        last_check_us = esp_timer_get_time();
        sdcard_info_t info;
        if (sdcard_info(&info) == ESP_OK) {
            last_ok = info.free_bytes > SD_MIN_FREE_BYTES;
            if (!last_ok && !s_sd_full) {
                ESP_LOGE(TAG, "SD card nearly full (%llu MB free) — SD logging stopped",
                         info.free_bytes / (1024 * 1024));
            }
        }
    }
    return last_ok;
}

static void sd_flush(void)
{
    if (s_sd_buf_len == 0) {
        return;
    }
    s_sd_full = sdcard_mounted() && !sd_check_space();
    if (!sdcard_mounted() || s_sd_full) {
        s_sd_buf_len = 0;  // nowhere to put it; MQTT/spool still has the data
        return;
    }

    static bool dir_ready;
    if (!dir_ready) {
        mkdir(SD_DIR, 0775);
        dir_ready = true;
    }

    char path[40];
    pick_sd_filename(path, sizeof(path));

    struct stat st;
    bool fresh = stat(path, &st) != 0;
    FILE *f = fopen(path, "a");
    if (!f) {
        // EINVAL on a date-stamped name usually means FATFS long filenames
        // are off (stale sdkconfig missing CONFIG_FATFS_LFN_HEAP=y).
        ESP_LOGW(TAG, "fopen(%s) failed: %d%s", path, errno,
                 errno == EINVAL ? " (FATFS LFN disabled?)" : "");
        s_sd_buf_len = 0;
        return;
    }
    if (fresh) {
        write_csv_header(f);
    }
    size_t written = fwrite(s_sd_buf, 1, s_sd_buf_len, f);
    fclose(f);

    if (written == s_sd_buf_len) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        strlcpy(s_status.sd_file, path, sizeof(s_status.sd_file));
        xSemaphoreGive(s_mutex);
        strlcpy(s_sd_file, path, sizeof(s_sd_file));
    } else {
        ESP_LOGW(TAG, "short write to %s (%u/%u)", path,
                 (unsigned)written, (unsigned)s_sd_buf_len);
    }
    s_sd_buf_len = 0;
}

static void sd_append(const char *line, int len)
{
    if (!sdcard_mounted() || s_sd_full) {
        return;
    }
    if (s_sd_buf_len + len + 1 > sizeof(s_sd_buf)) {
        sd_flush();
    }
    if (s_sd_buf_len == 0) {
        s_sd_buf_first_us = esp_timer_get_time();
    }
    memcpy(s_sd_buf + s_sd_buf_len, line, len);
    s_sd_buf_len += len;
    s_sd_buf[s_sd_buf_len++] = '\n';

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.sd_rows++;
    xSemaphoreGive(s_mutex);
}

// ---------------------------------------------------------------------------
// Store-and-forward spool. Lines that failed to publish are appended to a
// file; a byte-offset cursor in a sidecar marks how far replay has been
// acked. Both files are only ever touched from the datalog task.

static void spool_load_state(void)
{
    if (s_spool_size >= 0 || !sdcard_mounted()) {
        return;
    }
    mkdir(SPOOL_DIR, 0775);

    struct stat st;
    s_spool_size = stat(SPOOL_FILE, &st) == 0 ? st.st_size : 0;
    s_spool_cursor = 0;

    FILE *f = fopen(SPOOL_CURSOR_FILE, "r");
    if (f) {
        if (fscanf(f, "%ld", &s_spool_cursor) != 1) {
            s_spool_cursor = 0;
        }
        fclose(f);
    }
    if (s_spool_cursor > s_spool_size) {
        s_spool_cursor = s_spool_size;
    }
    if (s_spool_size > s_spool_cursor) {
        ESP_LOGI(TAG, "spool has %ld bytes pending from before boot",
                 s_spool_size - s_spool_cursor);
    }
}

static void spool_save_cursor(void)
{
    FILE *f = fopen(SPOOL_CURSOR_FILE, "w");
    if (f) {
        fprintf(f, "%ld", s_spool_cursor);
        fclose(f);
    }
    s_spool_unsaved_acks = 0;
}

static void spool_reset(void)
{
    unlink(SPOOL_FILE);
    s_spool_size = 0;
    s_spool_cursor = 0;
    spool_save_cursor();
}

static void spool_append(const char *line)
{
    spool_load_state();
    if (s_spool_size < 0) {
        return;  // no SD card: offline rows survive only in the daily CSV
    }
    // Past the cap the oldest data would have to be trimmed from the front of
    // a FAT file, which isn't worth it: everything is already in the daily
    // CSVs, so sacrifice the backlog and keep spooling fresh data.
    if (s_spool_size - s_spool_cursor > (long)SPOOL_MAX_BYTES) {
        ESP_LOGE(TAG, "spool exceeded %llu MB — dropping backlog (data remains in daily CSVs)",
                 SPOOL_MAX_BYTES / (1024 * 1024));
        spool_reset();
    }

    FILE *f = fopen(SPOOL_FILE, "a");
    if (!f) {
        return;
    }
    int n = fprintf(f, "%s\n", line);
    fclose(f);
    if (n > 0) {
        s_spool_size += n;
    }
}

// Replay a few spooled lines per tick while the broker is reachable. Returns
// once the budget is used, the spool is drained, or a publish fails.
static void spool_replay_tick(void)
{
    spool_load_state();
    if (s_spool_size <= 0 || s_spool_cursor >= s_spool_size || !mqtt_connected()) {
        return;
    }

    FILE *f = fopen(SPOOL_FILE, "r");
    if (!f) {
        return;
    }
    fseek(f, s_spool_cursor, SEEK_SET);

    char line[DL_LINE_MAX];
    for (int i = 0; i < SPOOL_REPLAY_PER_TICK; i++) {
        long line_start = ftell(f);
        if (!fgets(line, sizeof(line), f)) {
            break;
        }
        size_t len = strlen(line);
        if (len && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }
        if (line[0] && mqtt_publish_telemetry(line) != ESP_OK) {
            break;  // broker went away again; cursor stays at this line
        }
        s_spool_cursor = line_start + len;
        if (line[0]) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.spool_replayed++;
            xSemaphoreGive(s_mutex);
        }
        if (++s_spool_unsaved_acks >= SPOOL_CURSOR_EVERY) {
            spool_save_cursor();
        }
    }
    fclose(f);

    if (s_spool_cursor >= s_spool_size) {
        ESP_LOGI(TAG, "spool drained (%lu rows replayed since boot)",
                 (unsigned long)s_status.spool_replayed);
        spool_reset();
    } else if (s_spool_unsaved_acks) {
        spool_save_cursor();
    }
}

// ---------------------------------------------------------------------------

static void handle_snapshot(const bms_snapshot_t *snap)
{
    static char line[DL_LINE_MAX];
    int len = serialize_csv(snap, line, sizeof(line));
    if (len <= 0 || len >= (int)sizeof(line)) {
        return;
    }

    sd_append(line, len);

    if (mqtt_publish_telemetry(line) == ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.mqtt_rows++;
        xSemaphoreGive(s_mutex);
    } else {
        spool_append(line);
    }
}

static void datalog_task(void *arg)
{
    bms_snapshot_t snap;
    while (true) {
        if (xQueueReceive(s_queue, &snap, pdMS_TO_TICKS(500))) {
            handle_snapshot(&snap);
        }
        if (s_sd_buf_len &&
            esp_timer_get_time() - s_sd_buf_first_us > SD_FLUSH_MS * 1000LL) {
            sd_flush();
        }
        spool_replay_tick();
    }
}

void datalog_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(bms_snapshot_t));
    xTaskCreate(datalog_task, "datalog", 6144, NULL, 4, NULL);
}

void datalog_submit(const bms_snapshot_t *snap)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.rows++;
    xSemaphoreGive(s_mutex);

    if (xQueueSend(s_queue, snap, 0) != pdTRUE) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.dropped++;
        xSemaphoreGive(s_mutex);
    }
}

void datalog_get_status(datalog_status_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mutex);
    out->sd_ok = sdcard_mounted() && !s_sd_full;
    out->spool_pending = s_spool_size > s_spool_cursor
                       ? (uint32_t)(s_spool_size - s_spool_cursor) : 0;
}
