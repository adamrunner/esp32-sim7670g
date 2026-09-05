# Slop Audit Cleanup and Agent Handoff Plan

Status: local implementation complete; hardware acceptance remains pending.

Audit and plan date: 2026-09-04.
Audited source revision: `2ced12f62dbd1d37dce4e09c1d1a787b6d2c5db4` on `main`.
Implementation baseline: `50e0fa8256602a3bf777011884aaf0ba481c8c2a` on `main`.
Repository: `/Users/adamrunner/Code/esp32-sim7670g`.

## Objective and completion boundary

Remove demonstrably unused code, ineffective bookkeeping, duplicated state,
and tests that claim more than they verify. Fix the MQTT initialization
deadlock identified during the audit. Preserve working telemetry, modem,
OTA, storage, and local-control behavior except for the specific changes
listed below.

This document is a handoff specification, not an implementation record.
The user's request at creation was to prepare a detailed actionable plan.
Implementing agents should begin when assigned a work package by the user
or coordinating agent; this document does not itself activate work or authorize
deployment. All work packages start as pending.

Local completion means reviewed changes, meaningful host checks, a successful
pinned firmware build, focused local commits, and an explicit record of
remaining hardware acceptance. Installed and published firmware are separate
states; neither follows automatically from local completion.

## Source of truth and constraints

Read these before implementation:

- [Field reliability plan](FIELD_RELIABILITY_REMEDIATION_PLAN.md), especially
  the Phase 2 regression and rollback and passive OTA retry records.
- [Local WebUI stability plan](LOCAL_WEBUI_STABILITY_PLAN.md), especially
  bounded serialization, socket limits, and the latest Phase B evidence.
- [OTA reliability plan](OTA_RELIABILITY_PLAN.md).
- [Modem restart plan](MODEM_RESTART_PLAN.md).
- Any applicable `AGENTS.md` files and the ESP-IDF build skill before building.

The source revision above is the audit baseline. Old deployment statements in
those plans are dated evidence, not confirmation of today's hardware state.
In particular, the July supervisor runtime was withdrawn following stack and
heap failures. Do not restore it as part of cleanup. The August WebUI plan
still records a BMS-plus-SD load acceptance gap; this plan does not close it.

Preserve these boundaries:

1. Automatic supervisor redial/reset escalation stays disabled. The existing
   modem loop's recovery after real PPP loss, APN changes, and manual modem
   restart must continue working.
2. Keep passive OTA retries, poll suspension around OTA transport, SoftAP/HTTP
   quiet periods, and the browser's SoftAP manual-check restriction.
3. Preserve the 512-byte response chunking and fragment/event-at-a-time
   construction. Do not rebuild an entire status/event document on the heap.
4. Keep queues, bounded storage, locking, redaction, and the dedicated journal
   writer. Short functions that express these boundaries are not deletion
   targets merely because they are short.
5. Preserve HTTP routes, JSON keys/types, CSV ordering and values, MQTT topic
   names and payload schemas, NVS keys, and spool cursor format. The intended
   change to boot-ID correlation is specified in A2.
6. No flash, OTA publication, push, production deployment, SD-card mutation,
   or destructive watchdog/rollback/power-cut experiment is authorized by
   this plan. Do not run a combined build-and-flash example from the README.
7. Preserve unrelated work. At planning time `claudes.txt` was untracked;
   do not stage, delete, or rewrite it. Keep secrets and raw field data out of
   logs, fixtures, reports, and commits.
8. Do not introduce a framework, general-purpose dependency-injection layer,
   generic setter library, or repository-wide formatting change to do this
   cleanup. Every new abstraction needs an actual runtime or test use.

## Verified audit baseline

All 20 Python-discovered host tests passed on the audit revision. Two test
methods compile and run C harnesses with multiple cases, ten inspect source
text, six exercise the Python recovery proposal, and two exercise the event
report tool. These counts are not a coverage percentage.

