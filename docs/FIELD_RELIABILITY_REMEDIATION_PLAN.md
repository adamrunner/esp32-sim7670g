# Field Reliability Remediation Plan

Status: approved for phased implementation

Evidence date: 2026-07-26

Primary device: `gw-e3aba4`

## Purpose

Make the ESP32-S3/SIM7670G gateway recover predictably from cellular coverage
loss while preserving telemetry, keeping the local WiFi control plane usable,
and leaving enough durable evidence to diagnose the next field failure without
serial access.

This plan covers coordinated changes in:

- `esp32-sim7670g`: firmware, local web UI, SD logging, MQTT delivery, and
  hardware validation;
- `bms-dashboard-server`: backward-compatible telemetry ingestion, delivery
  deduplication, availability persistence, production validation, and database
  migration;
- Anton: broker, logger, SQLite, dashboard, and controlled deployment checks.

Implementation preparation is separate from activation. Building and testing
code does not authorize flashing hardware, publishing an OTA release, changing
the Anton deployment, or deleting production data. Each activation has an
explicit gate below.

## Field Evidence Baseline

The July 26 field test established the following:

- The SD card contained 27,345 structurally valid telemetry rows with no
  malformed or truncated records.
- All SD rows from July 25 and July 26, including timestamp-zero startup rows,
  were present in Anton's production database.
- The only card-only block was 790 rows from an early July 15 session that
  predated the current production ingestion period.
- Production contained 125 unique rows not present in the daily SD files.
  Their position near shutdown boundaries is consistent with MQTT delivery
  before the 30-second SD RAM buffer flushed.
- Production contained 749 excess duplicate telemetry inserts, including 600
  associated with July 26 device timestamps.
- The final field boot logged locally from approximately 16:00 until 16:17.
  MQTT connected around 16:15, disconnected around 16:18, and the remaining
  spool replay completed over home WiFi between 21:55 and 21:59.
- The card finished with no `SPOOL/bms.csv` and `SPOOL/bms.cursor` equal to
  `0`, proving that the backlog drained.
- Broker records showed real reconnects at approximately 12:48, 13:38, 14:09,
  14:23, 14:27, and 14:42. Recovery therefore occurred, but included a
  roughly 46-minute outage and other multi-minute gaps.
- Anton's logger, dashboard, and broker were healthy, and SQLite
  `PRAGMA quick_check` returned `ok`.
- Anton persisted 38 device-status events but zero availability events even
  though the logger subscription and broker ACL included
  `bms/availability/+`.
- The tested device ran firmware `7957a71`. The later manual modem restart
  implementation at `a2f5a10` was not installed on the tested device.

These counts are the reconciliation baseline. Later migrations or cleanup must
not be evaluated by comparing raw row counts alone because the existing
database intentionally still contains historical duplicates.

## Reliability Objectives

The completed system must:

1. Continue collecting BMS telemetry during loss of cellular service.
2. Detect a dead data path even when PPP still reports an IP address.
3. Recover cellular service automatically without requiring the local UI.
4. Escalate from redial to modem restart only when evidence justifies it.
5. Keep the local WiFi UI reachable while cellular recovery is running.
6. Avoid long AT/GNSS windows interrupting MQTT PUBACK transactions.
7. Replay buffered data without creating duplicate production records.
8. Publish trustworthy retained online/offline availability.
9. Preserve a bounded, redacted event history across power cycles.
10. Distinguish capture time, receipt time, and unsynchronized time instead of
    presenting timestamp-zero data as 1969.

## Safety and Compatibility Constraints

- Preserve compatibility with firmware already deployed in the field.
- Deploy backward-compatible backend parsing before firmware begins sending a
  new telemetry envelope.
- Keep credentials, SIM identifiers beyond the existing device ID, and broker
  secrets out of event logs, test output, commits, and chat.
- The modem task remains the sole owner of UART mode transitions and modem
  restart execution.
- Long-running work must not execute inside an HTTP request handler.
- OTA checking/downloading must retain its existing protection against modem
  polling and restart interference.
- Do not reintroduce CMUX. Prior testing showed that failed CMUX negotiation can
  wedge this modem until reset.
- MQTT QoS 1 remains at-least-once; correctness comes from delivery identity
  and server-side uniqueness, not an assumption of exactly-once transport.
- Daily SD telemetry remains human-readable CSV. Delivery metadata may use a
  separate MQTT envelope and spool representation.
