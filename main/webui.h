#pragma once

#include <stdint.h>

// Starts a WiFi SoftAP and an HTTP server with the status/config UI.
// Connect to the AP and browse to http://192.168.4.1/
void webui_init(void);

// Monotonic timestamp of the most recent local HTTP request. OTA uses this
// read-only signal to keep routine TLS work out of an active control-plane
// session. Returns zero before the first request.
uint64_t webui_last_request_uptime_ms(void);