An in-memory mutation removed `pollDashboard()` startup from the actual HTML;
the existing polling source-contract test still passed. No repository file
was changed by the probe. A separate model probe returned `request_redial`
at time zero for PPP-up/MQTT-disconnected/no-prior-PUBACK input.

No firmware build, hardware test, deployment check, or external consumer
inventory was performed in this audit. The findings below are grounded in
source and host-test evidence, with hardware limitations called out.

## Ownership and execution order

Use one coordinator and up to three implementation agents. Delegation is for
implementation after assignment; no implementation agents were launched to
write this plan. Prefer isolated `codex/` branches/worktrees. ESP-IDF component
paths are relative to this repository and require the sibling
`esp32-shared-components` checkout; verify that layout in each worktree.

| Owner | Packages | Exclusive source ownership | Suggested branch |
| --- | --- | --- | --- |
| Agent A: MQTT lifecycle | A1, A2, A3, sequential commits | `main/mqtt.c`, `main/mqtt.h`, `main/event_journal.c`, `main/event_journal.h`; new MQTT/boot host tests | `codex/slop-mqtt-lifecycle` |
| Agent B: test quality | B1, B2, sequential commits | `tests/test_field_safety_contracts.py`, `main/webui.c`, `main/www/index.html`; new WebUI tests | `codex/slop-test-quality` |
| Agent C: inactive code/storage | C1, C2, sequential commits | `main/modem.c`, `main/modem.h`, `main/datalog.c`, `main/datalog.h`; new storage tests | `codex/slop-inactive-code` |
| Coordinator | I0, D1, I1; optional D2 | This plan, README/test documentation, build wiring such as `main/CMakeLists.txt`, integration | `codex/slop-integration` |

Execution sequence:

1. **I0:** coordinator revalidates the baseline and assigns owners. Agree any
   new source/test filenames before parallel work starts.
2. **A1, B1, C1** may proceed independently. A1 has highest correctness priority.
3. Agent A proceeds A2 then A3; Agent B proceeds B2; Agent C proceeds C2.
   Each owner serializes its own changes and commits.
4. Coordinator performs D1, integrates commits, then runs I1 on the combined
   tree. D2 is optional and can be omitted without blocking core completion.

Only Agent B edits the existing source-contract suite; other agents send it
required updates. Only the coordinator edits shared build wiring and status
records. An owner needing a file in another lane requests a coordinated change
rather than editing it concurrently. Shared-directory agents must not switch
branches or stage another owner's files. Return commit IDs and reviewable
diffs; no remote push is needed for handoff.

## I0 — Establish the implementation baseline

1. Record current revision, branch, tracked diff, and untracked paths. If HEAD
   differs from the audit revision, revalidate findings against current code;
   do not reset to the old revision.
2. Run the host suite and record compiler/Python versions and failures.
3. Check local component dependencies and the pinned IDF environment. Capture
   an unmodified build/size baseline before source edits when practical. If
   unavailable, record that limitation rather than inventing size deltas.
4. Agree a test inventory: keep, replace, or explicitly retain as a static
   configuration check. Do not delete tests merely to keep the suite green.

Acceptance: baseline and ownership recorded; unrelated work preserved;
pre-existing failures distinguished from introduced failures.

## A1 — Fix MQTT error-path lock ownership (highest priority)

Evidence: `mqtt.c:set_last_error()` takes `s_mutex`;
`restart_client_locked()` calls it when `esp_mqtt_client_init()` returns NULL.
Both `mqtt_init()` and `mqtt_set_config()` call the restart helper while
holding the same nonrecursive mutex.

Implementation:

1. Trace every caller of `set_last_error()` and the restart helper, including
   event callbacks. Keep a clear distinction between locked and unlocked
   callers.
2. For the already-locked init-failure branch, assign the error directly while
   the existing lock is held. Retain the locking helper for callers that need
   it, or adopt an equally small explicit ownership arrangement.
3. Do not replace the mutex with a recursive mutex to hide ownership mistakes.
   Do not broaden this patch into a redesign of MQTT publishing.
4. Audit stop/destroy callback interactions while touching the restart path;
   record any additional lifecycle issue separately if fixing it requires
   broader changes.

