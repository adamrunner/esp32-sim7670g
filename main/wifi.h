#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

#define WIFI_SSID_MAX 33   // 32-char SSID + NUL
#define WIFI_PASS_MAX 64   // up to 63-char WPA2 passphrase + NUL

typedef enum {
    WIFI_UI_BOOTING = 0,
    WIFI_UI_STA_CONNECTING,  // trying to join the home network
    WIFI_UI_STA_CONNECTED,   // joined the home network (SoftAP off)
    WIFI_UI_AP,              // SoftAP fallback active
} wifi_ui_state_t;

typedef struct {
    wifi_ui_state_t state;
    bool sta_configured;         // home-WiFi credentials present in NVS
    char ssid[WIFI_SSID_MAX];    // configured home SSID (empty if none)
    char ip[16];                 // current IPv4 (STA when connected, else AP)
    int rssi_dbm;                // STA RSSI when connected, else 0
    char ap_ssid[WIFI_SSID_MAX]; // the SoftAP's SSID
    int disconnect_count;        // STA disconnect events since boot
    uint32_t connect_count;
    uint32_t connect_attempt_count;
    uint32_t ap_start_count;
    uint32_t ap_client_count;
    uint32_t ap_association_count;
    uint32_t ap_disassociation_count;
    uint32_t transition_count;
    uint64_t last_transition_uptime_ms;
    uint64_t last_connected_uptime_ms;
    uint64_t last_disconnected_uptime_ms;
    uint64_t last_ap_start_uptime_ms;
    uint64_t last_ap_association_uptime_ms;
    uint64_t last_ap_disassociation_uptime_ms;
} wifi_ui_status_t;

// Bring up WiFi: try to join the stored home network (STA); fall back to the
// SoftAP if no credentials are stored or the join fails. Requires nvs_flash,
// esp_netif_init() and the default event loop to already exist (app_main does
// this). Runs a background supervisor task; returns immediately.
void wifi_init(void);

// Thread-safe snapshot of the current WiFi state for the web UI.
void wifi_get_status(wifi_ui_status_t *out);
void wifi_status_json(cJSON *root);

// Persist new home-WiFi credentials to NVS and re-evaluate the connection: the
// supervisor will attempt STA and, on success, drop the SoftAP. An empty ssid
// clears the stored credentials and returns to SoftAP mode. Thread-safe.
esp_err_t wifi_set_credentials(const char *ssid, const char *pass);
