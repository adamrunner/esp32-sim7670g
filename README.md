# esp32-sim7670g

ESP-IDF project for the **Waveshare ESP32-S3-SIM7670G-4G** dev board.
Brings the board up, talks to the SIM7670G cellular modem, and serves a
web UI for monitoring/configuring the cellular connection.

## What it does

- **Cellular data via PPP** (`main/modem.c`) — the ESP32 gets its own IP
  stack on cellular using `esp_modem` + lwIP PPP over UART1 (TX=GPIO18,
  RX=GPIO17, 115200). The modem task syncs, waits for LTE registration +
  PS attach, dials, and after that the PPP link is the ESP32's default
  route (DNS comes from IPCP negotiation). On link loss it hangs up and
  redials automatically, and it runs a one-shot DNS+ping connectivity
  check after each connect.
- **Status polling** — SIM state, registration, signal, operator, band
  every 5 s over AT. The UART carries either AT or PPP (see *CMUX* note
  below); while the data link is up the firmware briefly pauses the PPP
  stream every 30 s (`esp_modem_pause_net`: pause lwIP → `+++` → AT →
  `ATO`) to refresh status and GPS, so the web UI stays live during a
  connection at the cost of a ~2 s data stall per poll.
- **GPS** (`main/modem.c` GNSS section) — the SIM7670G's GNSS receiver is
  powered on at boot (`AT+CGNSSPWR=1`) and polled with `AT+CGNSSINFO`
  (never the NMEA stream — see *GPS* note below). Position, speed,
  course, satellites, HDOP and fix time show in the web UI with an
  OpenStreetMap link, and are served as a `gnss` object in `/api/status`.
  The GPS antenna must be connected and needs sky view; first fix after
  cold start takes a couple of minutes outdoors.
- **Status LED** (`main/led.c`) — onboard WS2812 on GPIO38, blinks as a
  heartbeat; color = modem state:
  - red: modem not responding
  - yellow: modem up, not registered yet
  - green: registered on the network
  - blue: data connection up (has an IP)
- **Internal battery gauge** (`main/board_battery.c`) — reads the onboard
  MAX17048 on the V2.0 board over I2C (SDA=GPIO15, SCL=GPIO16, address `0x36`).
  Reports the raw VBAT rail voltage and fuel-gauge state of charge in
  `/api/status` and the web UI. The USB buck regulator holds VBAT near 4.27 V,
  so firmware infers external power above 4.23 V and marks SOC invalid instead
  of presenting the resulting stuck/high percentage as real battery capacity.
  After external power is removed it applies hysteresis and waits until the
  gauge output has genuinely recovered before declaring SOC valid again; it
  deliberately avoids MAX17048 quick-start under load. Solar charging still
  appears as `battery_or_solar` because the board has no routed digital input
  that distinguishes those two cases.
- **microSD storage** (`main/sdcard.c`) — the onboard TF slot, mounted as a
  FAT filesystem at `/sdcard` for data logging. Wired to the ESP32-S3 SDMMC
  peripheral in 1-bit mode (CLK=GPIO5, CMD=GPIO4, D0=GPIO6). Mounts at boot;
  if no card is inserted it logs a warning and the rest of the app runs
  normally. Callers just `fopen("/sdcard/…")`; long filenames are enabled so
  date-stamped names like `2026-07-14.csv` aren't truncated to 8.3.
- **JBD BMS monitor** (`main/bms.c`) — polls the JBD BMS on the 4S4P LiFePO4
  house battery over UART2 (TX=GPIO1, RX=GPIO2 by default, 9600 8N1; confirm
  against the header before wiring). TX/RX are reconfigurable from the web UI
  (NVS `bmscfg/tx` and `bmscfg/rx`) with a Swap button — handy when the BMS is
  wired backwards; changing them rebuilds the UART and re-probes from scratch.
  The protocol driver is the shared `jbd_bms` component
  from `../esp32-shared-components` (also used by `esp32-bms-monitor`).
  Adaptive polling: 1 s while charging/discharging, 10 s idle, quiet 30 s
  probes while no BMS has ever answered — the firmware runs fine with nothing
  wired. A sim mode (web UI toggle, NVS `bmscfg/sim`) generates a plausible
  fridge-compressor duty cycle to exercise the telemetry pipeline end-to-end.
