# Low-Power Operation Plan

Status: draft — Phase 0 (measurement) not started

## Purpose

The gateway runs from the 4S LiFePO4 house battery it monitors, and it never
stops drawing power. On a parked truck with the fridge off, the gateway is the
only load, and it can run the pack flat on its own. This plan adds a
battery-aware power policy: when the house battery is low, the firmware keeps
sampling the BMS but turns the cellular radio off between scheduled upload
windows. The rows it collects in between go to the existing SD spool and are
replayed during each window.

A parallel hardware track, moving the gateway onto its own 1S Li-ion pack, is
covered at the end because it changes what the policy has to protect.

## Field Evidence Baseline

Measured 2026-09-28 from Anton production telemetry (`gw-e3aba4`, Jul 25 –
Sep 28). The analysis scripts are not checked in; the method is described here
so it can be repeated.

- **Unmeasured continuous load: 4.9 Ah/day ≈ 0.20 A at ~13.2 V ≈ 2.6 W.**
  Between true full charges (pack ≥14.4 V with <1.5 A tail current), measured
  charge-in exceeded measured discharge by 4.7–5.6 Ah/day on every interval
  without telemetry gaps. The JBD reports 0.0 A at rest, so this load is below
  its current-sense deadband and absent from its coulomb-counted SOC. It is
  attributed to this gateway and its 12V→USB supply.
- **The BMS SOC reads high, and the error grows by ~5 points/day** until the next
  full charge resynchronizes it. Any SOC threshold in this plan therefore
  cannot use the raw BMS SOC (see *Effective SOC*).
- **2026-08-13 over-discharge.** The BMS showed 74–79% while the pack was
  actually empty. After the BMS disabled discharge (2.29 V/cell), cell voltage
  kept falling to **1.90 V/cell** over ~6 h, with the gateway still drawing
  current. The gateway feed is upstream of the BMS disconnect. Adam
  is checking the wiring separately; until that is fixed, firmware is the only
  thing that can cut this load.
- Reference loads: fridge compressor ~3.8 A when running, averaging
  ≈ 1.0 × (outdoor mean °C) − 3.5 Ah/day. The DC-DC charger delivers ~30 A while driving.

## Objectives

1. Cut the gateway's average draw substantially when the house battery is
   below 50% effective SOC, without losing any BMS samples.
2. Never let the gateway's own draw take the pack below a safe floor.
3. Resume full live telemetry within about a minute of the truck starting to
   charge (driving).
4. Keep the dashboard honest: a device that is sleeping on schedule must not
   raise `telemetry_stale`. A device that misses its own announced check-in
   must still raise it.

## Constraints

- **The modem supervisor in `modem.c` is the only owner of modem mode
  changes.** The Phase 2 AT-window regression (FIELD_RELIABILITY_REMEDIATION_PLAN,
  2026-07-28) is the precedent: the power policy *requests* radio states, and
  the supervisor carries them out under the existing AT mutex. No second code
  path sends `+++`/`ATO`/`CFUN`.
- No CMUX. No NMEA streaming (see README).
- The spool (`datalog.c`) is the delivery mechanism for batched uploads. It
  replays at ~4 msg/s (`SPOOL_REPLAY_PER_TICK` 2 per ~500 ms tick). Thirty
  minutes of 10 s rows is 180 rows, about 45 s of replay.
- Delivery duplicates (~5%, FIELD plan Phase 4) will now show up at every
  upload window, not only after coverage gaps. Delivery identity becomes more
  valuable but is not a prerequisite.
- The backend flags `telemetry_stale` after 900 s
  (`TELEMETRY_STALE_AFTER_SECONDS`). Every eco interval exceeds that.

## Design

### Effective SOC

The policy's battery input is the **most conservative** of:

1. **BMS SOC.**
2. **Drift-corrected SOC**, which is BMS SOC − `drift_pct_per_hour` × hours
   since the last true-full event. A true-full event is pack ≥14.4 V with
   0 < current < 1.5 A. The timestamp persists in NVS so reboots don't reset it.
   `drift_pct_per_hour` defaults to 0.20 (≈0.2 A on a 100 Ah pack) and is set
   from NVS/web UI. It goes to 0 once the gateway is off the house battery or
   is wired downstream of the BMS shunt.
