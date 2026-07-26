# Supervised Modem Restart Plan

## Goal

Add a web action that restarts the SIM7670G without rebooting the ESP32, then
lets the existing modem supervisor restore its UART, GNSS, registration, and
PPP state. This complements the ESP32 reboot control: it targets modem-side
failures while preserving the host and local WiFi UI.

## Ownership and API

- `POST /api/modem/restart` only requests the operation and responds
  immediately. The HTTP server never owns the modem UART or PPP transitions.
- The modem task consumes the request and performs the complete sequence while
  continuing to serialize all mode changes through the existing AT mutex.
- `/api/status` exposes a `modem_restart` object with the state, request count,
  elapsed time, and any terminal error so a browser can distinguish an
  accepted request from a recovered modem.
- Reject duplicate requests while a restart is active.
- Reject modem restarts while OTA is checking, downloading, verifying, or
  waiting to boot. Deliberately interrupting those phases would turn a
  maintenance action into an avoidable update failure.

## Restart Sequence

1. Mark the request `resetting`.
2. If PPP is active, return the modem to command mode and mark the cellular
   route down.
3. Send `AT+CRESET` directly in command mode. Do not use the ordinary raw-AT
   helper because it would try to resume the old PPP session with `ATO`.
4. Treat the resulting loss of AT response and PPP as expected. Clear live
   registration/GNSS state while retaining the last-known GNSS position.
5. Mark the request `waiting_at` and let the existing baud negotiation probe
   both 460800 and the modem's reset default of 115200.
6. Once AT responds again, mark the action `complete`; the normal supervisor
   re-enables GNSS, waits for stable registration and packet attachment, and
   redials PPP.
7. If `AT+CRESET` is rejected or the modem does not return within a bounded
   window, expose `error`. Normal background recovery continues even after
   that diagnostic timeout.

## UI

- Put a destructive `Restart modem` control in the Modem / SIM card, separate
  from the ESP32 firmware reboot control.
- Confirm that cellular, MQTT, and GNSS will disconnect temporarily while
  local WiFi remains available.
- Disable the button while the operation is active and render progress from
  `modem_restart.state`.

## Validation

1. Build with the pinned ESP-IDF 5.5 environment.
2. Confirm the embedded web asset and URI-handler count fit the existing
   server configuration.
3. Review all error paths for mutex release and continued background recovery.
4. On hardware, trigger the action over WiFi and verify:
   - the HTTP request is acknowledged before PPP drops;
   - the modem returns at its reset baud and is raised to 460800;
   - GNSS is powered again;
   - LTE registration and PPP recover;
   - MQTT reconnects;
   - `/api/status` records the completed restart.

An AT command cannot recover a modem that is too wedged to accept UART input.
That case still requires the board's hardware reset/power path.
