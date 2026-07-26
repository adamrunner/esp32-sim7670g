#include "mqtt.h"

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "nvs.h"

#include "datalog.h"
#include "ota.h"

static const char *TAG = "mqtt";

#define NVS_NS "mqttcfg"
#define PUBACK_TIMEOUT_MS 8000
#define STATUS_TOPIC_PREFIX "bms/status"
// The SD spool is the durability layer, so a failed/slow session shouldn't
// hoard RAM: keep esp-mqtt's own outbox small and let publishes fail fast.
#define OUTBOX_LIMIT_BYTES 4096

static SemaphoreHandle_t s_mutex;       // guards config/status/client swaps
static SemaphoreHandle_t s_pub_mutex;   // serializes publish+ack round-trips
static EventGroupHandle_t s_events;
#define EV_PUBACK BIT0

static esp_mqtt_client_handle_t s_client;
static mqtt_config_t s_cfg;
static mqtt_status_t s_status;
static volatile bool s_connected;
static volatile int s_pending_msg_id = -1;
static char s_boot_id[17];

static const char *reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:    return "power_on";
    case ESP_RST_EXT:        return "external";
    case ESP_RST_SW:         return "software";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:   return "task_watchdog";
    case ESP_RST_WDT:        return "watchdog";
    case ESP_RST_DEEPSLEEP:  return "deep_sleep";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_SDIO:       return "sdio";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_EFUSE:      return "efuse";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default:                 return "unknown";
    }
}

static void load_config(void)
{
    strlcpy(s_cfg.base_topic, "bms/telemetry", sizeof(s_cfg.base_topic));

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t len;
    len = sizeof(s_cfg.uri);        nvs_get_str(nvs, "uri", s_cfg.uri, &len);
    len = sizeof(s_cfg.username);   nvs_get_str(nvs, "user", s_cfg.username, &len);
    len = sizeof(s_cfg.password);   nvs_get_str(nvs, "pass", s_cfg.password, &len);
    len = sizeof(s_cfg.base_topic); nvs_get_str(nvs, "base", s_cfg.base_topic, &len);
    uint8_t enabled = 0;
    nvs_get_u8(nvs, "enabled", &enabled);
    s_cfg.enabled = enabled != 0;
    nvs_close(nvs);
}

static esp_err_t save_config(const mqtt_config_t *cfg)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(nvs, "uri", cfg->uri);
    nvs_set_str(nvs, "user", cfg->username);
    nvs_set_str(nvs, "pass", cfg->password);
    nvs_set_str(nvs, "base", cfg->base_topic);
    nvs_set_u8(nvs, "enabled", cfg->enabled ? 1 : 0);
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void set_last_error(const char *msg)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(s_status.last_error, msg, sizeof(s_status.last_error));
    xSemaphoreGive(s_mutex);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        set_last_error("");
        ESP_LOGI(TAG, "connected to broker");
        mqtt_publish_status();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected from broker");
        break;
    case MQTT_EVENT_PUBLISHED:
        if (event->msg_id == s_pending_msg_id) {
            xEventGroupSetBits(s_events, EV_PUBACK);
        }
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            set_last_error("transport error (broker unreachable?)");
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            set_last_error("connection refused (bad credentials?)");
        }
        break;
    default:
        break;
    }
}

// Stop any running client and start a fresh one from s_cfg. Caller holds
// s_mutex. With no route yet, esp-mqtt keeps retrying on its own (~10 s
// backoff), so there is nothing to coordinate with the modem/WiFi modules.
static void restart_client_locked(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
    }

    if (!s_cfg.enabled || s_cfg.uri[0] == '\0') {
        return;
    }

    char client_id[48];
    datalog_device_id(client_id, sizeof(client_id));

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_cfg.uri,
        .credentials.client_id = client_id,
        .outbox.limit = OUTBOX_LIMIT_BYTES,
        .network.disable_auto_reconnect = false,
    };
    if (s_cfg.username[0]) {
        cfg.credentials.username = s_cfg.username;
        cfg.credentials.authentication.password = s_cfg.password;
    }
    if (strncmp(s_cfg.uri, "mqtts://", 8) == 0) {
        cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        set_last_error("client init failed (bad URI?)");
        ESP_LOGE(TAG, "esp_mqtt_client_init failed for %s", s_cfg.uri);
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "client started, broker %s", s_cfg.uri);
}

void mqtt_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_pub_mutex = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    snprintf(s_boot_id, sizeof(s_boot_id), "%08" PRIx32 "%08" PRIx32,
             esp_random(), esp_random());

    load_config();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    restart_client_locked();
    xSemaphoreGive(s_mutex);
}