3. **Voltage floor.** A resting minimum cell (|current| < 0.5 A for ≥10 min)
   at ≤3.20 V forces effective SOC ≤ 20%, and ≤3.10 V forces it ≤ 10%. These
   anchors hold even if the BMS resynchronizes wrongly.

The policy also reports its inputs in `/api/status`, so a wrong tier can be
traced back to whichever input caused it.

### Power tiers

| Tier | Enter below (effective SOC) | Radio | Upload window every | Notes |
|---|---|---|---|---|
| `normal` | — | always on | live | current behavior |
| `eco_10` | 50% | off between windows | 10 min | |
| `eco_20` | 35% | off between windows | 20 min | |
| `eco_30` | 20% | off between windows | 30 min | |
| `critical` | 10%, or resting min cell ≤3.05 V | off | 6 h (proposed) | GNSS off, WiFi off, LED off |

- **Hysteresis.** A tier is left only when effective SOC is 5 points above its
  entry threshold.
- **Charging override.** Current ≥ +2.0 A sustained for 60 s means the truck is
  charging (driving), and forces `normal` immediately regardless of SOC. The
  +2 A threshold is above the small +0.5–2.5 A readings seen at rest.
- **Evaluation.** The policy is evaluated on every BMS poll. Tier changes are
  journaled as `power/tier_changed` with the inputs.

The logic lives in a pure `power_policy_core.c` (inputs → tier plus next wake).
It follows the `ota_policy_core.c` pattern so that `tests/` can cover it on the
host. `power_policy.c` gathers the inputs and hands the resulting requests to
the modem, WiFi, OTA, and LED modules.

### Board hardware (Waveshare ESP32-S3-A-SIM7670X-4G V2.0 schematic)

Source: `files.waveshare.com/wiki/ESP32-S3-A-SIM7670X-4G-HAT/ESP32-S3-A-SIM7670X-4G-V2.pdf`,
linked from the Resources page of docs.waveshare.com/ESP32-S3-SIM7670G-4G.
It was read 2026-09-28 together with the SIMCom SIM7672X Hardware Design
V1.01 and the Sleep Mode Application Note V1.00.

- **Modem power switch: GPIO21.** GPIO21 (via R34) and the net `BAT_SET` (via
  R32) are wired together onto the gate of M2A. M2A is an N-channel FET that
  drives M2B, a P-channel FET, which switches the board's `VBAT` rail onto
  `VVBAT`. `VVBAT` is the modem's entire supply (A7670 VBAT pins 55–57). R33
  (100K) pulls the gate low, so the modem is **unpowered by default**.
- **DIP switch SW2 position 3 ("4G") ties `BAT_SET` to 3V3.** With it ON, the
  modem is powered permanently and GPIO21 has no effect. That is the current
  field configuration, since this firmware never drives GPIO21. With it OFF,
  R63 (47K) pulls `BAT_SET` low and **GPIO21 alone controls modem power**. The
  vendor GNSS demo drives GPIO21 high at setup for exactly this case.
- **PWRKEY is not wired to the ESP32.** Q9 (MMBT3904) holds POWERKEY low
  whenever `VVBAT` is present (4.7K/47K divider off `VVBAT`), so **the modem
  boots automatically as soon as it gets power**. `AT+CPOF` can therefore
  only be undone by cycling GPIO21.
- **Modem RESET (pin 16) is not wired to the ESP32.** (The `RESET` net at
  R76/C87 belongs to the camera.)
- **DTR → GPIO45, RI → GPIO40**, through a TXB0104 auto-direction level
  shifter (U14). U14's A side and OE are powered from `VCC_1V8`, which is the
  modem's own VDD_EXT, so the shifter goes high-impedance whenever the modem
  is unpowered. GPIO45 is an ESP32-S3 strapping pin (VDD_SPI voltage). It has
  worked so far; any change that makes the modem drive DTR during ESP reset
  must be bench-verified to boot.
- **The modem's USB_VBUS (pin 24) is tied to USB-C VBUS** (`UVBUS` via R88,
  0Ω). SIMCom requires USB_VBUS low for both DTR sleep (`AT+CSCLK`) and the
  low-current minimum-functionality mode. **While the board is powered over
  USB-C, as it is today, the modem cannot reach its sleep currents**
  (0.7–1.5 mA). `CFUN=0`/`4` still turn off the RF, but the module core stays
  awake. Removing R88 lifts that restriction, but costs the modem's USB port
  (which firmware updates can need).
