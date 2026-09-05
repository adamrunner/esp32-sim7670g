#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "mqtt.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "event_journal.h"
#include "ota.h"
#include "timesync.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"

struct fake_semaphore {
    bool locked;
};

struct fake_event_group {
    EventBits_t bits;
};

struct fake_queue {
    size_t item_size;
    bool occupied;
    unsigned char item[1024];
};

struct fake_mqtt_client {
    int unused;
};

static struct fake_mqtt_client s_fake_client;
static bool s_client_init_fails;
static int s_recursive_take_count;
static int s_invalid_give_count;
static int s_client_start_count;
static mqtt_config_t s_nvs_config;
static bool s_fail_next_queue_create;
static bool s_ack_publishes;
static esp_event_handler_t s_mqtt_event_handler;
static char s_will_payloads[4][256];
static size_t s_will_payload_count;
static char s_enqueued_payload[1024];
static char s_published_payloads[4][256];
static size_t s_published_payload_count;
static uint32_t s_random_state;

static void reset_fakes(void)
{
    s_client_init_fails = false;
    s_recursive_take_count = 0;
    s_invalid_give_count = 0;
    s_client_start_count = 0;
    s_fail_next_queue_create = false;
    s_ack_publishes = false;
    s_mqtt_event_handler = NULL;
    s_will_payload_count = 0;
    s_published_payload_count = 0;
    s_enqueued_payload[0] = '\0';
    memset(&s_nvs_config, 0, sizeof(s_nvs_config));
    s_nvs_config.enabled = true;
    strcpy(s_nvs_config.uri, "mqtt://test.invalid:1883");
    strcpy(s_nvs_config.base_topic, "bms/telemetry");
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    (void)length;
    if (s_fail_next_queue_create) {
        s_fail_next_queue_create = false;
        return NULL;
    }
    assert(item_size <= sizeof(((struct fake_queue *)0)->item));
    struct fake_queue *queue = calloc(1, sizeof(*queue));
    assert(queue);
    queue->item_size = item_size;
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks)
{
    (void)ticks;
    assert(queue);
    if (queue->occupied) {
        return pdFALSE;
    }
    memcpy(queue->item, item, queue->item_size);
    queue->occupied = true;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks)
{
    (void)ticks;
    assert(queue);
    if (!queue->occupied) {
        return pdFALSE;
    }
    memcpy(item, queue->item, queue->item_size);
    queue->occupied = false;
    return pdTRUE;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return calloc(1, sizeof(struct fake_semaphore));
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    (void)ticks;
    assert(semaphore);
    if (semaphore->locked) {
        s_recursive_take_count++;
        return pdFALSE;
    }
    semaphore->locked = true;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore);
    if (!semaphore->locked) {
        s_invalid_give_count++;
        return pdFALSE;
    }
    semaphore->locked = false;
    return pdTRUE;
}

EventGroupHandle_t xEventGroupCreate(void)
{
    return calloc(1, sizeof(struct fake_event_group));
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits)
{
    EventBits_t before = group->bits;
    group->bits &= ~bits;
    return before;
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits)
{
    group->bits |= bits;
    return group->bits;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                               BaseType_t clear_on_exit,
                               BaseType_t wait_for_all, TickType_t ticks)
{
    (void)wait_for_all;
    (void)ticks;
    EventBits_t result = group->bits & bits;
    if (s_ack_publishes) {
        result |= bits;
    }
    if (clear_on_exit) {
        group->bits &= ~bits;
    }
    return result;
}

BaseType_t xTaskCreate(TaskFunction_t task, const char *name,
                       uint32_t stack_depth, void *argument,
                       UBaseType_t priority, TaskHandle_t *handle)
{
    (void)task;
    (void)name;
    (void)stack_depth;
    (void)argument;
    (void)priority;
    (void)handle;
    return pdPASS;
}

TickType_t xTaskGetTickCount(void)
{
    return 1;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}

esp_mqtt_client_handle_t esp_mqtt_client_init(
    const esp_mqtt_client_config_t *config)
{
    assert(config);
    assert(s_will_payload_count < 4);
    snprintf(s_will_payloads[s_will_payload_count],
             sizeof(s_will_payloads[s_will_payload_count]), "%s",
             config->session.last_will.msg);
    s_will_payload_count++;
    return s_client_init_fails ? NULL : &s_fake_client;
}

esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client,
                                         int32_t event_id,
                                         esp_event_handler_t handler,
                                         void *handler_args)
{
    (void)client;
    (void)event_id;
    s_mqtt_event_handler = handler;
    (void)handler_args;
    return ESP_OK;
}

esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client)
{
    assert(client == &s_fake_client);
    s_client_start_count++;
    return ESP_OK;
}

esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client)
{
    (void)client;
    return ESP_OK;
}

esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client)
{
    (void)client;
    return ESP_OK;
}

int esp_mqtt_client_publish(esp_mqtt_client_handle_t client,
                            const char *topic, const char *data, int len,
                            int qos, int retain)
{
    (void)client;
    (void)topic;
    assert(s_published_payload_count < 4);
    snprintf(s_published_payloads[s_published_payload_count],
             sizeof(s_published_payloads[s_published_payload_count]), "%s",
             data);
    s_published_payload_count++;
    (void)len;
    (void)qos;
    (void)retain;
    return 20 + (int)s_published_payload_count;
}

int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t client,
                            const char *topic, const char *data, int len,
                            int qos, int retain, bool store)
{
    (void)client;
    (void)topic;
    snprintf(s_enqueued_payload, sizeof(s_enqueued_payload), "%s", data);
    (void)len;
    (void)qos;
    (void)retain;
    (void)store;
    return 40;
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    (void)name;
    (void)mode;
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key,
                      char *value, size_t *length)
{
    (void)handle;
    const char *stored = "";
    if (strcmp(key, "uri") == 0) {
        stored = s_nvs_config.uri;
    } else if (strcmp(key, "user") == 0) {
        stored = s_nvs_config.username;
    } else if (strcmp(key, "pass") == 0) {
        stored = s_nvs_config.password;
    } else if (strcmp(key, "base") == 0) {
        stored = s_nvs_config.base_topic;
    }
    size_t needed = strlen(stored) + 1;
    if (*length >= needed) {
        memcpy(value, stored, needed);
    }
    *length = needed;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    (void)handle;
    (void)key;
    *value = s_nvs_config.enabled ? 1 : 0;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key,
                      const char *value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

void datalog_device_id(char *out, size_t out_len)
{
    snprintf(out, out_len, "gw-host-test");
}

bool sdcard_mounted(void)
{
    return false;
}

bool event_core_redact_json(const char *input, char *output, size_t output_len)
{
    snprintf(output, output_len, "%s", input);
    return true;
}

bool event_core_rate_limited(bool previously_emitted,
                             uint64_t last_emitted_ms, uint64_t now_ms,
                             uint32_t interval_ms)
{
    return previously_emitted && now_ms - last_emitted_ms < interval_ms;
}

int event_core_repair_tail(const char *path)
{
    (void)path;
    return 0;
}

int event_core_append(const char *directory, const char *json,
                      size_t json_len, bool critical, size_t max_file_bytes,
                      unsigned file_count)
{
    (void)directory;
    (void)json;
    (void)json_len;
    (void)critical;
    (void)max_file_bytes;
    (void)file_count;
    return 0;
}

void ota_get_status(ota_status_t *out)
{
    memset(out, 0, sizeof(*out));
    strcpy(out->running_version, "host-test");
    strcpy(out->running_slot, "ota_0");
}

esp_err_t ota_acknowledge_rollback_evidence(void)
{
    return ESP_OK;
}

bool timesync_valid(void)
{
    return false;
}

void timesync_get_status(timesync_status_t *out)
{
    memset(out, 0, sizeof(*out));
}

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t description = {
        .version = "host-test",
        .date = "Sep 04 2026",
        .time = "00:00:00",
        .idf_ver = "host",
    };
    return &description;
}

uint32_t esp_random(void)
{
    s_random_state = s_random_state * 1664525U + 1013904223U;
    return s_random_state;
}

esp_reset_reason_t esp_reset_reason(void)
{
    return ESP_RST_POWERON;
}

int64_t esp_timer_get_time(void)
{
    return 1000;
}

void esp_crt_bundle_attach(void)
{
}

const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "host error";
}