esp_err_t mqtt_set_config(const mqtt_config_t *cfg)
{
    esp_err_t err = save_config(cfg);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cfg = *cfg;
    restart_client_locked();
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void mqtt_get_config(mqtt_config_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_mutex);
}

bool mqtt_connected(void)
{
    return s_connected;
}

void mqtt_get_status(mqtt_status_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_status;
    strlcpy(out->uri, s_cfg.uri, sizeof(out->uri));
    strlcpy(out->base_topic, s_cfg.base_topic, sizeof(out->base_topic));
    out->state = !s_cfg.enabled || s_cfg.uri[0] == '\0' ? MQTT_UI_DISABLED
               : s_connected                            ? MQTT_UI_CONNECTED
                                                        : MQTT_UI_CONNECTING;
    xSemaphoreGive(s_mutex);
}

// Append the "mqtt" runtime-status object to the shared /api/status response.
void mqtt_status_json(cJSON *root)
{
    mqtt_status_t m;
    mqtt_get_status(&m);
    cJSON *mq = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddStringToObject(mq, "state", m.state == MQTT_UI_CONNECTED  ? "connected"
                                       : m.state == MQTT_UI_CONNECTING ? "connecting"
                                                                       : "disabled");
    cJSON_AddStringToObject(mq, "uri", m.uri);
    cJSON_AddStringToObject(mq, "base_topic", m.base_topic);
    cJSON_AddNumberToObject(mq, "published", m.published);
    cJSON_AddNumberToObject(mq, "publish_fails", m.publish_fails);
    cJSON_AddStringToObject(mq, "last_error", m.last_error);
}

esp_err_t mqtt_publish_status(void)
{
    char device_id[48];
    char topic[sizeof(STATUS_TOPIC_PREFIX) + sizeof(device_id)];
    datalog_device_id(device_id, sizeof(device_id));
    snprintf(topic, sizeof(topic), "%s/%s", STATUS_TOPIC_PREFIX, device_id);

    ota_status_t ota;
    ota_get_status(&ota);
    const esp_app_desc_t *app = esp_app_get_description();

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddBoolToObject(root, "online", true);
    cJSON_AddStringToObject(root, "firmware_version", ota.running_version);
    cJSON_AddStringToObject(root, "ota_slot", ota.running_slot);
    cJSON_AddBoolToObject(root, "pending_verify", ota.pending_verify);
    cJSON_AddStringToObject(root, "boot_id", s_boot_id);
    cJSON_AddStringToObject(root, "reset_reason", reset_reason_str(esp_reset_reason()));
    cJSON_AddStringToObject(root, "idf_version", app->idf_ver);
    cJSON_AddStringToObject(root, "build_date", app->date);
    cJSON_AddStringToObject(root, "build_time", app->time);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int msg_id = (!s_client || !s_connected)
               ? -1
               : esp_mqtt_client_enqueue(s_client, topic, payload, 0, 1, 1, true);
    xSemaphoreGive(s_mutex);
    cJSON_free(payload);

    if (msg_id < 0) {
        ESP_LOGW(TAG, "could not queue retained status for %s", device_id);
        return msg_id == -2 ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "queued retained status on %s (message id %d)", topic, msg_id);
    return ESP_OK;
}

esp_err_t mqtt_publish_telemetry(const char *payload)
{
    if (!s_client || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[MQTT_TOPIC_MAX + 48];
    char device_id[48];
    datalog_device_id(device_id, sizeof(device_id));
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(topic, sizeof(topic), "%s/%s", s_cfg.base_topic, device_id);
    xSemaphoreGive(s_mutex);

    xSemaphoreTake(s_pub_mutex, portMAX_DELAY);
    xEventGroupClearBits(s_events, EV_PUBACK);

    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
    if (msg_id < 0) {
        xSemaphoreGive(s_pub_mutex);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.publish_fails++;
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    s_pending_msg_id = msg_id;
    EventBits_t bits = xEventGroupWaitBits(s_events, EV_PUBACK, pdTRUE, pdTRUE,
                                           pdMS_TO_TICKS(PUBACK_TIMEOUT_MS));
    s_pending_msg_id = -1;
    xSemaphoreGive(s_pub_mutex);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (bits & EV_PUBACK) {
        s_status.published++;
    } else {
        s_status.publish_fails++;
    }
    xSemaphoreGive(s_mutex);

    return (bits & EV_PUBACK) ? ESP_OK : ESP_ERR_TIMEOUT;
}
