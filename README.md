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
- **microSD storage** (`main/sdcard.c`) — the onboard TF slot, mounted as a
  FAT filesystem at `/sdcard` for data logging. Wired to the ESP32-S3 SDMMC
  peripheral in 1-bit mode (CLK=GPIO5, CMD=GPIO4, D0=GPIO6). Mounts at boot;
  if no card is inserted it logs a warning and the rest of the app runs
  normally. Callers just `fopen("/sdcard/…")`; long filenames are enabled so
  date-stamped names like `2026-07-14.csv` aren't truncated to 8.3.
- **JBD BMS monitor** (`main/bms.c`) — polls the JBD BMS on the 4S4P LiFePO4
  house battery over UART2 (TX=GPIO1, RX=GPIO2, 9600 8N1; confirm against the
  header before wiring). The protocol driver is the shared `jbd_bms` component
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
  lets you change the APN (persisted in NVS), and has a raw AT-command
  console.

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

ESP-IDF v5.5 (at `~/esp/v5.5/esp-idf`). Fish config pins
`IDF_PYTHON_ENV_PATH` to the py3.13 virtualenv and defines `get_idf`
(PATH's `python3` is PlatformIO's 3.11, which has no IDF env):

```sh
get_idf   # fish: sources ~/esp/v5.5/esp-idf/export.fish
idf.py -p /dev/cu.usbmodem5B910478111 build flash
```

From bash, set `IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf5.5_py3.13_env`
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
- `POST /api/bms` — `{"enabled":true,"sim":false}` (both optional) toggle
  BMS polling / simulated data; persisted in NVS
- `GET /api/mqtt` — broker config (password never echoed, only
  `password_set`)
- `POST /api/mqtt` — partial update of
  `{"enabled":..,"uri":"mqtt(s)://..","username":..,"password":..,"base_topic":..}`;
  saves to NVS and reconnects. `/api/status` gains `bms`, `mqtt`, `datalog`
  (device id, row/spool counters, current SD file) and `time` objects
- `GET /api/ota` — running version/slot, OTA state (`idle`/`checking`/
  `downloading`/`verifying`/`wait_reboot`/`error`), progress, last
  check result
- `POST /api/ota/check` — check for an update now; body optional:
  `{"url":"https://.../manifest.json","transport":"cell"}` to target an
  alternate manifest or pin the transfer to the cellular interface
  (both mainly for testing)