static void test_init_failure_does_not_reacquire_mutex(void)
{
    reset_fakes();
    s_client_init_fails = true;

    mqtt_init();

    mqtt_status_t status;
    mqtt_get_status(&status);
    assert(strcmp(status.last_error, "client init failed (bad URI?)") == 0);
    assert(s_recursive_take_count == 0);
    assert(s_invalid_give_count == 0);

    s_client_init_fails = false;
    mqtt_config_t config = s_nvs_config;
    assert(mqtt_set_config(&config) == ESP_OK);
    assert(s_client_start_count == 1);
    assert(s_recursive_take_count == 0);
    assert(s_invalid_give_count == 0);
}

static const char *json_boot_id(const char *document)
{
    cJSON *root = cJSON_Parse(document);
    assert(root);
    cJSON *boot_id = cJSON_GetObjectItemCaseSensitive(root, "boot_id");
    assert(cJSON_IsString(boot_id));
    static char value[17];
    snprintf(value, sizeof(value), "%s", boot_id->valuestring);
    cJSON_Delete(root);
    return value;
}

static void assert_boot_id_document(const char *document,
                                    const char *expected_boot_id)
{
    assert(strcmp(json_boot_id(document), expected_boot_id) == 0);
}

static bool assert_journal_event_boot_id(const cJSON *event, size_t index,
                                         void *context)
{
    const char *expected_boot_id = context;
    const cJSON *boot_id = cJSON_GetObjectItemCaseSensitive(event, "boot_id");
    assert(index == 0);
    assert(cJSON_IsString(boot_id));
    assert(strcmp(boot_id->valuestring, expected_boot_id) == 0);
    return true;
}

static void test_shared_boot_identity(bool journal_setup_fails)
{
    reset_fakes();
    s_ack_publishes = true;
    s_fail_next_queue_create = journal_setup_fails;

    event_journal_init();
    const char *journal_boot_id = event_journal_boot_id();
    assert(strlen(journal_boot_id) == 16);
    char expected_boot_id[17];
    snprintf(expected_boot_id, sizeof(expected_boot_id), "%s",
             journal_boot_id);
    if (!journal_setup_fails) {
        event_journal_status_t journal_status;
        event_journal_get_status(&journal_status);
        assert(strcmp(journal_status.boot_id, expected_boot_id) == 0);
        assert(event_journal_visit_events_json(
            1, assert_journal_event_boot_id, expected_boot_id));
    }

    mqtt_init();
    assert(s_will_payload_count == 1);
    assert_boot_id_document(s_will_payloads[0], expected_boot_id);

    assert(s_mqtt_event_handler);
    esp_mqtt_event_t connected = {0};
    s_mqtt_event_handler(NULL, NULL, MQTT_EVENT_CONNECTED, &connected);
    assert_boot_id_document(s_enqueued_payload, expected_boot_id);

    mqtt_maintenance_tick();
    assert(s_published_payload_count == 1);
    assert_boot_id_document(s_published_payloads[0], expected_boot_id);

    mqtt_config_t config = s_nvs_config;
    assert(mqtt_set_config(&config) == ESP_OK);
    assert(s_published_payload_count == 2);
    assert_boot_id_document(s_published_payloads[1], expected_boot_id);
    assert(s_will_payload_count == 2);
    assert_boot_id_document(s_will_payloads[1], expected_boot_id);
    assert(strcmp(event_journal_boot_id(), expected_boot_id) == 0);

    printf("BOOT_ID=%s\n", expected_boot_id);
}

int main(int argc, char **argv)
{
    if (argc == 1 || strcmp(argv[1], "a1") == 0) {
        test_init_failure_does_not_reacquire_mutex();
    } else if (strcmp(argv[1], "boot-normal") == 0) {
        assert(argc == 3);
        s_random_state = (uint32_t)strtoul(argv[2], NULL, 0);
        test_shared_boot_identity(false);
    } else if (strcmp(argv[1], "boot-journal-fail") == 0) {
        assert(argc == 3);
        s_random_state = (uint32_t)strtoul(argv[2], NULL, 0);
        test_shared_boot_identity(true);
    } else {
        return 2;
    }
    puts("MQTT lifecycle tests: ok");
    return 0;
}