Validation:

- Add a bounded host failure-injection check that executes the production
  restart/error code with client initialization returning NULL. A small fake
  mutex can reject recursive acquisition instead of hanging the test.
- Verify the outer caller can release the mutex, the error is observable,
  and a subsequent attempt can proceed. Cover the successful path as well.
- The regression test must fail against the old implementation. Do not copy
  the error-handling algorithm into a Python model or test-only C duplicate.
- Keep test seams narrow; no full fake ESP-IDF implementation is warranted.

Acceptance: no recursive acquisition on initialization failure; callback
callers remain synchronized; failure-injection regression passes.

Suggested commit: `Fix MQTT initialization error-path lock ownership`.

## A2 — Share one boot identity across journal and MQTT

Evidence: both `mqtt_init()` and `event_journal_init()` independently generate
16-character random IDs. `event_journal_boot_id()` currently has no caller.
`app_main()` initializes the journal before MQTT.

Implementation:

1. Prefer the existing journal boot-ID owner/getter over introducing another
   identity subsystem. Ensure the ID is generated before fallible journal
   mutex/queue setup, so journal initialization failure cannot give MQTT an
   empty ID. The value must remain immutable throughout this boot.
2. Use that ID for MQTT status, retained online availability, and offline
   last-will payloads. Remove MQTT's independent random-ID state/generation
   and imports that become unused.
3. Preserve the 16-character representation, all payload keys and schema
   versions, status sequence semantics, and event sequence semantics. Do not
   rewrite historical records or require a backend migration.
4. Document that new firmware now uses the same boot identity across these
   streams; old firmware can continue having unrelated values.

Validation:

- Execute the actual ID initialization/access and payload construction paths
  with deterministic randomness. Compare journal/status/availability/will IDs.
- Inject journal setup failure and verify MQTT still receives a valid ID.
- Reconfiguration within one boot must not change the ID; independent startup
  cases must initialize fresh IDs. Test only supported initialization order.
- Existing journal tests and MQTT error-path tests continue passing.

Acceptance: one ID per boot across all listed producers, including the
journal-unavailable path; no payload schema or identity-format change.

Suggested commit: `Share boot identity between MQTT and event journal`.

## A3 — End the one-shot MQTT time-status task

Evidence: `mqtt.c:status_task()` wakes every second indefinitely after its
single time-synchronized status has been successfully queued. The task is
created with a 3,072-byte stack.

Implementation:

1. Default to the smallest change: have the task finish with `vTaskDelete(NULL)`
   after successful enqueueing and updating the existing published flag.
2. Keep retrying while the clock is invalid, no connection has succeeded, or
   enqueueing fails. Preserve the current criterion: successfully queued,
   not a newly introduced PUBACK wait.
3. Move this work into `mqtt_maintenance_tick()` only if the owner can explain
   why that is preferable and verify its latency interaction with spool replay
   and availability. Do not add another task or timer.

Validation: deterministic cases for delayed time sync, disconnected/enqueue
failure, later success, and task exit only after success. Confirm reconnect
status handling remains independent. Do not assert that 3,072 bytes of live
free heap are reclaimed until measured on hardware; RTOS cleanup timing matters.

Acceptance: retries preserved, one successful time notification per boot,
and no perpetual task polling after that work is complete.

Suggested commit: `End MQTT time-status task after successful publication`.

## B1 — Replace browser source assertions with execution tests

Evidence: the polling contract passes even when dashboard startup is removed.
The suite checks identifiers and literal prose rather than scheduling or
control behavior.

Implementation:

1. Execute the actual embedded JavaScript using an available host JavaScript
   runtime, controlled fetch promises, a fake clock, and only the DOM surface
   the tested paths need. Do not implement a second polling loop in the test.
2. Preserve the embedded asset/deployment layout. Prefer testing the existing
   script; extract a small production-used unit only if doing so makes the
   tests simpler overall. No new UI framework or package ecosystem is needed.
3. Replace the corresponding string assertions only after their behavioral
   replacements pass. UI wording and helper names should not be contracts.