- **Telemetry pipeline** (`main/datalog.c`) — every BMS reading becomes a CSV
  row byte-compatible with the `esp32-bms-monitor`/`bms-dashboard` schema
  (23 fixed columns + per-cell voltages + per-temp readings). Rows fan out
  from a queue to: (a) the SD card, `/sdcard/bms/YYYY-MM-DD.csv` with daily
  rotation, header-on-create, 30 s flush and a free-space guard; and (b) MQTT.
  Rows that can't be published (no broker, coverage gap) spool to
  `/sdcard/spool/bms.csv` and replay in order — rate-limited, cursor-tracked,
  resumable across reboots — when the broker returns.
- **MQTT** (`main/mqtt.c`) — IDF esp-mqtt over plain sockets on whichever
  link is up (WiFi at home, cellular PPP otherwise; no AT/PPP contention).
  Publishes to `<base_topic>/<device_id>` (default `bms/telemetry/gw-xxxxxx`)
  at QoS 1, success counted only on PUBACK. Broker URI/credentials/topic are
  set from the web UI and persist in NVS (`mqttcfg`); `mqtts://` uses the
  bundled CA store. Unconfigured = module stays idle.
- **Time sync** (`main/timesync.c`) — SNTP once any link is up, seeded
  earlier by GNSS UTC when there's a fix, so telemetry timestamps are real
  even off-WiFi. Rows before first sync carry timestamp 0.
- **Web UI** (`main/webui.c`, `main/www/index.html`) — WiFi SoftAP
  **ESP32-SIM7670G** (password **waveshare**), then browse to
  <http://192.168.4.1/>. Shows live connection status, modem/SIM identity,
  lets you change the APN (persisted in NVS), has a raw AT-command console,
  and can perform a delayed software reboot after acknowledging the browser.

### APN: leave it blank

The default APN is **blank**, meaning the carrier assigns one. This is
deliberate: the EIOTCLUB SIM roaming on Verizon **requires a blank attach
context** — the network hands out `globaldata` itself, and writing a named
APN (e.g. `wbdata`) into PDP context 1 makes Verizon reject the LTE attach
entirely after the next modem reboot (`+CEER: EMM_CAUSE_ESM_FAILURE`).
The firmware self-heals this: if registration is denied repeatedly, it
blanks the attach context and forces a network re-scan. Only set an APN
from the web UI if your SIM genuinely needs one.

### CMUX: don't

The SIM7670G advertises CMUX (`AT+CMUX=?`), which would allow AT commands
next to the live PPP stream, but `esp_modem`'s CMUX negotiation reliably
wedges this modem into a half-CMUX state where the UART answers nothing —
only a modem reset recovers it. This firmware therefore uses plain PPP
data mode.

**If the modem stops answering AT entirely** (e.g. after reflashing while
a CMUX experiment was active): connect to one of the modem's own USB
serial ports (`/dev/cu.usbmodem00000000000xx`, the first one talks AT)
and send `AT+CRESET`.

### GPS: poll, never stream

The GNSS engine runs inside the SIM7670G independently of the cellular
stack, but its output shares the one UART. `AT+CGNSSTST=1` (the mode the
vendor demo uses) streams NMEA sentences onto that UART, which would
interleave with PPP frames and corrupt the data link — the firmware
explicitly keeps it off and polls `AT+CGNSSINFO` instead. While PPP is
up, GNSS polls ride the same paused-AT windows as status polling
(every 30 s); with PPP down they run every 5 s poll cycle.
`+CGNSSINFO` field layouts differ between SIMCom firmwares (some insert
a Galileo SV count), so the parser anchors on the N/S hemisphere field
rather than absolute positions.

## Build & flash

Requires a sibling checkout of
[`esp32-shared-components`](../esp32-shared-components) — the `jbd_bms` and
`bms_interface` components are pulled from `../../esp32-shared-components/…`
as component-manager path dependencies (see `main/idf_component.yml`).

ESP-IDF v5.5 (at `~/esp/v5.5/esp-idf`). Use the py3.10 IDF virtualenv;
it carries the stable tool versions accepted by IDF's dependency constraints:

```sh
set -gx IDF_PYTHON_ENV_PATH $HOME/.espressif/python_env/idf5.5_py3.10_env
source ~/esp/v5.5/esp-idf/export.fish
idf.py -p /dev/cu.usbmodem5B910478111 build flash
```

From bash, set `IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf5.5_py3.10_env`
and source `export.sh` instead.

The `/dev/cu.usbmodem5B9...` port is the ESP32-S3's native USB-Serial-JTAG
(logs appear there too, e.g. `idf.py -p ... monitor`). The four
`/dev/cu.usbmodem00000000000xx` ports are the SIM7670G's own USB
interfaces.

## OTA updates

The device polls
`https://adamrunner.com/downloads/esp32-sim7670g/manifest.json` 90 s
after boot and hourly after that, over whichever link is up (WiFi wins
the route when home; cellular otherwise). A version mismatch —
inequality against the running image's `git describe` version, so a
stale manifest is a downgrade order — triggers an automatic download
(resumable 128 KB range requests, sha256 read-back before anything
becomes bootable) into the passive slot and a reboot. The new image
must reach the update server over HTTPS within 6 minutes of booting or
the bootloader rolls back to the previous slot; a rolled-back version
is never auto-retried.

The OTA task waits for an active WiFi or PPP route before checking. On
cellular it suppresses the modem's periodic AT/GNSS polling from the
manifest request through the binary transfer, because entering command
mode briefly pauses PPP. Manifest discovery and image download each get
bounded retries; after a transient connection failure, firmware performs
a clean PPP redial and waits for the route to return before trying again.
Any failed cycle, including a manually requested check, retries after
5 minutes rather than waiting for the normal hourly poll. HTTP operations
allow 60 seconds so the modem can tolerate transient cellular latency.
Dynamic mbedTLS TX/RX buffers reduce idle heap residency without reducing
the standard TLS record limits.

`/api/ota` preserves structured evidence from the last failure: operation
stage, ESP-IDF/ESP-TLS/mbedTLS errors, certificate flags, socket errno,
attempt counts, the heap snapshot at failure time, and the next scheduled
check delay. The web UI summarizes that evidence and the retry countdown;
serial logs contain the complete diagnostic line.

After each MQTT connection, the device publishes a retained QoS 1 schema-v2
JSON document to `bms/status/<device_id>`. It includes `firmware_version`,
`ota_slot`, `pending_verify`, a per-boot `boot_id`, boot-scoped `status_seq`,
`status_reason`, `reset_reason`, and build metadata. Once the wall clock is
valid it also carries `reported_at` and the `sntp` or `gnss` time source; a
device that connected before synchronization publishes a distinct
`time_synchronized` event. A newly installed image republishes after its HTTPS
self-test clears `pending_verify`, so the retained record can remotely confirm
both the running version and successful rollback validation.

MQTT connection availability is published separately as retained QoS 1 JSON
on `bms/availability/<device_id>`. Each successful connection publishes
`online: true`; the broker publishes the configured `online: false` last will
if the session disappears unexpectedly. Intentional MQTT reconfiguration
publishes offline and waits for its acknowledgement before replacing the
client. This state describes the broker session, not vehicle power.

Before rebooting into an OTA target, firmware persists the source and target
versions in NVS. Successful self-test verification clears that attempt. If the
bootloader restores the prior image, its first schema-v2 status event reports
`status_reason: "rollback_detected"` with `rollback_from_version` set to the
failed image and `rollback_target_version` set to the restored image. The
marker is cleared only after the broker acknowledges that status event; a
disconnect before acknowledgement causes the evidence to be published again
after reconnection.

Publishing a release:

```sh
tools/release.sh            # build, upload .bin + manifest, verify from outside
tools/release.sh --dry-run  # show what would be published
```

The script refuses dirty versions and refuses to reuse a published
filename (Cloudflare edge-caches `.bin` files forever; `manifest.json`
is never cached). Publish after every deploy: a device left running a
newer local build than the manifest advertises will "update" backwards
on its next check. Test builds can track a staging manifest via
`idf.py -DOTA_MANIFEST_URL=... build` — but that override sticks in the
CMake cache until `idf.py fullclean`, so the script also verifies the
URL actually embedded in the binary before publishing.