- **GNSS backup domain (`GNSS_VBKP`, pin 116) is powered from the ESP's
  VCC3V3**, so ephemeris and RTC survive modem power cycles. Expect warm or hot
  starts after a GPIO21 cycle rather than cold starts. `GNSS_PWRCTL` goes to
  the modem's own MK_IN_3, not to the ESP32.

### Turning the radio off

| Mechanism | Modem current between windows | Wake cost | Notes |
|---|---|---|---|
| **GPIO21 power cut** (DIP 3 OFF) | ≈0 (FET leakage) | modem boot + registration + PPP, est. 30–90 s; UART back at 115200 (the supervisor already probes it) | **preferred**: needs no USB_VBUS change and has the fewest modem states |
| `AT+CFUN=0` / `4` | RF off; module awake (unmeasured, tens of mA plausible while USB-powered) | registration + PPP only | software-only, works with DIP 3 ON; use it as Phase 2a |
| `AT+CSCLK=1` + DTR high (GPIO45) | 0.7–1.5 mA per datasheet | UART wake, stays registered | **blocked while USB-powered** (USB_VBUS high); also forfeits the upload-window model because the modem stays attached |
| `AT+CPOF` | ≈0 | recoverable only by a GPIO21 cycle | no advantage over the GPIO21 cut |

Add a requested radio state (`MODEM_RADIO_ON` / `MODEM_RADIO_OFF`) to the
modem supervisor. While OFF, the supervisor:
- hangs up PPP and powers the modem down. With the GPIO21 cut, it first sends
  `AT+CPOF` so the modem shuts down its filesystem cleanly, waits for the
  shutdown, then drives GPIO21 low. With `CFUN` only, it sends `CFUN=0` plus
  GNSS off;
- stops the 30 s AT/GNSS polls and the redial loop;
- reports `radio: "off_requested"` instead of counting failures.

The existing recovery logic must not treat a requested-off radio as a fault or
restart the modem. On ON, the supervisor drives GPIO21 high and goes through
its existing cold-start path (baud probe, GNSS enable, registration, dial).

**GPIO21 rollout hazard.** Once DIP 3 is OFF, *any* image that does not drive
GPIO21 high, including an OTA rollback target, boots with no modem. The truck's
parking spot is out of home-WiFi range (as of 2026-09-28), so cellular is the
*only* OTA path even at home, and recovery means a trip to the truck: flip
DIP 3 back ON, which powers the modem whatever the image does, or reflash over
USB-C, which needs DIP 2 "HUB" ON. So the rollout goes in this order:
1. Ship a release whose only modem change is driving GPIO21 high before
   `modem_init()`. Let it soak with DIP 3 still ON; the pin has no effect
   then.