Required cases:

- Loading the script starts dashboard polling without an external manual call.
- Dashboard requests for status, WiFi, and OTA run sequentially. Distinguish
  this from the separate one-time MQTT configuration fetch.
- A slow dashboard request does not start overlapping dashboard cycles.
- Deadline expiry aborts a request, clears its in-flight state, and allows
  later polling to recover. A rejected fetch also allows recovery.
- The next cycle is scheduled after completion with the existing delay.
- In SoftAP state, submitting the OTA form sends no check POST; outside
  SoftAP the permitted action works and submission state is restored.

Mutation checks: locally remove polling startup, remove sequencing, and
disable timeout/SoftAP enforcement one at a time. Each relevant test must
fail. Restore mutations and never commit them. Use fake time, not slow sleeps.

Acceptance: behavior is exercised from shipped code; harmless naming/prose
changes do not fail the tests; the missing-startup audit mutation is caught.

Suggested commit: `Test dashboard scheduling and OTA controls by execution`.

## B2 — Make remaining safety checks honest and useful

Implementation:

1. Classify each remaining source-contract assertion in a short test guide.
   Configuration assertions can remain, explicitly named static checks.
   Parse exact configuration assignments so commented-out values do not count
   as enabled. Distinguish tracked defaults from optional local `sdkconfig`.
2. Keep the existing production-linked OTA-policy and event-journal C harnesses.
   Avoid replacing those useful boundaries with model-only tests.
3. For response serialization, execute the shipped writer against a collecting
   or failing output sink. Cover escaped/control characters, nested objects
   and arrays, numeric/null values, chunk boundaries, and output failures.
   Parse the result with an independent JSON parser and compare values/types.
4. Verify fragment/event-at-a-time lifetime using observable allocation or
   builder/visitor behavior. A search for a function name is not proof of
   bounded memory. Report any limit of host allocation measurement explicitly.
5. Keep any extraction narrowly scoped and production-used; send build-wiring
   changes to the coordinator. Do not replace the streaming design itself.
6. For hardware-only DHCP/session behavior, retain accurately labeled static
   guardrails where helpful and record the hardware acceptance requirement.
   Do not pretend a string check proves a working DHCP offer or socket close.

Acceptance: every retained test states what it actually verifies; key streaming
behavior is tested; source checks do not claim runtime coverage; no reduction
in bounded-memory safeguards.

Suggested commit: `Replace serialization source checks with behavior tests`.

## C1 — Remove inactive explicit-redial scaffolding

Evidence: `modem_request_redial()` has no callers; the sourced variant is
called only by that wrapper. Their flags, source enum, counters, and explicit
request-consumption branch therefore have no active producer.

Implementation:

1. Re-run reference searches across first-party code, tests, and build wiring.
   If a new real caller exists, stop treating that part as unused and report it.
2. Remove the unused declarations/functions, request flag/source state, and
   explicit request-consumption branch. Preserve real PPP-loss recovery,
   APN-dirty handling, and the separately used manual restart API/state machine.
3. Preserve published recovery JSON keys/types and disabled-action flags.
   Fields backed exclusively by removed counters can emit their existing
   zero/`none` values directly. Keep a concise comment explaining compatibility
   fields; do not keep a whole dormant action engine to supply zeros.
4. Keep policy-default status fields for compatibility, but identify them as
   proposed, inactive defaults in documentation. Do not wire in the Python model.
5. Notify Agent B of any source-contract changes; do not edit its file.

Validation: reference searches show no dangling symbols, public status output
retains its shape, and the firmware builds. Review the remaining modem loop
against the baseline for unchanged real-drop, APN-change, and manual-restart
branches. Hardware restoration tests remain outstanding until authorized.

Acceptance: no unused explicit-redial mechanism; existing recovery behavior
and external status compatibility retained.

Suggested commit: `Remove inactive explicit modem redial machinery`.

## C2 — Simplify spool cursor checkpoint bookkeeping