- Production SQLite changes require a WAL-safe `.backup`, restore rehearsal,
  and `PRAGMA quick_check` before service recreation.
- Controlled production test records must be uniquely identifiable and removed
  after validation without touching real device data.

## Proposed Runtime Architecture

### Connectivity state

Add a small connectivity supervisor that consumes snapshots from WiFi, modem,
MQTT, and datalog modules. It owns policy and timers but requests actions from
the existing module owners.

Inputs:

- active uplink and WiFi STA/AP state;
- LTE registration, packet attachment, PPP state, and last PPP transition;
- MQTT state, last connect/disconnect, last PUBACK, consecutive failures, and
  last transport error;
- spool bytes/rows pending and replay progress;
- OTA activity;
- recent modem AT/pause failures.

Outputs:

- request a clean PPP redial;
- request a supervised `AT+CRESET`;
- defer recovery because no coverage is present;
- emit structured recovery events and expose current policy state through
  `/api/status`.

The supervisor must never manipulate the modem UART, MQTT client, or WiFi
driver directly.

### Initial recovery policy

Use named constants and synthetic-time tests. Initial values should be
conservative and adjustable after hardware testing:

1. If WiFi is the active working uplink, do not reset the modem merely because
   MQTT is disconnected.
2. If the modem is unregistered or reports no packet attachment, classify the
   condition as `waiting_for_coverage`; retry with backoff and do not repeatedly
   reset a functioning modem.
3. If PPP is up but MQTT has no connection or successful PUBACK for 90 seconds,
   classify the path as `ppp_unhealthy` and request one clean redial.
4. Allow up to 90 seconds for registration, PPP, MQTT, and one confirmed
   publish to recover.
5. After two unsuccessful redial cycles while the modem is responsive and
   registered, request a supervised modem restart.
6. Apply a 15-minute modem-restart cooldown unless the AT channel itself is
   dead. Continue logging and spooling throughout the cooldown.
7. Reset recovery counters only after a stable MQTT/PUBACK interval, initially
   five minutes, rather than immediately after PPP gets an IP.

These are implementation defaults, not immutable product requirements. Record
the actual values in `/api/status` so field evidence can be interpreted later.

### AT/GNSS scheduling

The existing PPP poll can run multiple AT commands with cumulative worst-case
timeouts far beyond MQTT's eight-second PUBACK timeout. Replace the monolithic
window with a coordinated scheduler:

- Add a network-pause gate shared by the modem poll scheduler and synchronous
  MQTT PUBACK operations.
- Never begin an AT window while a telemetry or availability PUBACK transaction
  is active.
- Tell the datalog publisher that a short AT window is beginning so it queues
  new snapshots without manufacturing publish failures.
- Run at most one bounded query per healthy-PPP window.
- Target a maximum pause of three seconds; abort and record any overrun.
- Poll high-value registration/signal state more often than identity data.
- Read modem identity only after boot/restart.
- Poll GNSS less frequently while the MQTT path is busy, and skip rather than
  block when the pause gate is unavailable.
- Poll aggressively in command mode when PPP is already down because those
  queries cannot interrupt data traffic.
- Keep all AT polling suspended during active OTA transport.

Increasing `PUBACK_TIMEOUT_MS` alone is not sufficient. The transport pause
must be bounded and coordinated so timeouts retain diagnostic meaning.

### MQTT delivery envelope

Keep daily SD CSV rows unchanged, but wrap MQTT telemetry and spool entries in
a versioned JSON envelope after the backend is ready:

```json
{
  "schema_version": 2,
  "device_id": "gw-e3aba4",
  "boot_id": "0123456789abcdef",
  "sequence": 1234,
  "captured_at": 1785107836,
  "timestamp_valid": true,
  "csv": "gw-e3aba4,1785107836,..."
}
```

Requirements:

- `boot_id` is generated once per ESP32 boot.
- `sequence` is monotonic within the boot and assigned before the first
  delivery attempt.
- The exact envelope is written to the spool; replay must not regenerate an
  identity.
- Live delivery and replay of the same sample therefore carry the same
  `(device_id, boot_id, sequence)`.
- The backend accepts both legacy raw CSV and schema-v2 envelopes.
- New SQLite delivery columns remain nullable for historical/legacy rows.
- A partial unique index enforces uniqueness only when both delivery identity
  fields are present.
- Duplicate envelope deliveries increment a suppression counter but do not
  create another telemetry row.
- Topic and envelope device IDs must match.
- Size limits and JSON validation must be explicit.

