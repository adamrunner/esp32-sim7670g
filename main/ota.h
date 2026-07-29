#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define OTA_VERSION_MAX 32      // matches esp_app_desc_t.version
#define OTA_ERRMSG_MAX  96
#define OTA_URL_MAX     256
#define OTA_FAILURE_STAGE_MAX 24

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CHECKING,     // fetching/parsing the manifest
    OTA_STATE_DOWNLOADING,  // esp_https_ota transfer in progress
    OTA_STATE_VERIFYING,    // sha256 read-back of the written slot
    OTA_STATE_WAIT_REBOOT,  // image installed, restarting shortly
    OTA_STATE_ERROR,        // last check/update failed (see error)
} ota_state_t;

typedef struct {
    char stage[OTA_FAILURE_STAGE_MAX]; // e.g. "manifest_connect"
    int esp_err;                       // top-level ESP-IDF error
    int tls_err;                       // ESP-TLS error, 0 when unavailable
    int mbedtls_err;                   // underlying mbedTLS error
    int tls_flags;                     // certificate verification flags
    int sock_errno;                    // socket errno, 0 when unavailable
    uint32_t free_heap;                // heap snapshot at failure
    uint32_t largest_free_block;
    uint32_t minimum_free_heap;
} ota_failure_t;

typedef struct {
    ota_state_t state;
    char running_version[OTA_VERSION_MAX];  // esp_app_get_description()
    char running_slot[17];                  // partition label, e.g. "ota_0"
    bool pending_verify;                    // running image not yet confirmed
    bool rollback_detected;                 // prior OTA target failed verification
    char rollback_from_version[OTA_VERSION_MAX];   // failed target version
    char rollback_target_version[OTA_VERSION_MAX]; // restored version
    bool update_available;                  // manifest version != running
    char available_version[OTA_VERSION_MAX];
    int progress_pct;                       // 0-100 while downloading
    int bytes_read;                         // downloaded so far (this session)
    int image_size;                         // total image size, 0 = unknown
    char error[OTA_ERRMSG_MAX];             // valid when state == OTA_STATE_ERROR
    int64_t last_check_us;                  // esp_timer time of last manifest fetch, 0 = never
    bool last_check_ok;
    int manifest_attempts;                  // attempts used by the current/last cycle
    int download_attempts;
    int64_t next_check_us;                  // scheduled monotonic time, 0 while active
    uint32_t passive_retry_count;           // retry waits; never manipulates PPP
    uint32_t control_plane_defer_count;      // routine checks deferred for SoftAP use
    int64_t last_control_plane_defer_us;
    char last_control_plane_defer_reason[24];
    ota_failure_t failure;                  // structured details for the last failure
} ota_status_t;

// Optional overrides for a manually triggered check (web UI / testing).
typedef struct {
    char url[OTA_URL_MAX];  // manifest URL; empty = built-in default
    bool force_cellular;    // bind the HTTP client to the PPP netif
} ota_check_opts_t;

// Start the OTA background task. On a pending-verify boot it first runs the
// self-test (HTTPS reach of the update server) and confirms or rolls back;
// afterwards it checks the manifest hourly and auto-installs on a version
// mismatch. Call after wifi/modem init.
void ota_init(void);

// Thread-safe snapshot of the OTA state for the web UI.
void ota_get_status(ota_status_t *out);

// Clear persisted rollback evidence only after its MQTT status event has been
// acknowledged by the broker. Returns an NVS error so the evidence survives
// retry when persistence cannot be updated.
esp_err_t ota_acknowledge_rollback_evidence(void);

// Trigger an immediate manifest check (and update, on version mismatch).
// opts may be NULL for defaults. Returns ESP_ERR_INVALID_STATE if a check or
// update is already running.
esp_err_t ota_check_now(const ota_check_opts_t *opts);
