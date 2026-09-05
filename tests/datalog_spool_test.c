#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


typedef int esp_err_t;
typedef int SemaphoreHandle_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define EVENT_SEVERITY_INFO 1
#define DL_LINE_MAX 640
#define SPOOL_REPLAY_PER_TICK 2
#define SPOOL_CURSOR_EVERY 16
#define portMAX_DELAY 0
#define ESP_LOGI(...) ((void)0)

typedef struct {
    uint32_t spool_replayed;
    uint32_t spool_replay_starts;
    uint32_t spool_cursor_failures;
    uint64_t last_spool_replay_uptime_ms;
    uint64_t last_spool_drain_uptime_ms;
    uint64_t last_spool_failure_uptime_ms;
} datalog_status_t;

static char g_spool_dir[1024];
static char g_spool_file[1024];
static char g_cursor_file[1024];

#define SPOOL_DIR g_spool_dir
#define SPOOL_FILE g_spool_file
#define SPOOL_CURSOR_FILE g_cursor_file

static SemaphoreHandle_t s_mutex;
static datalog_status_t s_status;
static long s_spool_size = -1;
static long s_spool_cursor;
static int s_spool_unsaved_acks;
static bool s_spool_replay_active;

static bool g_connected;
static esp_err_t g_publish_outcomes[8];
static size_t g_publish_outcome_count;
static size_t g_publish_calls;
static char g_published[8][64];

static int xSemaphoreTake(SemaphoreHandle_t mutex, int ticks)
{
    (void)mutex;
    (void)ticks;
    return 1;
}

static int xSemaphoreGive(SemaphoreHandle_t mutex)
{
    (void)mutex;
    return 1;
}

static int64_t esp_timer_get_time(void)
{
    return 123456000;
}

static bool sdcard_mounted(void)
{
    return true;
}

static bool mqtt_connected(void)
{
    return g_connected;
}

static esp_err_t mqtt_publish_telemetry(const char *line)
{
    assert(g_publish_calls < 8);
    snprintf(
        g_published[g_publish_calls], sizeof(g_published[g_publish_calls]),
        "%s", line
    );
    esp_err_t outcome = g_publish_calls < g_publish_outcome_count
        ? g_publish_outcomes[g_publish_calls] : ESP_OK;
    g_publish_calls++;
    return outcome;
}

static void event_journal_emit(
    const char *component,
    const char *event_name,
    int severity,
    const char *reason,
    const char *details,
    bool critical,
    uint32_t interval_ms
)
{
    (void)component;
    (void)event_name;
    (void)severity;
    (void)reason;
    (void)details;
    (void)critical;
    (void)interval_ms;
}

static void note_storage_failure(
    const char *event_name,
    const char *reason,
    int error_number
)
{
    (void)event_name;
    (void)reason;
    (void)error_number;
}

/* PRODUCTION_SPOOL_FUNCTIONS */

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

static long read_cursor(void)
{
    FILE *file = fopen(g_cursor_file, "r");
    assert(file);
    long value = -1;
    assert(fscanf(file, "%ld", &value) == 1);
    assert(fclose(file) == 0);
    return value;
}

static void prepare_case(
    const char *root,
    const char *name,
    const char *spool_contents
)
{
    assert(snprintf(g_spool_dir, sizeof(g_spool_dir), "%s/%s", root, name) > 0);
    assert(mkdir(g_spool_dir, 0700) == 0);
    assert(snprintf(
        g_spool_file, sizeof(g_spool_file), "%s/bms.csv", g_spool_dir
    ) > 0);
    assert(snprintf(
        g_cursor_file, sizeof(g_cursor_file), "%s/bms.cursor", g_spool_dir
    ) > 0);
    write_text(g_spool_file, spool_contents);
    write_text(g_cursor_file, "0");

    memset(&s_status, 0, sizeof(s_status));
    s_spool_size = -1;
    s_spool_cursor = 0;
    s_spool_unsaved_acks = 0;
    s_spool_replay_active = false;
    g_connected = true;
    g_publish_outcome_count = 0;
    g_publish_calls = 0;
    memset(g_published, 0, sizeof(g_published));
}