### Availability publishing

Replace the unproven availability path with an asynchronous, observable state
machine:

- Configure the retained QoS 1 offline Last Will before the MQTT client starts.
- Queue retained online availability after each successful connection.
- Track its MQTT message ID and acknowledge only the matching published event.
- Do not block the datalog loop waiting for availability.
- Retry online availability with bounded backoff until acknowledged or the
  session disconnects.
- Expose `requested`, `queued`, `acknowledged`, failure count, and last error in
  `/api/status`.
- Publish a clean retained offline state before intentional MQTT
  reconfiguration when the session is healthy enough to acknowledge it.
- Validate broker retention, ungraceful Last Will, graceful reconfiguration,
  reconnect, and repeated-state suppression on Anton.

### Local control plane

Make local management independent of cellular health:

- Add a documented field mode using `WIFI_MODE_APSTA` so the SoftAP stays
  available while STA association and cellular recovery continue.
- Decide the secure default for field mode during implementation; do not expose
  a permanently enabled weak AP without explicitly recording that tradeoff.
- Continue serving `http://192.168.4.1/` on the AP interface regardless of the
  default internet route.
- Add common captive-portal probe endpoints and a local DNS redirect so phones
  can discover the HTTP UI without HTTPS guessing.
- Move ping, raw AT, modem restart, and other long operations to asynchronous
  jobs. A POST should acknowledge the request and later status calls should
  report progress.
- Ensure the HTTP task never waits on a modem command, DNS lookup, ICMP run,
  MQTT PUBACK, or OTA transfer.
- Add request timeout/error feedback in the browser instead of silently
  swallowing every refresh failure.
- Expose AP client count, last association/disassociation, HTTP request count,
  last successful request, and last server error.
- Add a web-service watchdog or restartable server lifecycle after long
  handlers are removed.

### Durable event journal

Create a bounded JSON Lines event journal under `/sdcard/events/`.

Each event should contain:

- schema version;
- boot ID and per-boot event sequence;
- monotonic uptime;
- wall timestamp and time source when valid;
- component and event name;
- severity;
- a short reason/error code;
- small structured details.

Record at least:

- boot and reset reason;
- time synchronization;
- SD mount/write/flush failures;
- WiFi mode changes, STA results, AP start, and AP client events;
- web server start/restart and long/failed requests;
- LTE registration and packet-attach transitions;
- PPP up/down, IP changes, pause duration, pause failure, and redial;
- MQTT connect/disconnect/error/PUBACK timeout;
- availability request/ack/failure;
- spool append, replay start/progress/drain, and cursor recovery;
- connectivity-supervisor decisions;
- modem restart request/result;
- OTA start/retry/result.

Journal requirements:

- no credentials or raw AT responses containing identifiers;
- bounded file size/count with deterministic rotation;
- critical transition events flushed immediately;
- lower-priority repetitive events rate-limited;
- failures to write the journal never stop telemetry;
- a small RAM ring exposed through a read-only API for current diagnostics;
- old unsynchronized events remain ordered by boot ID plus monotonic sequence.

### SD durability and time semantics

- Reduce the daily CSV flush interval from 30 seconds to an initial five
  seconds and measure write behavior on hardware.
- Track submitted, queued, written, flushed, dropped, and write-failed rows
  separately; `sd_rows` must not imply durable flush completion.
- Flush immediately before intentional reboot, OTA activation, or requested
  modem/ESP32 maintenance where practical.
- Keep the current immediate append behavior for offline spool records.
- Never rewrite timestamp-zero source data as if the device knew wall time.
- Send `timestamp_valid` in the delivery envelope.
- In production, use `created_at` as receipt time and render an explicit
  `unsynchronized` capture state rather than 1969.
- Defer optional timestamp reconstruction from later clock synchronization to
  a separate design because it creates inferred rather than measured data.

## Phased Execution Plan

### Phase 0: Baseline and test harness

Deliverables:

- Preserve the July 26 evidence counts and hashes in a dated diagnostic note or
  test fixture without committing raw sensitive telemetry.
- Add host-side helpers that can:
  - parse legacy CSV and future envelopes;
  - reconcile card identities with database identities;
  - summarize boot segments, gaps, duplicates, and replay delay;
  - generate a machine-readable field-test report.
- Add synthetic-time unit tests for the proposed recovery policy before
  connecting it to modem actions.
- Record current firmware, backend, schema, and production deployment versions.

Exit criteria:

- Existing firmware builds unchanged.
- Existing backend tests pass unchanged.
- The reconciliation helper reproduces the documented baseline counts.
- Recovery policy tests cover no coverage, dead PPP path, healthy WiFi,
  repeated redial failure, restart cooldown, and stable recovery.

Commit boundary:

- One focused diagnostic/test-harness commit per repository.

### Phase 1: Observability before behavior changes

Deliverables:

- Implement the firmware event schema, RAM ring, bounded SD journal, and
  read-only status/events API.
- Add timestamps and counters for WiFi, PPP, MQTT, PUBACK, spool, availability,
  HTTP, and recovery state.
- Improve UI error rendering so a failed status request is visible.
- Add tests for rotation, redaction, timestamp-zero ordering, rate limiting,
  and write failures.

Exit criteria:

- Power cycling during journal writes leaves parseable files.
- No event contains configured WiFi/MQTT credentials.
- Journal failure does not stop BMS collection, MQTT, or the web UI.
- A test outage can be reconstructed from SD events without serial logs.

Activation gate:

- Build and local validation are allowed.
- Hardware flash or OTA publication requires explicit approval.

#### Phase 0 and Phase 1 implementation record — 2026-07-26

Implementation status:

- Phase 0 is complete for the approved local scope.
- Phase 1 implementation and host fault validation are complete. Direct
  hardware power-cut, FAT-card, and live outage validation remain activation
  gated; no Phase 2 behavior is enabled by these changes.

Commits:

- Firmware `5791090` — synthetic-time recovery-policy reference model and
  tests for no coverage, stale PPP/MQTT, healthy WiFi, repeated redial
  failure, restart cooldown, and stable recovery.
- Backend `91bb63f` — legacy/schema-v2 parsing and field reconciliation
  harness, machine-readable July 26 baseline fixture, boot/gap/duplicate and
  replay-delay summaries.
- Firmware `b6213ff` — fixed RAM event ring, zero-wait producer queue,
  dedicated SD JSONL worker, redaction, interrupted-tail repair, deterministic
  128 KiB/four-file rotation, critical `fsync`, host fault tests, and outage
  report generator.
- Firmware `02bcc6b` — additive WiFi/PPP/MQTT/PUBACK/spool/availability/
  HTTP/recovery counters and monotonic timestamps, read-only `/api/status` and
  `/api/events`, AP-client and transition events, and visible browser status
  errors.

Baseline and validation:

- Revalidated clean starting states before edits: firmware `6c73a38` on
  `main`; backend `940bc2a` on `dev`.
- Treated the mounted `/Volumes/SDCARD` as read-only. Its four BMS CSVs
  contained 27,345 data rows; byte sizes and SHA-256 values are preserved in
  the non-sensitive backend fixture. The same hashes were rechecked after
  implementation.
- The reconciliation fixture reproduces 27,345 card rows, 790 card-only rows,
  125 production-only unique rows, 749 excess production duplicates, 26,680
  derived unique production rows, and 27,429 derived total production rows.
- Backend isolated Python 3.10 validation: 48 tests passed (44 pre-existing
  plus four reconciliation tests).
- Firmware host validation: nine tests passed. Coverage includes rotation,
  redaction, interrupted-tail repair, timestamp-zero ordering, rate limiting,
  write failure, synthetic outage reconstruction, and all six recovery-policy
  cases.
- Embedded browser JavaScript passed `node --check`.
- Clean pinned ESP-IDF 5.5/Python 3.10 build passed at `02bcc6b`; application
  size was `0x150d30`, leaving `0x2af2d0` bytes (67%) free in the smallest app
  partition. The unchanged Phase 0 firmware also built successfully before
  Phase 1 runtime changes.

Compatibility and safety:

- Existing status fields, CSV telemetry, MQTT payloads, NVS keys, modem
  ownership, OTA polling suspension, and HTTP mutation endpoints are
  unchanged. New status objects and fields are additive.
- Event producers never perform SD I/O or wait for queue space. A dedicated
  low-priority task owns encoding/rotation/writes; journal failures and queue
  pressure are counted and do not stop telemetry, MQTT, WiFi, or HTTP.
- Event details are bounded and sensitive keys are redacted before both RAM
  and SD sinks. No raw telemetry, credentials, SIM identifiers, or raw AT
  responses were added.
- Automatic recovery actions remain explicitly disabled in status. The policy
  constants are observable, but Phase 2 is the first phase allowed to connect
  decisions to dry-run actions.