## API

- `GET /api/status` — JSON snapshot of modem status, including a `gnss`
  object (`powered`, `fix`, `sats` (in view), `sats_used` (in the fix),
  `hdop`, and — once there has been a
  fix — `lat`, `lon`, `alt_m`, `speed_kmh`, `course_deg`, `utc`,
  `fix_age_s`; position persists as last-known when the fix drops)
- `POST /api/apn` — `{"apn":"..."}` save APN to NVS and reconnect
- `POST /api/at` — `{"cmd":"AT+CSQ"}` raw AT passthrough; while the PPP
  link is up this pauses the data stream for the duration of the command
  (~2 s extra latency)
- `POST /api/ping` — `{"host":"google.com"}` DNS lookup + 4-packet ICMP
  ping using the ESP32's own lwIP stack, i.e. it exercises the PPP link
  itself; returns resolved IPs, RTT stats, and a ping-style transcript.
  Blocks up to ~20 s for an unreachable host.
- `POST /api/bms` — `{"enabled":true,"sim":false,"tx_pin":1,"rx_pin":2}` (all
  optional) toggle BMS polling / simulated data and set the UART TX/RX GPIOs
  (0-48, distinct); persisted in NVS
- `GET /api/mqtt` — broker config (password never echoed, only
  `password_set`)
- `POST /api/mqtt` — partial update of
  `{"enabled":..,"uri":"mqtt(s)://..","username":..,"password":..,"base_topic":..}`;
  saves to NVS and reconnects. `/api/status` gains `bms`, `mqtt`, `datalog`
  (device id, row/spool counters, current SD file) and `time` objects
- `GET /api/ota` — running version/slot, OTA state (`idle`/`checking`/
  `downloading`/`verifying`/`wait_reboot`/`error`), progress, last
  check result, manifest/download attempt counts, `next_check_in_s`,
  structured `failure` diagnostics, and current heap diagnostics
  (`free_heap`, `largest_free_block`, `minimum_free_heap`) for
  distinguishing total-memory pressure from fragmentation after a failed
  transfer
- `POST /api/ota/check` — check for an update now; body optional:
  `{"url":"https://.../manifest.json","transport":"cell"}` to target an
  alternate manifest or pin the transfer to the cellular interface
  (both mainly for testing)
- `POST /api/reboot` — acknowledge with a one-second delay, then restart the
  ESP32 using `esp_restart()`; this does not electrically power-cycle the
  separate SIM7670G modem
- `POST /api/modem/restart` — asynchronously tear down PPP, issue
  `AT+CRESET`, recover the modem at its reset UART baud, re-enable GNSS, and
  let the modem supervisor restore LTE/PPP. Progress and terminal errors are
  reported by the `modem_restart` object in `GET /api/status`. The request is
  rejected while OTA is actively checking or installing.

## Deferred cleanups

- **Factor out the NVS get/set boilerplate.** Every config module
  (`bms.c`, `mqtt.c`, `datalog.c`, `modem.c`, `ota.c`, `wifi.c`) hand-rolls
  the same `nvs_open` → read-with-defaults and `nvs_open` →
  set → `nvs_commit` → `nvs_close` dance, plus the "namespace missing = use
  defaults" fallback. A couple of thin helpers would remove ~40 lines and put
  that fallback behavior in one place, e.g.:
  - `nvs_get_str_default(ns, key, out, len, default)` and matching
    `_u8`/`_i32` readers that swallow "not found" and return the default;
  - a small scoped-write helper (open READWRITE, run a callback of
    `nvs_set_*` calls, commit, close) so call sites can't forget the commit.

  Deferred because it's low-urgency (idiomatic ESP-IDF as-is) and touches
  `modem.c`/`ota.c`/`wifi.c` — stable modules outside recent feature work —
  so the churn/risk isn't worth it until a new config module lands. There's
  no shared util component yet; a new one (or a `nvsutil.[ch]` in `main/`)
  would be the home for these. Note the NVS namespace macro is `NVS_NS` in
  `bms.c`/`mqtt.c`/`datalog.c` but `NVS_NAMESPACE` in `modem.c`/`ota.c`/
  `wifi.c`; standardize on one name in the same pass.