2. Flip DIP 3 OFF only when the running image *and* the OTA rollback slot
   both carry that change, and while at the truck to confirm the modem comes
   back up (the web UI's modem status, or the dashboard receiving telemetry).
3. Then enable the power-cut behavior.

Step 1 landed as `modem_power_enable()` in `modem.c`. It sets GPIO21 high
before the pin becomes an output, then takes a pad hold (`gpio_hold_en`). The
hold survives software, watchdog, panic and OTA resets, and is cleared only by
a power-down. So with DIP 3 OFF the modem is *not* power-cycled on ESP32
reboots. An older image booted by an OTA rollback after a soft reset still
finds the modem powered. Only a full power loss followed by booting an image
without step 1 leaves the modem off. The eco power cut in Phase 2 must drive
the pin as a high output before `gpio_hold_dis()`. Otherwise R33 drops the
modem's power at the release. `test_field_safety_contracts.py` guards the
ordering.

### Upload window

On each scheduled wake:

1. Radio ON: CFUN=1, wait for registration, packet-service attach, and PPP.
   The wait is bounded (proposed 180 s).
2. MQTT connects. The client publishes retained availability `online` with
   `mode` and `tier`.
3. The spool drains to empty, or until a bounded window (proposed 120 s) ends,
   with a checkpointed cursor as today.
4. One GNSS fix is taken if it arrives within the window. Otherwise the
   window proceeds without one.
5. The client publishes retained availability
   `{"online": false, "reason": "sleep", "tier": "eco_20", "next_checkin_s": 1200}`
   and then disconnects **cleanly**. A clean disconnect suppresses the LWT, so
   the broker never reports the sleep as an unexpected drop.
6. Radio OFF.

If the window fails (no registration, no broker), the rows stay spooled and the
next window retries them. Failed windows are counted. After 3 consecutive
failed windows, one full modem restart is attempted at the next window rather
than immediately. A failed window must never keep the radio on indefinitely.

### Other loads

- **WiFi.** There is no internet-sharing hotspot; that was never built. But
  `wifi.c` does fall back to the local *configuration* SoftAP
  ("ESP32-SIM7670G", 192.168.4.1) whenever home WiFi is not stored or not
  reachable, and it stays up indefinitely. Away from home, the WiFi radio is
  therefore always on just to serve the web UI. The truck's usual parking
  spot is out of home-WiFi range, so in practice the SoftAP is up nearly all
  the time. (A repeater or WiFi HaLow link at home is a possible future
  change, out of scope here.) In eco tiers the SoftAP is disabled. Local access
  becomes available for 10 minutes after boot or after a charging override. A
  way to wake it without cellular (BOOT button or similar) is an open question.
  At home, STA uses `esp_wifi_set_ps(WIFI_PS_MAX_MODEM)`.
- **OTA.** Routine hourly checks are suspended in eco tiers. A pending OTA
  verification window blocks entry into eco until it completes.
- **LED.** Off in eco tiers, except one blink per upload window.
- **BMS sampling.** Stays at 10 s so the SD record keeps full resolution. If
  replay time or SD wear matters later, eco tiers can drop to 30 s.
- **CPU.** Automatic light sleep (`CONFIG_PM_ENABLE`, tickless idle) is
  deferred to a later phase. UART RX wakeups for both the modem and the BMS
  make it the riskiest change, and it is only worth doing if Phase 0 shows the
  CPU is a meaningful share of the budget.

### Backend changes (bms-dashboard-server)

- Accept the optional availability fields `reason`, `tier`, and
  `next_checkin_s` (schema-v1 stays compatible; the new fields are optional).
- `evaluate_telemetry_staleness()` uses a per-device threshold of
  `max(900, next_checkin_s + 300)` while the latest availability says
  `reason: sleep`. A missed check-in still alerts.
- The dashboard shows "Sleeping (low battery) — next check-in 14:30" instead
  of "offline".
- The dashboard can also chart the gateway's own effective SOC, using the
  corrected figure rather than the raw BMS SOC.

## Phased Execution Plan

### Phase 0: Measure where the 2.6 W goes

Adam's hypothesis is that the modem dominates. Measure it before building
around it. With a USB power meter inline on the gateway's 5 V feed, record the
average over ≥5 min in each state:

1. Current firmware, LTE connected, SoftAP on (baseline)
2. SoftAP off
3. GNSS off (`AT+CGNSSPWR=0` from the web console)
4. Radio off (`AT+CFUN=0`)
5. Modem unpowered (DIP 3 OFF with GPIO21 low, bench only). This state is the
   ESP32-only floor.
6. All of the above together

Also measure the 12 V side at the same time, to get the 12V→USB converter's
efficiency and its no-load draw.

**Exit:** a table of watts per state, checked into this doc. That table sets
the real savings per tier and decides whether CPOF/PWRKEY or light sleep are
worth pursuing.

### Phase 1: Policy core and observability (no behavior change)

- `power_policy_core.c` with effective SOC, tiers, hysteresis, and the charging
  override, plus host tests including the Aug 13 trace as a fixture.
- True-full tracking persisted in NVS.
- `/api/status` `power` object and journal events. The policy runs in
  **dry-run** mode: it computes and reports the tier but actuates nothing.
- Extend BMS sim mode with a scripted SOC ramp. The existing 87%→20% sawtooth
  already crosses every eco threshold.

**Exit:** a week of dry-run field data where the tier the policy picks matches
what the pack is actually doing.

### Phase 2: Radio-off and upload windows

- Supervisor radio request state, CFUN/GNSS actuation, and the upload-window
  sequence, with a clean disconnect and sleep availability.
- Bench test on sim mode, forcing each tier from the web UI.

**Exit:** 48 h on the bench in `eco_10` with zero lost rows (SD count equals
database distinct timestamps). The radio must also come back on every window.

### Phase 3: Backend sleep awareness

The staleness threshold and availability fields above, plus the dashboard
display. This ships **before** Phase 2 reaches the truck so there is no alert
storm.

### Phase 4: WiFi, OTA, and LED policy

### Phase 5: Field rollout

Enable on the truck with eco thresholds armed. Validate on a real parked
period: the drift-corrected SOC matches a later true-full resync to within 5
points, and the measured drain drops by the amount Phase 0 predicted.

## Hardware Track: Dedicated 1S Pack for the Gateway

Proposal: run the board from a 1S10P 18650 pack on its battery input (the V2.0
board's MAX17048 already gauges it). A USB charger on the ignition-switched
12 V circuit charges the pack while driving. The house battery then no longer
feeds the gateway at all.

Things to settle before building it:

- **Runtime.** 10 × ~3.0 Ah ≈ 30 Ah × 3.6 V ≈ 108 Wh. At today's draw
  (≈2–2.5 W at the board, minus the 12 V converter loss) that is only
  **~2 days**. It becomes useful only together with the eco tiers. At a
  hypothetical 0.3–0.5 W eco average it is 9–15 days, which Phase 0 will
  confirm or correct.
- **The board's own charge paths (from the schematic).** There are two:
  - **USB-C.** U8 (SY8105) steps USB 5 V down to `VBAT` = 0.6 × (1 +
    110K/18K) ≈ **4.27 V**. The cell sits directly on that rail (through the
    SW1 power switch), so USB "charges" it with only the buck's current limit
    and a float slightly above 4.2 V. There is no charge termination and no
    temperature input. That is tolerable for one 18650. For a 30 Ah pack it
    means a large, uncontrolled inrush from a deeply discharged pack, and a
    permanent 4.27 V float.
  - **SOLAR_IN (5–18 V).** U1 (CN3791) is a proper CC/CV charger to 4.2 V, set
    to **1.2 A** by R1 (0.1 Ω, 0.12 V sense). Its MPPT set resistors (R78–R84)
    select 5/9/12/18 V. An ignition-switched 12 V feed could drive this input
    directly, with the MPPT set to 12 V, and replace the 12V→USB supply
    entirely. That also keeps USB_VBUS low, which unblocks modem sleep (see
    *Board hardware*). But 1.2 A is slow for 30 Ah: a 2–3 h drive returns
    2.4–3.6 Ah, about 8–12% of the pack. Vehicle 12 V also carries transients
    above 18 V, so this input needs a TVS or protected supply ahead of it.
  - A dedicated 3–5 A 1S charger with its own temperature cutoff, feeding the
    pack rather than the board, would fill it far faster than either path.
- **Pack protection.** The board has S-8261 + FS8205 single-cell protection
  on the battery negative. Keep it, but it is sized for one cell's currents
  and is not a substitute for per-pack fusing.
- **Pack safety.** Use matched cells from a single batch, a fuse per pack, and
  a 1S protection board. Do not charge below 0 °C: overnight lows in the high
  desert in October can freeze, so the charger needs a temperature cutoff or
  the pack needs to stay in the cab.
- **Firmware impact.** With the gateway off the house battery,
  `drift_pct_per_hour` goes to 0. The policy should then also consider the
  gateway's own pack, taking the most conservative of house-battery and
  gateway-battery effective SOC. `board_battery.c` already detects the
  "external power present" rail (>4.23 V), which becomes a second, direct
  signal for ignition on.

**Simpler interim alternative.** Put a low-voltage disconnect (12.4–12.6 V)
on the gateway's 12 V feed, or feed it from the BMS-switched side. Either one
removes the over-discharge risk regardless of firmware.

## Open Questions

1. ~~Does the board route PWRKEY or DTR to a GPIO?~~ Answered 2026-09-28 (see
   *Board hardware*): GPIO21 switches modem power when DIP 3 is OFF, DTR is
   GPIO45, RI is GPIO40, and PWRKEY/RESET are not routed. Still open: confirm
   the physical DIP 3 position on the installed board.
2. Is the `critical` tier's 6 h check-in right, or should the radio stay off
   entirely until charging is seen?
3. How should local WiFi access be woken while in eco (BOOT button, timed
   window, or never)?
4. Should the eco thresholds be configurable from the web UI, or fixed in
   firmware?