- No hardware was flashed, no OTA artifact was published, Anton was not
  contacted or mutated, the SD card was not modified, and no branch was
  pushed.

Deviations and remaining evidence:

- The approved activation boundary prevented real power-cut and live outage
  tests. Host tests inject partial JSONL writes, failed paths, rotations, and a
  complete zero-wall-time PPP/MQTT/spool outage timeline; actual FAT behavior,
  task/heap pressure, and event rates still require direct hardware evidence.
- The historical production database was not exported or committed. Baseline
  reproduction therefore uses approved aggregate counts plus one-way SD/schema
  hashes; the helper is ready to consume a separately handled JSONL production
  export when field validation is authorized.
- The field device remains on `7957a71`; `a2f5a10` and the Phase 0/1 commits
  are build-validated only.

### Phase 2: Coordinate AT windows and add automatic recovery

Deliverables:

- Implement the short-query AT scheduler and network-pause gate.
- Add MQTT health timestamps/counters.
- Implement the connectivity supervisor and recovery state in dry-run mode
  first, emitting the action it would take.
- Validate dry-run decisions with injected state transitions.
- Enable clean redial actions after dry-run evidence is correct.
- Enable supervised modem-reset escalation only after redial testing passes.
- Integrate the already implemented manual modem restart as a fallback.

Exit criteria:

- A healthy telemetry/PUBACK operation is never interrupted by an AT window.
- No AT window exceeds its configured maximum without an overrun event.
- Restored coverage results in MQTT plus a confirmed publish within the target
  recovery window.
- No-coverage conditions do not cause a modem-reset loop.
- A deliberately blackholed PPP path triggers redial and bounded escalation.
- OTA remains protected from polling and automatic restart.

Activation gate:

- Hardware testing requires explicit flash approval.
- OTA publication remains a separate approval after direct-flash validation.

### Phase 3: Repair the local WiFi control plane

Deliverables:

- Add secure field-mode AP+STA behavior.
- Add captive-portal discovery endpoints and local DNS redirect.
- Convert ping/raw-AT/restart actions to asynchronous jobs.
- Add AP association and HTTP health telemetry.
- Add browser request deadlines and visible errors.

Exit criteria:

- `http://192.168.4.1/` and `/api/status` respond within two seconds while:
  - cellular is unavailable;
  - PPP is redialing;
  - the modem is restarting;
  - MQTT is retrying;
  - a ping or AT job is running.
- The AP remains usable through each recovery transition.
- iOS and another client can discover or explicitly open the HTTP UI.
- The UI clearly distinguishes local-control health from cellular/MQTT health.

### Phase 4: Delivery identity and production deduplication

Backend-first deliverables:

- Extend the logger to accept legacy CSV and the schema-v2 envelope.
- Add nullable `delivery_boot_id` and `delivery_sequence` columns.
- Add a partial unique index for identified deliveries.
- Add duplicate-suppression metrics and tests.
- Preserve existing APIs and historical rows.

Firmware deliverables:

- Assign per-boot sequence numbers before delivery.
- Publish and spool the exact schema-v2 envelope.
- Keep the daily CSV human-readable and unchanged.
- Expose live/replay delivery counters by identity.

Exit criteria:

- Replaying the same envelope repeatedly creates one database row.
- Disconnect after broker receipt but before local PUBACK creates no duplicate.
- Legacy firmware continues ingesting.
- A mixed legacy/new-firmware test passes.
- Outage/reboot/replay reconciliation shows zero missing identified sequences.

Production activation sequence:

1. Take and validate a WAL-safe Anton backup.
2. Deploy the backward-compatible backend and partial unique index.
3. Validate legacy telemetry before changing firmware.
4. Flash one device with envelope firmware.
5. Run live, outage, replay, duplicate, and reboot tests.
6. Publish OTA only after the direct-flash test passes and approval is given.

### Phase 5: Availability and SD/time durability

Deliverables:

- Complete the asynchronous availability state machine.
- Validate retained online, Last Will offline, reconnect, and graceful offline.
- Reduce and measure the SD flush interval.
- Add explicit durable-row counters.
- Render unsynchronized telemetry correctly in the backend/dashboard.

Exit criteria:

- Anton records the expected availability lifecycle for each test boot.
- Retained replay does not create repeated availability transitions.
- Five abrupt power-cycle tests leave valid CSV, spool, cursor, and event files.
- Offline captured rows replay with complete delivery identities.
- Production never renders timestamp-zero telemetry as a 1969 sample.

