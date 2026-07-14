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