static void set_publish_outcomes(const esp_err_t *outcomes, size_t count)
{
    assert(count <= 8);
    memcpy(g_publish_outcomes, outcomes, count * sizeof(*outcomes));
    g_publish_outcome_count = count;
    g_publish_calls = 0;
    memset(g_published, 0, sizeof(g_published));
}

static void test_zero_progress(const char *root)
{
    prepare_case(root, "zero-progress", "row-a\nrow-b\n");
    g_connected = false;
    spool_replay_tick();
    assert(s_spool_cursor == 0);
    assert(read_cursor() == 0);
    assert(g_publish_calls == 0);
}

static void test_one_success_then_failure(const char *root)
{
    prepare_case(root, "one-success", "row-a\nrow-b\nrow-c\n");
    const esp_err_t outcomes[] = {ESP_OK, ESP_FAIL};
    set_publish_outcomes(outcomes, 2);
    spool_replay_tick();
    assert(s_spool_cursor == 6);
    assert(read_cursor() == 6);
    assert(g_publish_calls == 2);
    assert(strcmp(g_published[0], "row-a") == 0);
    assert(strcmp(g_published[1], "row-b") == 0);

    const esp_err_t retry[] = {ESP_FAIL};
    set_publish_outcomes(retry, 1);
    spool_replay_tick();
    assert(g_publish_calls == 1);
    assert(strcmp(g_published[0], "row-b") == 0);
    assert(read_cursor() == 6);
}

static void test_two_successful_rows_checkpoint(const char *root)
{
    prepare_case(root, "two-success", "row-a\nrow-b\nrow-c\n");
    spool_replay_tick();
    assert(s_spool_cursor == 12);
    assert(read_cursor() == 12);
    assert(g_publish_calls == 2);
    assert(strcmp(g_published[0], "row-a") == 0);
    assert(strcmp(g_published[1], "row-b") == 0);
}

static void test_blank_line_progress(const char *root)
{
    prepare_case(root, "blank-line", "\nrow-a\nrow-b\n");
    const esp_err_t outcomes[] = {ESP_FAIL};
    set_publish_outcomes(outcomes, 1);
    spool_replay_tick();
    assert(s_spool_cursor == 1);
    assert(read_cursor() == 1);
    assert(g_publish_calls == 1);
    assert(strcmp(g_published[0], "row-a") == 0);

    s_spool_size = -1;
    s_spool_cursor = 0;
    s_spool_replay_active = false;
    set_publish_outcomes(outcomes, 1);
    spool_replay_tick();
    assert(g_publish_calls == 1);
    assert(strcmp(g_published[0], "row-a") == 0);
}

static void test_drain_resets_files(const char *root)
{
    prepare_case(root, "drain", "row-a\nrow-b\n");
    spool_replay_tick();
    assert(s_spool_size == 0);
    assert(s_spool_cursor == 0);
    assert(access(g_spool_file, F_OK) != 0);
    assert(read_cursor() == 0);
    assert(s_status.spool_replayed == 2);
}

static void test_cursor_write_failure_retry_window(const char *root)
{
    prepare_case(root, "cursor-failure", "row-a\nrow-b\nrow-c\n");
    assert(unlink(g_cursor_file) == 0);
    assert(mkdir(g_cursor_file, 0700) == 0);

    const esp_err_t outcomes[] = {ESP_OK, ESP_FAIL};
    set_publish_outcomes(outcomes, 2);
    spool_replay_tick();
    assert(s_spool_cursor == 6);
    assert(s_status.spool_cursor_failures == 1);

    const esp_err_t fail[] = {ESP_FAIL};
    set_publish_outcomes(fail, 1);
    spool_replay_tick();
    assert(strcmp(g_published[0], "row-b") == 0);

    s_spool_size = -1;
    s_spool_cursor = 0;
    s_spool_replay_active = false;
    set_publish_outcomes(fail, 1);
    spool_replay_tick();
    assert(strcmp(g_published[0], "row-a") == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    test_zero_progress(argv[1]);
    test_one_success_then_failure(argv[1]);
    test_two_successful_rows_checkpoint(argv[1]);
    test_blank_line_progress(argv[1]);
    test_drain_resets_files(argv[1]);
    test_cursor_write_failure_retry_window(argv[1]);
    puts("datalog spool replay tests: ok");
    return 0;
}
