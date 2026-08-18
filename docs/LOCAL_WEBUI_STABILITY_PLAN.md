# Local WebUI Stability Plan

Status: in progress — Phase A complete and accepted on field evidence
(2026-08-17). Phase B: PSRAM plus mbedTLS-in-PSRAM (`646ead9`) now clears
every heap criterion including the OTA/TLS one that `b062302` failed
(2026-08-17). `646ead9` is measured but **not published**, so the field
device still runs `b062302`; the BMS/SD field re-check also remains open.

Evidence date: 2026-08-08 (diagnosis), 2026-08-17 (Phase A field results)

Scope: the local control plane only — the SoftAP, the embedded HTTP server,
and the browser dashboard — when an operator is connected directly to the
ESP32 at `http://192.168.4.1/`. Cellular recovery policy, MQTT delivery
identity, and OTA transport remain governed by
`FIELD_RELIABILITY_REMEDIATION_PLAN.md`.

Operator evidence, 2026-08-08: instability is observed **without** any use of
the AT console or ping card (the operator deliberately avoids both because of
their blocking architecture). The blocking-handler problem is therefore real
but is *not* the root cause of the currently observed instability; the
diagnosis and phase ordering below reflect that. The module is confirmed to
have 8 MB PSRAM, currently disabled in `sdkconfig`.

## What has already been fixed (history)

Chronological record of every local-control fix to date, so this plan builds
on evidence instead of re-trying old ideas:

1. **Phase 1 observability (`02bcc6b`)** — HTTP request/failure counters,
   min-heap/min-stack tracking, the event journal, and visible browser error
   banners instead of silently swallowed fetch failures.
2. **Phase 2 regression and rollback (`7736d39` → `063075c`)** — the dry-run
   supervisor era exposed two local-control failures: a modem-task stack
   overflow (panic loop) and `/api/status` returning 500 when
   `cJSON_PrintUnformatted()` could not allocate a response-sized buffer
   under AP+BMS+SD load. Rollback restored Phase 1.
3. **Passive OTA + control-plane deference (`6cf9a16`)** — the OTA retry
   helper had been calling `modem_request_redial()` and destabilizing
   transport while an operator was connected. OTA retries became passive, and
   routine checks now defer while a SoftAP client is associated, for five
   minutes after disassociation, and for one minute after any WebUI request.
4. **Bounded JSON serialization (`45b49ac`)** — first chunked-output attempt
   still built the whole cJSON event tree and drove min free heap to 316
   bytes; the corrected version streams `/api/status` one module fragment at
   a time and `/api/events` one event at a time in fixed 512-byte chunks.
   Verified: 71 requests, zero failures, min heap 23 KB.
5. **Local-only SoftAP (`8ff3076`)** — DHCP router and DNS offers disabled so
   a phone keeps its own internet route while the 192.168.4.0/24 route still
   reaches the UI. Field-verified with an iPhone.
6. **Socket-pressure correction (`4d9759d`)** — an accidental WebUI OTA check
   hit `ENFILE` (errno 23) with all ten lwIP sockets consumed: httpd allowed
   seven client sessions plus three internal sockets. Fix: four client
   sessions with LRU purge, sequential dashboard polling with a 2.5 s abort
   deadline and in-flight guards, OTA button disabled during SoftAP
   sessions. Field SoftAP acceptance passed after this.

## Why the UI is still unstable (current limitations)

Ranked by how much of the *currently observed* instability each mechanism
plausibly explains, given that AT/ping are not in use.

### L1. Session/socket fragility on the single-task server

The most likely root-cause cluster for what the operator is seeing now:

- **Slow or dead client sends stall everything.** `esp_http_server` runs
  one task; a response chunk to a client in bad RF, or to a session the
  browser already abandoned, blocks in `send()` for up to
  `send_wait_timeout` (default **5 s**) per chunk before failing. During
  that stall every other poll queues, blows the browser's 2.5 s deadline,
  and aborts — manufacturing failures on otherwise healthy requests. Phones
  routinely produce exactly these conditions: screen lock mid-poll, WiFi
  power-save latency, walking to the edge of RF range.
- **LRU purge can close the live connection.** Four client sessions is
  fewer than one iOS Safari instance will open (up to six parallel
  connections); when the table fills, LRU purge may evict the socket the
  active poll loop is using, which surfaces as random resets/failed
  fetches.
- **No TCP keepalive on sessions.** A phone that disassociates without
  closing leaves its sockets held until LRU pressure reclaims them,
  keeping the table near-full and purge-happy.
- **Client aborts don't free the server promptly.** The 2.5 s fetch abort
  closes the client side only; the handler keeps streaming until a send
  fails, tying up the task and a socket for seconds more.

### L2. Thin internal-RAM margin, PSRAM unused

Historical minimums on record: 840 bytes free heap, 224-byte largest block,
16-byte OTA-path minimum, and TLS failures at 1,920–2,944 contiguous bytes.
The streaming serializer removed the biggest single consumer, but every
request still allocates cJSON fragments, and any background TLS (an OTA
check firing after a brief client disassociation ends the quiet period, or
`mqtts://`) squeezes internal RAM while the browser polls. The module has
**8 MB PSRAM sitting disabled** — the single cheapest source of headroom in
the whole system, and it also de-risks every "spend a little RAM" fix
(more sockets, bigger session budget) that is currently rationed.

One concrete collision worth instrumenting for: an iPhone screen-lock can
disassociate the AP client; five minutes later the OTA quiet period expires
and a TLS manifest check runs; the operator wakes the phone and resumes
polling mid-TLS. That reproduces "instability with nobody touching AT/ping."

### L3. SoftAP availability gaps (no APSTA)

The WiFi supervisor (`wifi.c`) runs STA and AP as mutually exclusive modes.
With home credentials stored:

- at boot, the AP appears only after five failed STA attempts with
  backoff — up to **~90 s** of no AP;
- after a live STA drop, ten attempts with backoff up to 30 s — potentially
  **several minutes** of no AP.

