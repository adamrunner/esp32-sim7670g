# Local WebUI Stability Plan

Status: proposed

Evidence date: 2026-08-08

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
- Known risk to respect: PSRAM changes cache/timing behavior globally.
  Treat this as its own flash-gated experiment with a straightforward
  rollback (config revert), and run the full soak — not just a boot check —
  before publication.

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
