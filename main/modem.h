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
    bool pdp_active;        // has an IP address
    int rssi_dbm;           // 0 = unknown
    char model[32];
    char fw_rev[48];
    char imei[24];
    char iccid[24];
    char operator_name[48];
    char rat[16];           // e.g. "LTE" from +CPSI
    char band[24];          // e.g. "EUTRAN-BAND2"
    char ip_addr[40];
    char apn[MODEM_APN_MAX];
    int64_t last_update_us; // esp_timer time of last successful poll
} modem_status_t;

// Start the modem UART and background poll task.
void modem_init(void);

// Thread-safe snapshot of the latest modem status.
void modem_get_status(modem_status_t *out);

// Send a raw AT command and capture the response (thread-safe).
// resp receives everything read until OK/ERROR or timeout.
esp_err_t modem_send_at(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms);

#define MODEM_PING_MAX_IPS 4

typedef struct {
    bool dns_ok;
    int dns_err;                        // +CDNSGIP error code when dns_ok == false
    int num_ips;
    char ips[MODEM_PING_MAX_IPS][40];
    bool ping_ok;                       // got a ping summary
    int sent, received, lost;
    int min_ms, max_ms, avg_ms;
    char raw[1024];                     // raw modem output, for display
} modem_netdiag_t;

// DNS-resolve `host` and ping it 4 times over the cellular connection.
// Blocks for up to ~1 min worst case (unreachable host). Returns
// ESP_ERR_INVALID_ARG for a malformed hostname; DNS/ping failures are
// reported in `out`, not the return value.
esp_err_t modem_ping_host(const char *host, modem_netdiag_t *out);

// Persist a new APN to NVS and re-run PDP context setup.
esp_err_t modem_set_apn(const char *apn);

// Currently configured APN (from NVS or default).
void modem_get_apn(char *out, size_t out_len);