Phase 2's field notes recorded exactly this: "the field SoftAP was usually
unavailable because the existing WiFi state machine disables it while
retrying the stored STA network." An operator arriving during this window
sees no network to join; one who was connected sees the AP vanish. Reads as
"the WebUI is flaky" even when HTTP itself is healthy. (Once fallen back,
the AP is stable until reboot or reconfig — so this explains
session-*start* pain more than mid-session drops.)

### L4. Polling cost and rigidity

Each 2 s cycle issues three sequential requests (`/api/status`, `/api/wifi`,
`/api/ota`) — ~90 requests/minute per open tab, each rebuilding cJSON
fragments server-side. There is no failure backoff (errors retry at the same
cadence, piling onto whatever caused the failure) and no visibility pause.
The 2.5 s deadline is tight against real handler latency, so
borderline-slow responses get aborted and retried instead of arriving late.
More requests = more exposure to every L1 mechanism.

### L5. Blocking action handlers (AT/ping)

`/api/at` blocks the HTTP task up to 10 s (`webui.c:627`) and `/api/ping`
~20+ s (`webui.c:655`), both also pausing PPP. Architecturally wrong and the
reason the operator avoids these features — but per the operator evidence
above, **not** the driver of current instability. Fixing it restores two
useful diagnostic features rather than fixing the dashboard.

### L6. No HTTP server lifecycle management

If the httpd task wedges, nothing restarts it; only a reboot recovers. The
remediation plan lists a web-service watchdog as a Phase 3 deliverable; it
does not exist yet.

## Plan

Ordering reflects the re-ranked diagnosis: evidence first, then heap
headroom (PSRAM), then the session/socket layer, then polling, then AP
availability, and only then the blocking-handler rework. Each phase is
independently shippable and gated like prior work: host tests + JS syntax
check + clean pinned build, then explicit flash approval, then explicit OTA
publication approval. Automatic supervisor redial and modem-reset escalation
remain disabled throughout; none of this plan touches that boundary.

### Phase A: Instrument the HTTP/socket layer and define acceptance

Cheap additions so later phases are judged on evidence, not impressions —
and specifically to catch L1/L2 mechanisms in the act.

Deliverables:

- Register an `esp_http_server` event handler for connect/disconnect/error
  events; count sessions opened/closed, LRU purges, and accept errors
  (with errno) in `/api/status.http`, and journal them rate-limited.
- Track a concurrent-session high-water mark, per-request duration
  buckets, and time-blocked-in-send so a slow-client stall is directly
  visible rather than inferred.
- Journal a distinct event when an OTA check starts while any HTTP request
  occurred in the preceding 60 s, to confirm or rule out the
  quiet-period/TLS collision hypothesis.