Evidence: `SPOOL_REPLAY_PER_TICK` is 2; `SPOOL_CURSOR_EVERY` is 16. Every batch
with acknowledged progress calls `spool_save_cursor()` at its end, resetting
the counter before it could reach 16.

Implementation:

1. Remove the ineffective 16-ack threshold and cross-tick unsaved-ack counter.
2. Track whether the current batch made acknowledged cursor progress using
   a local value. Save at the same end-of-batch point as today; retain the
   existing drained-spool reset behavior.
3. Preserve behavior for blank lines, failed PUBACKs, partial batches, and
   write errors. Do not silently adopt genuine 16-row batching: that would
   change the reboot replay/duplicate window.
4. Update the comment to describe actual checkpoint cadence. Do not change
   spool size limits, replay rate, CSV format, cursor format, or durability
   guarantees, and do not mix in a storage migration.

Validation: exercise actual replay code with temporary host files and controlled
publish outcomes. Check zero progress, one/two successful rows, success followed
by failure, blank-line advancement, drain/reset, and cursor-write failure.
Assert persisted offsets and which rows are retried; do not merely count calls
to the renamed helper. Add no tests for trivial symbol deletion alone.

Acceptance: cursor behavior matches baseline, including failure paths; the
unreachable batching threshold and persistent counter are gone.

Suggested commit: `Simplify spool cursor checkpoint bookkeeping`.

## D1 — Label recovery model tests as design work

Evidence: `tools/recovery_policy.py` explicitly describes a host-only proposal;
firmware does not call it. Its six tests are useful examples of intended policy,
not evidence that cellular recovery is implemented. The initial no-PUBACK case
currently requests redial immediately instead of waiting through a grace period.

Coordinator actions:

1. Keep the model and its tests for now: future recovery remains an existing
   planned feature. Avoid moving files purely for cosmetic organization.
2. In test documentation, separate firmware-execution tests, browser behavior
   tests, tooling tests, static checks, and recovery design-model tests.
3. Record the initial-grace-period discrepancy as a requirement for future
   Phase 2 policy work. Do not make new firmware decisions based on this model
   or expand this cleanup into implementing the supervisor.
4. State that a future implementation must exercise a production-used C policy
   with deterministic time and then pass hardware acceptance. Porting the
   Python tests verbatim is insufficient evidence of correctness.

Acceptance: reports cannot reasonably mistake model tests for implemented
recovery coverage; planned work and its known policy gap are retained.

## D2 — Optional small cleanup after core integration

These are low priority and are not core-completion blockers:

- Replace the one-member `check_req_t` wrapper with `ota_check_opts_t` if
  direct queue use is clearer and preserves zero/default initialization.
- Consolidate the identical WiFi state-name switches while keeping the two
  existing endpoint schemas. Do not merge endpoints or add a serializer framework.
- Recheck unused getters such as `modem_get_apn()` before removing them.
  A2 consumes `event_journal_boot_id()`, so do not delete that getter.

Use the current source owner or perform these after all lanes are integrated.
No new tests are needed just for deleting a trivial wrapper; run applicable
existing checks and the integrated build. Leave useful lock/snapshot helpers
and bounded serialization helpers intact.

## I1 — Integrated validation and review

### Host checks