### Phase 6: End-to-end field validation and rollout

Run a controlled route with:

- known good coverage;
- a forced broker block;
- a forced PPP blackhole if reproducible;
- a real low/no-coverage segment;
- WiFi STA loss and recovery;
- SoftAP UI use during cellular failure;
- at least one controlled ESP32 power cycle;
- a final home-WiFi backlog drain.

Collect:

- SD daily CSV, spool/cursor, and event journal;
- serial logs when available;
- broker connection history;
- production telemetry, status, and availability rows;
- firmware `/api/status` snapshots;
- exact test-action timestamps.

Final acceptance:

- zero missing identified telemetry sequences;
- zero duplicate production telemetry rows for identified deliveries;
- local UI reachable throughout cellular recovery;
- recovery actions match the documented policy;
- no reset loop during true lack of coverage;
- backlog drains automatically after connectivity returns;
- availability accurately follows broker sessions;
- event evidence explains every outage and recovery;
- Anton backup/restore and `PRAGMA quick_check` pass;
- all controlled test data is either retained as labeled evidence or removed
  intentionally.

## Test Matrix

| Scenario | Expected behavior | Required evidence |
| --- | --- | --- |
| Cellular registration lost | Continue SD/spool; wait with backoff; no reset loop | Event journal, spool growth |
| PPP has IP but broker path is dead | Detect stale MQTT/PUBACK, redial, then escalate if needed | Supervisor decisions, broker timeline |
| Coverage returns | MQTT and confirmed publish recover within target window | PUBACK and status event |
| AT poll during active telemetry | Poll defers; publish completes without false timeout | Pause-gate and PUBACK events |
| Ack lost after broker receipt | Replay carries same identity; backend suppresses duplicate | Duplicate-suppression counter |
| Abrupt power loss offline | CSV/spool remain valid; identified rows replay after boot | Card parse and DB reconciliation |
| Abrupt power loss online | At most configured CSV flush window absent locally; delivered data remains in DB | Card/DB comparison |
| WiFi STA unavailable | Field AP remains reachable | AP association and HTTP events |
| Modem restart | Local UI stays up; modem/GNSS/PPP/MQTT recover | Restart state timeline |
| Broker restart | Last Will/session state resolves correctly and telemetry reconnects | Availability table and broker logs |
| Timestamp unavailable | Preserve order/identity; UI says unsynchronized | Envelope and dashboard output |

## Commit and Documentation Strategy

Use focused commits and report their IDs. Suggested sequence:

1. `Document field reliability remediation plan`
2. `Add field diagnostic event journal`
3. `Expose connectivity health and recovery evidence`
4. `Coordinate modem polling with MQTT delivery`
5. `Add automatic cellular recovery supervisor`
6. `Keep the field WiFi control plane responsive`
7. Backend: `Accept identified telemetry envelopes`
8. Backend: `Deduplicate identified telemetry delivery`
9. Firmware: `Publish identified telemetry envelopes`
10. `Repair retained MQTT availability`
11. `Tighten SD flush and unsynchronized time handling`
12. `Document field validation results`

Update this plan after each phase with:

- commit IDs;
- validation performed;
- deviations and reasons;
- activation/deployment state;
- remaining risks.

Do not mark a phase complete based only on compilation or a status card.
Connectivity claims require broker and telemetry evidence; durability claims
require card-to-database reconciliation.

## Rollback Strategy

- Firmware: retain the last known-good OTA slot and rollback verification.
- Recovery supervisor: keep action enablement behind compile-time or NVS
  controls so dry-run/disabled operation is possible during testing.
- Field AP: preserve a way to return to the current SoftAP fallback behavior.
- Backend envelope ingestion: additive parsing and nullable columns allow
  legacy firmware to continue without rollback.
- SQLite uniqueness: rehearse against a restored backup; never delete
  historical duplicates as part of the ingestion migration.
- Anton: snapshot application files, take a WAL-safe database backup, and keep
  the prior images/configuration available before recreation.

## Deferred Work

The following are related but not required to complete this plan:

- migrating the commissioning backend from Flask/SQLite to the long-term
  Rails/PostgreSQL system;
- reconstructing inferred wall timestamps for pre-sync samples;
- remote command/control beyond diagnostics and explicitly safe maintenance;
- public exposure of the local or production dashboards;
- replacing the modem or adding a dedicated GNSS/diagnostic UART.