- Lower `send_wait_timeout`/`recv_wait_timeout` from 5 s to ~2 s so dead
  sessions release the task sooner (config-only; measure, don't assume).
- A bench soak script under `tools/` (curl/Python against the device) that
  runs a 30-minute SoftAP-profile load — 2 s polling plus abrupt client
  disconnects and an RF-degraded client simulation (bandwidth-throttled
  peer) — reporting failure/latency percentiles from `/api/status.http`
  plus client-side observations.

Acceptance definition (used by every later phase):

- 30-minute SoftAP session with one phone and one laptop polling:
  ≥ 99% of polls succeed, p95 poll latency < 1 s, zero error banners,
  zero `ENFILE`, min free internal heap > 15 KB.

#### Phase A implementation record — 2026-08-08

- `main/webui.c` instrumentation, all additive to `/api/status.http`:
  - session lifecycle via httpd `open_fn`/`close_fn` (the custom `close_fn`
    closes the descriptor itself, per the IDF contract): `sessions_open`,
    `sessions_opened_total`, `sessions_closed_total`, `session_high_water`,
    and `session_table_full_count` with a rate-limited
    `session_table_full` journal event — table saturation is the
    precondition for LRU purge closing a live browser connection;
  - `ESP_HTTP_SERVER_EVENT` handler counting `HTTP_SERVER_EVENT_ERROR`
    (`transport_error_count`, `last_transport_error_code`) with a
    rate-limited `server_error_event` journal event;
  - per-chunk send timing in the JSON stream: `max_chunk_send_ms`,
    `send_stall_count` (chunk blocked ≥ 500 ms), and a rate-limited
    `send_stall` journal event naming the URI — direct evidence of a
    slow/dead client pinning the single server task;
  - per-request duration buckets `request_duration_counts`
    (≤100/≤500/≤1000/≤2500/>2500 ms);
  - `request_during_ota_count` plus a rate-limited `request_during_ota`
    journal event when a WebUI request arrives while OTA transport is
    active — the screen-lock/quiet-period collision hypothesis probe;
  - `recv_wait_timeout`/`send_wait_timeout` lowered 5 s → 2 s so a dead
    session releases the server task sooner;
  - `max_open_sockets` unchanged at four, now via the named
    `WEBUI_MAX_CLIENT_SESSIONS` constant shared with the saturation check.
- `tools/webui_soak.py` — stdlib-only, read-only (GET endpoints) soak
  driver mirroring the browser profile (three sequential requests per 2 s
  cycle, 2.5 s abort deadline), with optional `--churn` (mid-flight RST
  aborts) and `--slow-reader` (trickle-read session hold) chaos clients;
  prints latency percentiles, first failures, and the device's final
  `/api/status.http`, and exits nonzero against the acceptance bar above.
- `tests/test_field_safety_contracts.py` — new
  `test_webui_instruments_the_session_and_socket_layer` source contract;
  the socket-reservation contract updated for the named constant.
- Validation: all 18 host tests pass; pinned ESP-IDF 5.5/Python 3.10 build
  passes; the application is `0x153ac0` bytes leaving `0x2ac540` (67%)
  free in the smallest app partition. No frontend or endpoint schema
  changes; all new status fields are additive. Source/build validated
  only — hardware flash, the first instrumented soak run, and OTA
  publication remain separate approval gates.

OTA publication update — 2026-08-08:

- After explicit approval, the Phase A head `bba2413` was pushed to
  GitHub and rebuilt/published through `tools/release.sh` with the pinned
  ESP-IDF 5.5/Python 3.10 environment.
- The published `esp32-sim7670g-bba2413.bin` is 1,391,296 bytes with
  SHA-256
  `cae122ae46f007045dd76faead95bd2ba68617e9d6aab0617855caf6c52e719d`.
  The atomically replaced manifest was verified externally: fresh version,
  full-download size and checksum, and a 1,024-byte HTTP Range request
  answered with status 206.
- No USB flash occurred. The field device is expected to install
  `bba2413` at its next OTA window (checks defer while a SoftAP client is
  associated and during the post-use quiet periods). Acceptance of Phase A
  requires device-local or production evidence of `bba2413` running,
  followed by field `/api/status.http` and `/api/events` review after
  real SoftAP use.
- The chosen field-evidence path: gather organic SoftAP-session counters
  first; the controlled `tools/webui_soak.py --churn --slow-reader` bench
  run (which requires forgetting the stored home network to force SoftAP
  indoors) is deferred unless field evidence is inconclusive.
- Automatic supervisor redial and modem-reset escalation remain disabled.

#### Phase A field evidence — 2026-08-17

Deployment confirmed first: `bba2413` installed by OTA on 2026-08-09 18:29:59
PDT and reported `ota_verified` nine seconds later, running from `ota_0` with
`pending_verify: false` and no rollback. `c5c48b4` remains in `ota_1`.

Evidence was then gathered from a genuine operator session rather than a bench
run: the operator joined the SoftAP in the vehicle, used the dashboard, and
switched on the fridge while watching pack current respond in the UI. Device
`gw-e3aba4`, boot `3e55643ee22309c4`, uptime ~75 h at capture. The HTTP
counters and the 48-slot ring are RAM-only, so these are this boot's totals.

`/api/status.http` after the session:

- 825 requests with `failure_count: 0`, `response_error_count: 0`,
  `serialization_failure_count: 0`, `stream_failure_count: 0`.
- `request_duration_counts`: 821 ≤100 ms, 2 ≤500 ms, 1 ≤1000 ms, none above.
- `min_free_heap: 9944`, `min_largest_free_block: 2560`,
  `min_task_stack_free: 4512`.
- `sessions_opened_total: 28`, `sessions_closed_total: 26`,
  `session_high_water: 4`, `session_table_full_count: 13`.
- `send_stall_count: 1`, `max_chunk_send_ms: 956`.
- `transport_error_count: 18`, `last_transport_error_code: 6`.
- `request_during_ota_count: 0`.

Verdict against the shared acceptance bar: **passes on success rate and
latency, fails on heap.** p95 is far below the 1 s target and no `ENFILE`
occurred, but `min_free_heap` of 9,944 bytes is well under the 15 KB floor,
and one error banner did occur (see below).

What the counters settle:

- **L1 (session/socket fragility) — confirmed.** `session_table_full_count: 13`
  with `session_high_water` pinned at the maximum of 4 means the table
  saturated thirteen times, each an interval in which LRU purge can evict the
  live poll socket. The journal caught one directly:
  `http/session_table_full` `{"open":4,"max":4,"heap":28020}`. One slow-client
  stall was also observed — `http/send_stall`
  `{"uri":"/api/status","chunk_ms":956,"ms":1000}` — a chunk blocked for
  nearly a second on the single server task.
- **L2 (thin internal RAM) — confirmed, and worse than the summary counters
  suggest.** The single failed request journaled
  `{"uri":"/api/status","ms":41,"error":45062,"heap":21192,"largest":5632,`
  `"stack":4512,"min_heap":840}`. `error 45062` is `ESP_ERR_HTTPD_RESP_SEND`
  (0xB006), a client vanishing mid-response. `min_heap` is
  `esp_get_minimum_free_heap_size()`, i.e. the all-time system low since boot:
  **840 bytes**. `min_largest_free_block: 2560` also sits inside the
  1,920–2,944 byte band where this system has historically failed TLS
  allocations. The historical minimums quoted in L2 above are not stale — they
  are still current behavior on `bba2413`.
- **L4 (polling cost) — not a current problem.** 821 of 824 completed requests
  finished in ≤100 ms, so the 2.5 s client deadline is not being missed on
  handler latency. Phase D remains worth doing to shrink exposure, but it is
  not what the operator was feeling.
- **L5 (blocking AT/ping) — untested by design.** The operator did not use
  either feature, consistent with the 2026-08-08 evidence.
- **Quiet-period/TLS collision hypothesis — still unresolved.** An OTA check
  did run during the session (`ota/state_changed` 0→1→1→0), but
  `request_during_ota_count: 0` means no WebUI request overlapped it. Neither
  confirmed nor ruled out.

Independent corroboration from the production side: a broker subscription on
`bms/status/+` watched the whole session and recorded no `mqtt_reconnected`
status event. Compare the `c5c48b4` history, which logged fifteen
`mqtt_reconnected` events between 2026-08-08 21:27 and 2026-08-09 01:29. The
control-plane deference added in `6cf9a16` and the socket budget from `4d9759d`
are doing their job: an operator can now use the local UI without destabilizing
cellular transport. Field telemetry cadence over the same period held at a
9.88 s average against the flat 10 s poll from `c5c48b4`.

Instrumentation defect found by this capture, to be fixed in Phase C:
`transport_error_count` and its `httpd_transport_error` journal reason are
mislabeled. `HTTP_SERVER_EVENT_ERROR` delivers an `httpd_err_code_t`, not a
socket error, and code `6` is `HTTPD_404_NOT_FOUND` — so the 18 counted
"transport errors" are ordinary 404s (favicon and similar browser probes).
Rename to reflect HTTP status errors and count genuine transport failures
separately, or the counter will keep overstating trouble.

Conclusion: Phase A's deliverable is complete — the suspected mechanisms are
now observable, and a real field baseline exists. That baseline fails the
shared acceptance bar on heap headroom and confirms L1 session saturation,
which together are the entry criteria for Phase B and then Phase C. Phase D
drops in priority on this evidence.

### Phase B: Enable PSRAM (8 MB)

Promoted to second because it multiplies the margin every other phase
spends, and because L2 heap pressure is a live suspect for the observed
instability.

Deliverables:

- Confirm the PSRAM variant from the boot ROM line / `esptool.py flash_id`
  (expected octal 8 MB on this Waveshare S3 module) and enable
  `CONFIG_SPIRAM` with the matching mode (`CONFIG_SPIRAM_MODE_OCT`),
  `CONFIG_SPIRAM_USE_MALLOC`, boot-time memory test on
  (`CONFIG_SPIRAM_MEMTEST`), and a sane
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` threshold (start at the 16 KB
  default) plus `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` so
  DMA/WiFi-critical allocations keep guaranteed internal space.
- Remember the standing gotcha: `sdkconfig.defaults` edits do **not**
  propagate into the generated `sdkconfig` — set both, or fullclean.
- Evaluate (separately toggleable, evidence-gated): moving mbedTLS
  allocations to PSRAM and `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`. Each is
  its own measurement, not a bundle.
- Add PSRAM free/min-free to `/api/status` alongside the existing internal
  heap fields so both pools are observable in the field.

Exit criteria:

- Clean boot with memory test passing; soak test passes with BMS + SD
  active; an explicit OTA check during browser polling completes without
  the historical contiguous-allocation failures; internal min free heap
  stays comfortably above the Phase A bar; no WiFi/PPP throughput
  regression (PPP transfer rate within 10% of the 460800-baud baseline).
- Measure against the 2026-08-17 pre-PSRAM field baseline, so the gain is a
  number rather than an impression: `http.min_free_heap` 9,944 bytes,
  `http.min_largest_free_block` 2,560 bytes, and an all-time
  `esp_get_minimum_free_heap_size()` of 840 bytes. The 15 KB acceptance floor
  is the target for the first of these.
- Known risk to respect: PSRAM changes cache/timing behavior globally.
  Treat this as its own flash-gated experiment with a straightforward
  rollback (config revert), and run the full soak — not just a boot check —
  before publication.

#### Phase B implementation record — 2026-08-17

Source and build level only. The hardware gates — PSRAM variant confirmation,
USB flash, soak — are still open; nothing has been flashed or published.

Configuration (`sdkconfig.defaults`, and the generated `sdkconfig`
regenerated from it so the standing propagation gotcha cannot bite):

- `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_TYPE_AUTO=y`,
  `CONFIG_SPIRAM_SPEED_40M=y` (the default; 80 MHz is a separate experiment),
  `CONFIG_SPIRAM_USE_MALLOC=y`, `CONFIG_SPIRAM_MEMTEST=y`,
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`,
  `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`.
- Kconfig pulled in four derived options, all wanted:
  `CONFIG_FATFS_ALLOC_PREFER_EXTRAM` (SD/FAT buffers move off internal RAM —
  a direct win for this build, which logs to microSD),
  `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` with
  `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` (opt-in per task; no existing
  task moves), and the standard
  `CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND` /
  `CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND` errata workarounds. Diffing the
  regenerated `sdkconfig` against the pre-Phase-B copy showed **only** PSRAM
  symbols added and nothing dropped, so no local configuration drift was lost.
- `CONFIG_SPIRAM_IGNORE_NOTFOUND` deliberately left off: a mode or variant
  mismatch aborts at the boot memory test instead of running degraded. On a
  bootloader with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` that failure mode
  is self-recovering — the new image never marks itself valid, so the
  bootloader reverts to the previous slot.
- The evidence-gated extras stay off and are recorded as such in
  `sdkconfig.defaults`: `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` and
  `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`.

Delivery finding, checked rather than assumed: **nothing in the ESP-IDF 5.5
bootloader depends on `CONFIG_SPIRAM`** (a `CONFIG_SPIRAM` grep over
`components/bootloader`, `components/bootloader_support` and
`components/esp_rom` returns nothing; PSRAM is brought up by the app during
startup). Enabling PSRAM therefore reaches the field device through an
ordinary app-only OTA update — no bootloader reflash, and the rollback slot
stays valid. The bootloader binary is unchanged at `0x52c0` bytes.

Observability — the important correctness consequence of enabling PSRAM.
`esp_get_free_heap_size()`, `esp_get_minimum_free_heap_size()` and
`heap_caps_*(MALLOC_CAP_8BIT)` all span *both* pools once PSRAM is on, so
every recorded resource metric would have jumped to an 8 MB number and the
2026-08-17 baseline would have become uncomparable overnight — while the
internal-RAM pressure those metrics exist to catch went unmeasured. Every
resource query in `main/webui.c`, `main/ota.c` and `main/modem.c` is now
explicitly `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` (named
`WEBUI_INTERNAL_CAPS` / `OTA_INTERNAL_CAPS` / `MODEM_INTERNAL_CAPS`). With
PSRAM disabled these return exactly what the previous calls returned, so
`bba2413`'s numbers remain a valid before-picture. Field names are unchanged;
no endpoint schema was broken.

New additive fields, all in `/api/status.http` unless noted:

- `min_free_heap_all_time` — `heap_caps_get_minimum_free_size()` over the
  internal pool, i.e. the all-time low since boot. Phase A could only recover
  this number (840 bytes) from a journaled failure detail that happened to
  fire; it is now readable on demand.
- `psram_total`, `psram_free`, `min_free_psram`,
  `min_largest_free_psram_block`, `min_free_psram_all_time` — the external
  pool, tracked at request boundaries the same way the internal minima are.
  All read zero on a build with PSRAM disabled, so the fields are safe either
  way.
- `/api/ota` gains `free_psram` beside its existing internal-heap footer.
- The `http/request_failed` and `http/slow_request` journal details gain
  `psram`, and their `min_heap` is now the internal all-time low. The detail
  buffer moved 192 → 256 bytes so the widest expansion (189 bytes) cannot
  truncate; that still fits the journal's own 192-byte `EVENT_DETAILS_MAX`,
  over which `event_journal_emit()` would discard the whole detail as a
  redaction error and lose precisely this evidence.

Validation: 20 host tests pass (18 prior plus two new contracts — one
asserting the PSRAM options in both `sdkconfig.defaults` and any generated
`sdkconfig`, which is the standing gotcha made into a test failure rather
than a field surprise; one asserting no resource query reaches for the
pool-spanning APIs and that both pools appear in `/api/status`).
`node --check` passes on the embedded JS (unchanged this phase). The pinned
ESP-IDF 5.5 / Python 3.10 build is clean and warning-free.

Build-level memory evidence, measured against a control build of the same
source with PSRAM off (`idf.py size`, same toolchain):

| | PSRAM off | PSRAM on | delta |
|---|---|---|---|
| DIRAM used | 184,387 | 188,083 | **+3,696** |
| DIRAM remaining | 157,373 | 153,677 | −3,696 |
| App image | 1,391,609 | 1,398,709 | +7,100 |

So PSRAM support costs ~3.7 KB of internal RAM statically before it returns
anything, and `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` fences a further 32 KB
of internal heap off from ordinary `malloc()` (still counted by the internal
metrics above — that reserve is exactly the DMA/stack/TLS headroom the
acceptance bar cares about).

Expectation to hold honestly before the soak, so the result is judged rather
than hoped for: with `ALWAYSINTERNAL` at the 16 KB default, small allocations
— including the per-request cJSON fragments — still prefer internal RAM. The
gain in this configuration comes from large allocations moving out (FATFS/SD
buffers via `FATFS_ALLOC_PREFER_EXTRAM`, and any allocation over 16 KB), not
from the WebUI's own traffic. If the first soak leaves `min_free_heap` short
of the 15 KB floor, the ordered next knobs are: lower `ALWAYSINTERNAL`
(4096, then 2048), then the two gated extras — mbedTLS allocations in PSRAM
first, since the recorded failures are TLS-shaped, then
`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`. One change, one measurement.

Still open, in order: confirm the PSRAM variant on hardware
(`esptool.py flash_id` should report "Embedded PSRAM 8MB" and the boot line
should show octal mode) — the device is vehicle-resident and was not
connected when this was built, so the octal assumption rests on the
ESP32-S3R8 module identification and the user confirmation of 2026-08-08 and
must be checked at the USB gate before trusting the build; then flash over
USB; then the full `tools/webui_soak.py` run with BMS + SD active plus an
explicit OTA check during polling; then, only on those numbers, OTA
publication. Rollback remains a config revert.

#### Phase B hardware confirmation — 2026-08-17

Flashed over USB in the field, on the same unit that produced the Phase A
baseline (MAC `a0:f2:62:e3:ab:a4` = `gw-e3aba4`).

Variant confirmed before trusting the build, as the deliverable required.
`esptool.py flash_id` reported `Chip is ESP32-S3 (QFN56) (revision v0.2)`,
`Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)`, 16 MB flash with the
eFuse flash type set to quad. The S3 ships no 8 MB *quad* in-package PSRAM
variant, so "Embedded PSRAM 8MB" identifies the ESP32-S3R8 and settles octal
mode; the boot log then confirmed it directly rather than by inference.

Boot log on `9a1bc10`, the lines that matter:

- `octal_psram: vendor id : 0x0d (AP)`, `dev id : 0x02 (generation 3)`,
  `density : 0x03 (64 Mbit)`, `VCC : 0x01 (3V)`,
  `Readlatency : 0x02 (10 cycles@Fixed)`.
- `esp_psram: Found 8MB PSRAM device`, `esp_psram: Speed: 40MHz`.
- **`esp_psram: SPI SRAM memory test OK`** — the memory test the config
  deliberately left enabled, passing.
- `esp_psram: Adding pool of 8192K of PSRAM memory to heap allocator` and
  `esp_psram: Reserving pool of 32K of internal memory for DMA/internal
  allocations`, i.e. `SPIRAM_MALLOC_RESERVE_INTERNAL` doing its job.
- Internal heap at init: 206 KiB + 21 KiB + a 32 KiB DRAM pool + 7 KiB
  RTCRAM. No boot warnings or aborts; SD mounted, BMS and MAX17048 online,
  PPP up on Verizon LTE, MQTT connected.

First `/api/status.http` read, light load, minutes after boot — indicative
only, **not** a soak result and not comparable to a 75-hour field session:

| metric | Phase A (`bba2413`) | `9a1bc10` first read |
|---|---|---|
| all-time internal low | 840 B | **31,907 B** |
| `psram_total` / `psram_free` | — | 8,388,608 / 8,355,204 |
| `min_free_psram_all_time` | — | 8,354,048 |

Only ~33 KB of PSRAM was in use at that point, which is consistent with the
expectation recorded above: at `ALWAYSINTERNAL` 16 KB the WebUI's own small
allocations still prefer internal RAM, so most of the internal gain here
comes from large allocations (SD/FAT buffers) moving out. Note that
`min_free_heap` and `min_free_psram` read zero on the very first request of a
boot: `observed_handler()` updates the minima after the response fragment is
built, so they populate from the second request onward. Expected, not a
defect.

Control-plane deference also confirmed on the new build: the routine OTA
check at 91.5 s logged `routine check deferred for local control plane
(ap_client_active)`.

Deliberately not done, and why:

- **The acceptance soak was deferred**, so Phase B is *not* accepted. The
  field laptop reaches the internet through a mobile hotspot; joining the
  device's SoftAP for the 30-minute run would have cut its only route. The
  soak, and the explicit OTA check during polling, remain the open exit
  criteria.
- **No OTA publication.** The plan gates publication on the full soak, and
  the soak has not run.
- Consequence to expect: `9a1bc10` was flashed locally while the published
  manifest still names `bba2413`. Version selection is a plain string
  comparison, not an ordering, so once no SoftAP client is associated and the
  quiet periods lapse the device will install `bba2413` over this build at a
  routine check. That is the intended config-revert rollback rather than a
  failure — but it means the PSRAM build does not persist in the field
  without a publication, and a future soak needs a fresh USB flash first.

#### Phase B acceptance soak — 2026-08-17 (bench, SoftAP)

Run indoors on the same unit, forced onto SoftAP by clearing the stored STA
credentials (`POST /api/wifi {"ssid":""}`) — the indoor STA network otherwise
takes the AP down. The laptop joined the SoftAP for the soak while keeping
internet on ethernet; the local-only SoftAP design from `8ff3076` is what made
that possible, since the AP offers no router or DNS and therefore never became
the default route. Device rebooted first, so all counters below are this run.

`tools/webui_soak.py --host 192.168.4.1 --minutes 30 --clients 2 --churn
--slow-reader`:

| metric | Phase A (`bba2413`) | this soak (`9a1bc10`) |
|---|---|---|
| polls succeeded | — | 5,388 / 5,388 (100%) |
| p50 / p95 / max latency | — | 57.8 / 109.8 / 322 ms |
| `min_free_heap` | 9,944 | **33,531** |
| `min_largest_free_block` | 2,560 | **10,240** |
| all-time internal low | **840** | **19,095** |
| `max_chunk_send_ms` | 956 | 11 |
| `send_stall_count` | 1 | 0 |
| `session_high_water` | 4 | 4 |
| `session_table_full_count` | 13 | 5 |

Verdict against the shared acceptance bar: **passes on every clause.** 100% of
polls succeeded, p95 is an order of magnitude inside the 1 s target, no
`ENFILE` occurred, and the 15 KB internal-heap floor is cleared by the
*all-time* low of 19,095 bytes, not merely by the per-request minimum. Server
side, 5,508 of 5,509 requests completed in ≤100 ms with none above.

The 87 `failure_count` entries are all `error 45062`
(`ESP_ERR_HTTPD_RESP_SEND`) from the `--churn` client aborting mid-response on
purpose; the journal shows no other failure reason and the client observed
zero failures. `min_largest_free_block` never approached the 1,920–2,944 byte
band where this system has historically failed TLS — the journaled failure
details report `largest` between 13,312 and 22,528 bytes throughout. The modem
was healthy across the run: PPP up, 60 AT/GNSS pause windows, zero pause
failures, no redials or restarts.

How to read the gain honestly. PSRAM absorbed only ~33 KB (`psram_free`
8,355,204 of 8,388,608), so this is **not** eight megabytes of pressure relief.
Most of the internal headroom comes from `SPIRAM_MALLOC_RESERVE_INTERNAL`
fencing 32 KB off from ordinary `malloc()` — that reserve is still counted by
the internal metrics because it remains internal 8-bit RAM, and it is exactly
the DMA/stack/TLS headroom the acceptance bar exists to protect, but the
mechanism is a protected reserve rather than bulk relocation. This matches the
expectation recorded before the run rather than beating it, and it means the
`ALWAYSINTERNAL` and mbedTLS knobs still have room to give more if a later
phase needs it.

L1 is unchanged by this phase, as expected: `session_table_full_count` of 5
with `session_high_water` pinned at 4 means the table still saturates and LRU
purge can still evict a live socket. That is Phase C's work, not Phase B's.

**Gap in the exit criteria, recorded rather than glossed:** the criterion says
"soak test passes with BMS + SD active", and this run had neither. The battery
pack stayed in the vehicle while the gateway came indoors, so the BMS UART
talked to nothing (`polls: 63, fails: 63, ever_ok: false`) and the datalog
never wrote (`rows: 0`, `sd_flush_count: 0`) despite the card being mounted.
The SD/FATFS allocation path — the one `FATFS_ALLOC_PREFER_EXTRAM` was
expected to relieve — was therefore never exercised, and the soak load was
lighter than the field. Phase B is accepted **conditionally** on that basis: a
field re-check with the pack connected must confirm the heap result under real
BMS + SD load before the phase is closed outright. A synthetic-BMS re-soak
(`POST /api/bms {"sim":true}`) was considered and deliberately declined, to
keep fabricated rows out of the SD log and the production broker.

#### Phase B OTA and TLS evidence — 2026-08-17

`b062302` was published through `tools/release.sh` (1,398,832 bytes, sha256
`c72f9ee0b5424cf340b8620e04864cb2ef206518815a31bcf08eac1f4d847fb6`, manifest
verified externally with a Range request answering 206). Publishing first was
a deliberate inversion of the plan's ordering: a manual OTA check bypasses all
deference *and* auto-installs any version mismatch, so the "explicit check
during polling" criterion can only be exercised non-destructively once the
manifest names the running build. The install itself then supplied the
cellular-transfer measurement.

The check ran with two poll clients active and the device on cellular only
(STA credentials cleared for the soak, so PPP was the only route).

**The criterion failed.** The manifest fetch and the first 786,432 bytes
succeeded, then:

```
ota: download_transfer failed: esp=ESP_FAIL
     tls=ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED mbedtls=0x7f00
     flags=0x0 errno=0 heap=21383 largest=4608 minimum=1743
