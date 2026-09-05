#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

typedef struct fake_mqtt_client *esp_mqtt_client_handle_t;

typedef struct {
    struct {
        struct {
            const char *uri;
        } address;
        struct {
            void (*crt_bundle_attach)(void);
        } verification;
    } broker;
    struct {
        const char *client_id;
        const char *username;
        struct {
            const char *password;
        } authentication;
    } credentials;
    struct {
        struct {
            const char *topic;
            const char *msg;
            int qos;
            bool retain;
        } last_will;
    } session;
    struct {
        int limit;
    } outbox;
    struct {
        bool disable_auto_reconnect;
    } network;
} esp_mqtt_client_config_t;

typedef enum {
    MQTT_EVENT_CONNECTED = 0,
    MQTT_EVENT_DISCONNECTED,
    MQTT_EVENT_PUBLISHED,
    MQTT_EVENT_ERROR,
} esp_mqtt_event_id_t;

typedef enum {
    MQTT_ERROR_TYPE_NONE = 0,
    MQTT_ERROR_TYPE_TCP_TRANSPORT,
    MQTT_ERROR_TYPE_CONNECTION_REFUSED,
} esp_mqtt_error_type_t;

typedef struct {
    esp_mqtt_error_type_t error_type;
} esp_mqtt_error_codes_t;

typedef struct {
    int msg_id;
    esp_mqtt_error_codes_t *error_handle;
} esp_mqtt_event_t;
typedef esp_mqtt_event_t *esp_mqtt_event_handle_t;

typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);

esp_mqtt_client_handle_t esp_mqtt_client_init(
    const esp_mqtt_client_config_t *config);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client,
                                         int32_t event_id,
                                         esp_event_handler_t handler,
                                         void *handler_args);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client);
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client);
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t client,
                            const char *topic, const char *data, int len,
                            int qos, int retain);
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t client,
                            const char *topic, const char *data, int len,
                            int qos, int retain, bool store);