Run from the repository root. The audit machine's default Apple developer-tool
selection was broken; these local overrides successfully ran the existing
suite without changing global configuration:

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools \
PYTHONDONTWRITEBYTECODE=1 \
/opt/homebrew/bin/python3 -m unittest discover -s tests -v
```

On another host, discover the available compiler/Python instead of copying
machine-specific paths blindly. Document one repeatable command for any added
JavaScript tests and connect it to the normal validation entry point. New tests
must not require access to a real device, broker, SD card, or production service.
Keep timeouts on concurrency/failure-injection checks so a regression cannot
hang the suite. Do not add tests that only assert the new implementation exists.

### Firmware build

Use the ESP-IDF build skill and the pinned Python 3.10 environment. A build-only
example, executed in a shell where the IDF export script is supported:

```sh
export IDF_PYTHON_ENV_PATH=/Users/adamrunner/.espressif/python_env/idf5.5_py3.10_env
source /Users/adamrunner/esp/v5.5/esp-idf/export.sh
idf.py build
idf.py size
```

Verify the sibling shared-component paths first. Do not upgrade dependencies,
change memory configuration, regenerate unrelated settings, or flash as a
convenience. If the existing build cache used an incompatible Python/IDF,
clean the generated build using the skill workflow and record why.

Review the combined diff for new heap allocations, task stacks, enlarged
buffers, HTTP schema drift, altered MQTT publication/sequence semantics, and
modem control changes. Report application-size changes only when a comparable
baseline exists. Host tests do not measure ESP32 stack high-water behavior.

### Later hardware acceptance, separately authorized

These remain pending after local integration; do not auto-run them:

| Change | Reversible acceptance evidence |
| --- | --- |
| A1/A3 MQTT lifecycle | Normal connect/reconfigure/reconnect; one time-sync event after delayed sync; task cleanup and heap observed. Use host fault injection for allocation failure. |
| A2 shared boot ID | Journal, HTTP event/status, broker status, and retained availability agree within one boot; a separately authorized reboot produces a new ID. No production record rewriting. |
| C1 modem cleanup | Ordinary PPP-loss restoration, authorized APN change/restoration, and manual restart still work; automatic supervisor flags stay false. |
| C2 spool cleanup | Controlled offline/online replay preserves offsets and rows on isolated test storage; live SD mutation requires separate authorization. |
| B1/B2 WebUI | SoftAP phone/browser use, deadline recovery, valid status/events, OTA deferral, and the existing full BMS+SD workload acceptance. |

Use the existing reliability plans' acceptance criteria and record build IDs,
duration, workload, errors, and resource evidence. Do not claim previous dated
soaks accept a newly changed image. No deliberate power-cut or destructive
watchdog/failed-OTA rollback test is part of this work.

### Completion checklist

- [ ] A1 regression fails on old code and passes on the fix.
- [ ] A2 IDs agree, including journal-setup failure.
- [ ] A3 retries remain intact and one-shot task finishes after success.
- [ ] B1 detects missing startup, overlap, lost timeout, and lost SoftAP guard.
- [ ] B2 executes real streaming code and labels static checks accurately.
- [ ] C1 removes dormant action code without removing active modem recovery.
- [ ] C2 preserves cursor persistence and retry behavior.
- [ ] D1 distinguishes design examples from firmware coverage.
- [ ] All host/browser checks and the integrated firmware build pass.
- [ ] Compatibility/resource review completed; optional D2 disposition recorded.
- [ ] Focused commits recorded; unrelated work preserved; nothing pushed or activated.
- [ ] Hardware acceptance gaps explicitly carried forward.

## Copyable agent assignments

### Agent A

Read `docs/SLOP_AUDIT_CLEANUP_PLAN.md` and its referenced reliability constraints.
Implement A1, A2, and A3 in order, with one focused commit per package. Own only
the MQTT/journal files and new tests listed in the ownership table. Start with
a production-code failure-injection regression for the nested mutex acquisition.
Preserve payload schemas and make boot identity survive journal setup failure.
Do not deploy, flash, publish, push, activate recovery, or modify other lanes.
Return commits, checks, evidence, size/resource implications, and hardware gaps.

### Agent B

Read `docs/SLOP_AUDIT_CLEANUP_PLAN.md` and implement B1 then B2. Own the existing
source-contract suite, WebUI source/embedded script, and new WebUI tests. Test
the shipped code with deterministic execution; do not create parallel models.
Keep useful static configuration checks honestly labeled. Preserve bounded
streaming, polling cadence, endpoint schemas, and SoftAP OTA behavior. Coordinate
build wiring and changes requested by other lanes. Return commits, replacement
test inventory, mutation-check results, and remaining hardware-only claims.

### Agent C

Read `docs/SLOP_AUDIT_CLEANUP_PLAN.md` and implement C1 then C2 in separate commits.
Recheck callers before deleting explicit-redial scaffolding. Preserve normal
modem recovery/manual restart and recovery JSON compatibility. Simplify cursor
bookkeeping while preserving per-batch checkpoint and retry semantics, verified
with actual replay code and temporary test files. Own only the modem/datalog
files and new tests listed in the ownership table. Do not touch a live SD card
or activate firmware. Return commits, checks, behavioral comparisons, and gaps.

## Handoff results and status log

Each owner returns: package ID; starting and ending revisions; changed files;
what changed and why; exact checks/results; regression/mutation evidence where
required; deviations; remaining concerns; and a statement of activation status.
If a finding is no longer true, return current caller/behavior evidence rather
than forcing the planned deletion. The coordinator updates this table once
per completed package; preserve existing historical reliability records.

### Local implementation record — 2026-09-04

- I0 revalidated the newer implementation baseline without resetting it. The
  tracked tree was clean and the pre-existing untracked `claudes.txt` was
  preserved. The original 20 host tests passed with Python 3.14.7, Node
  20.11.1, and the Command Line Tools compiler selected through
  `DEVELOPER_DIR`. The pinned ESP-IDF 5.5/Python 3.10.3 build passed before
  edits with a 1,398,705-byte image and 188,083 bytes of DIRAM use.
- The integrated suite passes all 29 Python-discovered tests. That includes the
  shipped browser script, production MQTT/journal paths, production JSON
  streaming, production spool replay functions, the retained OTA/journal
  harnesses, tooling checks, honestly labeled static guardrails, and the
  explicitly separate recovery design model.
- The A1 harness fails against the pre-fix MQTT implementation because it
  observes a recursive mutex acquisition, and passes against `0086d47`. The B1
  browser harness fails for each in-memory missing-startup, parallel-polling,
  missing-timeout, and missing-SoftAP-guard mutation.
- The final pinned build and `idf.py size` pass. Total image size is 1,398,345
  bytes, 360 bytes smaller than baseline; DIRAM use is 188,019 bytes, 64 bytes
  smaller. The response buffer remains 512 bytes. No new heap allocation,
  task, queue, timer, dependency, schema, route, MQTT topic, NVS key, CSV
  ordering, or spool format was introduced. The extracted JSON encoder adds a
  small callback/context wrapper to the existing HTTP request stack.
- D2 was intentionally omitted: its optional wrapper/switch/getter cleanups do
  not block the core objective and were not worth additional integration churn.
- No firmware was flashed, published, pushed, installed, or activated, and no
  live SD card or production service was mutated. The hardware acceptance table
  above remains fully pending, including the existing BMS-plus-SD workload gap.

| Package | Status | Commit(s) | Validation / remaining evidence |
| --- | --- | --- | --- |
| I0 | Complete | — | Baseline `50e0fa8`; 20 host tests and pinned build/size passed; `claudes.txt` preserved |
| A1 | Complete | `0086d47` | Old implementation triggers recursive-acquisition assertion; fixed failure and subsequent success paths pass |
| A2 | Complete | `b4702b3` | Journal/status/availability/will IDs agree; setup failure, reconfiguration, and fresh startup cases pass |
| A3 | Complete | `678603f` | Delayed time, disconnect/reconnect, enqueue failure, later success, and task exit pass |
| B1 | Complete | `ba692ac` | Shipped-script execution and all four mutation checks pass |
| B2 | Complete | `3f34dab` | Independent JSON parse, chunk/failure cases, and fragment/event allocation lifetime pass |
| C1 | Complete | `416b779` | Dormant redial mechanism removed; compatibility guardrails/build pass; hardware recovery pending |
| C2 | Complete | `12e1207` | Equivalent temporary-file replay cases pass; isolated hardware-storage replay pending |
| D1 | Complete | `713fa1f` | Test inventory separates production, browser, tooling, static, and design-model evidence |
| D2 | Omitted (optional) | — | Deferred to avoid nonessential cleanup churn |
| I1 | Complete locally | This record | 29 host/browser tests and pinned build/size pass; all hardware acceptance remains separate |