```

`mbedtls=0x7f00` is `MBEDTLS_ERR_SSL_ALLOC_FAILED` — an out-of-memory failure
inside the TLS handshake, i.e. **exactly the historical failure mode Phase B
was meant to remove**, reproduced with PSRAM enabled and 8.35 MB of external
heap free at the time. The all-time internal low reached 1,743 bytes during
the attempt and 1,487 bytes by the end of the cycle, against the 840-byte
pre-PSRAM record. Enabling PSRAM did not move this failure, because mbedTLS
allocations are still internal by construction:
`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` is deliberately off as an evidence-gated
extra. That gate is now backed by evidence.

The corollary matters for how the soak result is read: the 15 KB floor holds
comfortably for WebUI polling alone (19,095 bytes all-time in the soak), but
not once TLS is in the picture. Even the post-reboot self-test — one HTTPS
connection with light polling — drove the all-time internal low to 13,367
bytes, already under the floor. **Phase B raised the WebUI path well clear of
the bar and left the TLS path essentially where it was.**

What worked, and is worth keeping on the record:

- The passive-retry and resume architecture from the earlier phases handled a
  real allocation failure exactly as designed: no modem redial, a passive
  wait, then `resuming download at 786432/1398832 bytes`, completing with
  `sha256 verified against manifest`. The failure cost a retry, not a
  transport teardown.
- **PPP throughput shows no regression.** The resumed transfer moved 612,400
  bytes in 24,703 ms — **24 KB/s**, against the 25–34 KB/s baseline for
  460800 baud, and measured *under concurrent poll load* the baseline never
  had. Earlier mid-flight samples suggesting 6–13 KB/s were spanning the
  stall and failure window, not clean throughput. The cache/timing regression
  this phase's risk note warned about did not materialise.
- Rollback safety behaved: the new image booted pending-verify, self-tested
  over HTTPS, and marked itself valid 38 s in.

Instrumentation defect found by this run, for Phase C alongside the
mislabeled error counter: `request_during_ota_count` is RAM-only, so when an
OTA check ends in an install the reboot destroys the very counter that
recorded the overlap. It read 0 afterwards despite polling having run
throughout the download. The quiet-period/TLS collision probe needs to
survive the event it measures — journal it (the SD journal spans boots)
rather than relying on the counter alone.

Revised verdict on the Phase B exit criteria:

| criterion | result |
|---|---|
| clean boot, memory test passes | pass |
| soak with BMS + SD active | not run (BMS/SD absent; see above) |
| explicit OTA check during polling, no allocation failure | **fail** |
| internal min free heap above the Phase A bar | pass for WebUI, fail under TLS |
| no PPP throughput regression | pass (24 KB/s under load) |

Phase B therefore **stays open**. The next step is the first gated extra on
its own — `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`, moving TLS allocations to
PSRAM — measured by repeating exactly this OTA-under-polling test, since it
now has a known, reproducible failure to beat. One change, one measurement,
as before. Lowering `SPIRAM_MALLOC_ALWAYSINTERNAL` is the fallback if that is
insufficient; `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` remains last.

#### Phase B, mbedTLS in PSRAM — 2026-08-17

The first gated extra, taken on its own as the plan requires. Single knob:
`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` replacing the IDF default
`CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC`; the regenerated `sdkconfig` differs from
its predecessor by exactly those two lines. `CONFIG_MBEDTLS_DYNAMIC_BUFFER`
was left alone — it governs *when* TLS buffers are allocated, this governs
*where*. `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` remains off and unmeasured.

Measured by repeating the failing test exactly: `646ead9` flashed over USB
while the manifest still named `b062302`, so a manual check pulled a 1.4 MB
image over TLS with two poll clients running. Same operation, same load, same
manifest; the only difference is where TLS allocates.

| | TLS internal (`b062302`) | TLS in PSRAM (`646ead9`) |
|---|---|---|
| before download: `largest` | 10,752 | **27,648** |
| before download: `minimum` | 16,675 | **43,203** |
| download attempts | 2 (first died at 786,432) | **1** |
| TLS failure | `mbedtls=0x7f00` `ALLOC_FAILED` | **none** |
| all-time internal low after | **1,487** | **24,187** |
| throughput | 24 KB/s | 24 KB/s |

**The criterion passes.** No allocation failure occurred, the image verified
against the manifest sha256 on the first attempt, and the all-time internal
low of 24,187 bytes clears the 15 KB floor *under TLS plus polling* — the one
condition Phase B had previously failed.

The mechanism was observed directly rather than inferred: `psram_free` fell
8,355,204 → 8,331,712 across the handshake, so roughly 23 KB of TLS
allocation moved off internal RAM. The option is doing work, not sitting
inert because a dependency was unmet.

Throughput is unchanged at 24 KB/s against the 25–34 KB/s baseline, still
measured under concurrent poll load the baseline never had, so moving TLS
buffers into slower external RAM cost no measurable transfer rate here.

The WebUI stayed healthy for the whole download: 330 polls, zero failures,
p95 rising from ~109 ms idle to ~336 ms during the transfer — a third of the
1 s target. The first sample of the transfer hit 1,228 ms as it spun up,
which is the OTA/polling contention this plan predicted under L2, and it
recovered immediately.

Caveat on the measurement, recorded rather than buried: the attempt resumed
from a stale 262,144-byte offset held in the `otares` NVS namespace (keyed by
version and sha, and NVS survives a USB flash), so it transferred 1,136,688
bytes rather than the full image. That is still 1.45× the 786,432 bytes the
failing build managed before dying, and the internal minimum never came near
the failure band — but a from-zero download would remove the caveat entirely
and is worth doing when the resume state is next clear.

Consequence of testing this way: on success the device installed `b062302` —
the build the manifest names — and rebooted into it, self-tested over HTTPS
and marked itself valid. So the improvement is **measured but not deployed**;
`646ead9` sits in `ota_0` and the running image is still the one with the
known TLS failure. Publishing `646ead9` is the outstanding decision.

Phase B exit criteria, revised again:

| criterion | result |
|---|---|
| clean boot, memory test passes | pass |
| soak with BMS + SD active | still not run (BMS/SD absent) |
| explicit OTA check during polling, no allocation failure | **pass on `646ead9`** |
| internal min free heap above the Phase A bar | pass, WebUI *and* TLS |
| no PPP throughput regression | pass (24 KB/s, unchanged) |

The only criterion still outstanding is the BMS + SD load, which needs the
gateway back on the pack in the vehicle.

### Phase C: Session and socket robustness

The direct fix for L1, now affordable because Phase B relaxed the RAM
rationing. Informed by Phase A numbers.

Deliverables:

- Enable TCP keepalive on accepted sessions (httpd keep-alive config
  fields in IDF 5.5, or a session-open callback setting `SO_KEEPALIVE`
  with short idle/interval/count) so walked-away phones free their
  sockets in seconds instead of holding them until LRU pressure.
- Raise the socket budget: `CONFIG_LWIP_MAX_SOCKETS` 10 → 16 and httpd
  client sessions 4 → 6–8, keeping ≥ 3 reserved outbound slots (MQTT,
  OTA, transient). The `4d9759d` cap was a rationing decision made under
  the old memory budget; re-derive it from Phase A evidence instead of
  inheriting it.
- Verify the lowered send/recv wait timeouts from Phase A against a
  deliberately stalled client; tune so a dead session costs the server
  ≤ ~2 s.
- Add the web-service watchdog (standing Phase 3 deliverable): if clients
  are associated but no request has completed within a threshold while
  attempts are arriving, journal and `httpd_stop()`/`httpd_start()`.
  Restart-not-reboot.
- Correct the mislabeled error counter found by the Phase A field capture:
  `HTTP_SERVER_EVENT_ERROR` carries an `httpd_err_code_t` (status-level, e.g.
  `HTTPD_404_NOT_FOUND`), not a socket error. Count HTTP status errors and
  genuine transport failures as separate quantities so 404s stop inflating
  `transport_error_count`.

Exit criteria:

- Kill-test: associate a phone, start polling, then repeatedly drop the
  association mid-request / walk out of range; sockets recover without
  LRU closing the surviving client's connection, and the soak still
  passes. Safari with six parallel connections never evicts the poll
  socket.

### Phase D: One poll request, friendlier cadence

Shrinks exposure to every remaining failure mechanism (L4).

Deliverables:

- Fold the `wifi` and `ota` payloads into `/api/status` (both already
  exist as builder fragments; the standalone endpoints stay for
  compatibility and manual use). The dashboard makes **one** request per
  cycle instead of three: ~3× fewer requests, aborts, and socket events.
- Slow the cycle to 3 s; all cards update atomically so perceived
  freshness is unchanged or better.
- Raise the client deadline to 5 s — after Phases B/C a slow response is
  worth waiting for rather than aborting and re-dialing sockets.
- Exponential backoff (3 s → 6 s → 12 s, cap 30 s) after consecutive poll
  failures, reset on success; pause polling via the Page Visibility API
  when the tab is hidden (also prevents the screen-lock/quiet-period OTA
  collision from meeting an actively polling page).
- Keep the visible error banner; add a small "last update N s ago"
  freshness indicator so a paused/backed-off state is legible.

Exit criteria: soak passes with request rate ≤ 1/3 s per client; banner
appears only when the device is genuinely unreachable.

### Phase E: Keep the AP available (APSTA field mode)

Fixes L3 — session-start pain and AP disappearance during STA retry storms.
Touches the WiFi supervisor, so it lands after the HTTP layer is solid and
observable.

Deliverables:

- `WIFI_MODE_APSTA` while STA is connecting/retrying: the AP comes up
  immediately at boot and never disappears during reconnect storms.
- Decide and record the steady-state policy explicitly (the plan's standing
  requirement): recommended default — AP stays up whenever STA is not
  connected; when STA connects, keep the AP for a grace period (~5 min),
  then turn it off unless an NVS `field_mode` flag pins it on. The
  existing WPA2 password stays; document the tradeoff.
- Handle the APSTA channel constraint: the AP must follow the STA's
  channel while both are active; when STA is down the AP keeps its fixed
  channel. Journal channel moves; a brief beacon move is far better than
  minutes of no AP.
- Additive `/api/status.wifi` fields: current mode, AP-enabled reason,
  grace-period remaining.

Exit criteria (from the remediation plan's Phase 3, unchanged): the UI and
`/api/status` respond within two seconds while cellular is unavailable, PPP
is redialing, the modem is restarting, and MQTT is retrying; the AP remains
usable through each transition — plus: the AP is joinable within 10 s of
power-on with home credentials stored and out of range, and survives a
forced STA drop without disassociating clients.

### Phase F: Async job model for AT/ping

Still the right architecture — it restores two diagnostic features the
operator currently must avoid — but demoted from root-cause fix to feature
repair on the operator's evidence.

Deliverables:

- A small job registry (one slot per job type: `at`, `ping`). POST
  validates, enqueues, and returns `{"ok":true,"job":"at"}` immediately; a
  busy slot returns an error.
- Execution on the modem task's existing loop (it already owns the AT
  channel and serializes against polling) — the "modem task is the sole
  owner of UART transitions" constraint is preserved and no new stack is
  at risk (the Phase 2 overflow is the cautionary tale; record stack
  high-water for the executing task).
- Results in a bounded static buffer exposed as a `jobs` fragment in
  `/api/status` (state `idle/running/done/error`, timestamps, result
  text); the AT console and ping card poll it like everything else.
- Source-contract test: no HTTP handler calls
  `modem_send_at()`/`modem_ping_host()` directly.

Exit criteria: soak passes **while** an AT command and a 20 s
unreachable-host ping run — polls keep succeeding, no banner. Worst-case
HTTP handler time drops to NVS-write endpoints (tens of ms), asserted via
the Phase A duration counters.

### Phase G: Discovery polish (optional, last)

- mDNS hostname (e.g. `http://gateway.local/`) advertised on both AP and
  STA interfaces — cheap, avoids typing 192.168.4.1, and sidesteps the
  deliberate absence of a DHCP DNS offer.
- Captive-portal probe endpoints remain deliberately unimplemented: the
  `8ff3076` local-only design wants phones to *keep* their own internet
  route, and answering probes invites the OS to treat the AP as a portal.
  Revisit only if discovery is still a pain point after mDNS.

## Explicitly rejected / deferred alternatives

- **SSE or WebSocket push** instead of polling: holds a session socket open
  permanently per client and adds a second code path through the socket
  budget; aggregated 3 s polling achieves the same UX with less machinery.
  Reconsider after Phase C/D if request volume still matters.
- **Second HTTP server task first**: the send-stall problem is better fixed
  by timeouts, keepalive, and socket budget than by adding concurrency to
  an embedded server.
- **Re-enabling captive portal DNS**: conflicts with the field-proven
  local-only SoftAP design.

## Test and evidence strategy

- Every phase adds host tests in the existing harness style plus
  `node --check` on the embedded JS.
- The Phase A soak script becomes the shared acceptance instrument; run it
  before/after each phase and record numbers in this document the way
  activation evidence is recorded in the remediation plan.
- Field acceptance for Phase E mirrors the July 28 SoftAP field test:
  iPhone association, UI use during a drive, cellular routing intact,
  followed by a read-only journal review.
