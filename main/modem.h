#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define MODEM_APN_MAX 64

typedef struct {
    bool at_ok;             // modem responds to AT
    bool sim_ready;         // +CPIN: READY
    int reg_status;         // +CEREG stat: 1=home, 5=roaming, 2=searching, 0/3/4=not registered
    bool ppp_up;            // PPP session established, ESP32 has cellular IP connectivity
    bool pdp_active;        // same as ppp_up (kept for the LED / web UI)
    int rssi_dbm;           // 0 = unknown
    char model[32];
    char fw_rev[48];
    char imei[24];
    char iccid[24];
    char operator_name[48];
    char rat[16];           // e.g. "LTE" from +CPSI
    char band[24];          // e.g. "EUTRAN-BAND2"
    int uart_baud;          // current host<->modem UART rate, 0 = unknown
    char ip_addr[40];
    char apn[MODEM_APN_MAX];
    int64_t last_update_us; // esp_timer time of last successful poll
} modem_status_t;

typedef struct {
    bool powered;           // GNSS engine is on (AT+CGNSSPWR=1 accepted)
    bool has_fix;           // most recent CGNSSINFO poll had a position
    double lat, lon;        // decimal degrees, last known fix (valid if fix_time_us != 0)
    float alt_m;
    float speed_kmh;
    float course_deg;
    int sats;               // satellites in view, summed across constellations
    int sats_used;          // satellites used in the fix (<NoSV>), 0 if not reported
    float hdop;
    char utc[24];           // fix timestamp, "2026-07-13 04:37:54" (UTC)
    int64_t fix_time_us;    // esp_timer time of last fix, 0 = never
    int64_t poll_time_us;   // esp_timer time of last successful CGNSSINFO poll
} modem_gnss_t;

// Create the PPP netif + esp_modem DCE and start the background task that
// polls status and keeps the PPP data connection dialed while registered.
// Requires esp_netif_init() and the default event loop to exist already.
void modem_init(void);

// Thread-safe snapshot of the latest GNSS state. Position fields keep the
// last known fix when the current poll has none (check has_fix/fix_time_us).
void modem_get_gnss(modem_gnss_t *out);

// Thread-safe snapshot of the latest modem status.
void modem_get_status(modem_status_t *out);

// Send a raw AT command and capture the response (thread-safe).
// resp receives everything read until OK/ERROR or timeout.
esp_err_t modem_send_at(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms);

#define MODEM_PING_MAX_IPS 4

typedef struct {
    bool dns_ok;
    int dns_err;                        // getaddrinfo error code when dns_ok == false
    int num_ips;
    char ips[MODEM_PING_MAX_IPS][40];
    bool ping_ok;                       // ping session completed (stats valid)
    int sent, received, lost;
    int min_ms, max_ms, avg_ms;
    char raw[1024];                     // ping-style transcript, for display
} modem_netdiag_t;

// DNS-resolve `host` and ping it 4 times using the ESP32's own lwIP stack,
// i.e. through the PPP link. Blocks for up to ~20 s worst case (unreachable
// host). Returns ESP_ERR_INVALID_ARG for a malformed hostname; DNS/ping
// failures are reported in `out`, not the return value.
esp_err_t modem_ping_host(const char *host, modem_netdiag_t *out);

// Persist a new APN to NVS and re-run PDP context setup.
esp_err_t modem_set_apn(const char *apn);

// Currently configured APN (from NVS or default).
void modem_get_apn(char *out, size_t out_len);
