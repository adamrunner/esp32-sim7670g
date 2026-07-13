# esp32-sim7670g

ESP-IDF project for the **Waveshare ESP32-S3-SIM7670G-4G** dev board.
Brings the board up, talks to the SIM7670G cellular modem, and serves a
web UI for monitoring/configuring the cellular connection.

## What it does

- **Modem driver** (`main/modem.c`) — AT commands to the SIM7670G over
  UART1 (TX=GPIO18, RX=GPIO17, 115200). Polls SIM state, registration,
  signal, operator, band, and PDP/IP every 5 s; activates the PDP context
  with the configured APN once registered.
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

The default APN is `wbdata` (EIOTCLUB); change it from the web UI if
your SIM needs something else.

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

- `GET /api/status` — JSON snapshot of modem status
- `POST /api/apn` — `{"apn":"..."}` save APN to NVS and reconnect
- `POST /api/at` — `{"cmd":"AT+CSQ"}` raw AT passthrough
- `POST /api/ping` — `{"host":"google.com"}` DNS lookup (`AT+CDNSGIP`) +
  4-packet ping (`AT+CPING`) over the cellular connection; returns resolved
  IPs, RTT stats, and the raw modem output. Blocks up to ~1 min for an
  unreachable host (modem's minimum per-packet timeout is 10 s).
