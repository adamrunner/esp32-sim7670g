# Host Test Inventory

Run the complete host suite from the repository root:

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools \
PYTHONDONTWRITEBYTECODE=1 \
/opt/homebrew/bin/python3 -m unittest discover -s tests -v
```

The `DEVELOPER_DIR` override is only needed on hosts whose selected Apple
developer directory cannot find a working compiler. Use an available Python
and C compiler elsewhere. The Python discovery command includes the browser
tests through `test_webui_browser.py`; the browser harness can also be run
directly with `node tests/webui_browser_test.js`.

## What each group verifies

- **Firmware production-code harnesses:** `test_event_journal_core.py`,
  `test_ota_policy_core.py`, `test_mqtt_lifecycle.py`,
  `test_datalog_spool.py`, and `test_webui_json_stream.py` compile and execute
  production C or narrowly extracted production functions with controlled host
  dependencies. They cover the named deterministic behavior, not ESP32 task
  scheduling, stack high-water marks, real networking, flash, or SD hardware.
- **Browser behavior:** `test_webui_browser.py` runs the JavaScript embedded in
  `main/www/index.html` with controlled fetch promises, a fake clock, and a
  minimal DOM. It verifies polling startup and sequencing, timeout and rejection
  recovery, non-overlap, scheduling cadence, and the SoftAP manual-OTA guard.
  It is not a browser rendering, RF, DHCP, or socket-capacity test.
- **Tooling behavior:** `test_event_report.py` exercises the host event-report
  tool with synthetic, redacted records. It does not exercise firmware.
- **Static configuration and source guardrails:**
  `test_field_safety_contracts.py` checks exact tracked configuration values and
  selected wiring/disabled-action boundaries. `test_modem_cleanup.py` checks
  compatibility fields and that the intended active recovery branches remain
  present. These checks do not prove working DHCP offers, socket closure,
  modem recovery, resource headroom, or other runtime behavior.
- **Recovery design model:** `test_recovery_policy.py` exercises the host-only
  proposal in `tools/recovery_policy.py`. Firmware does not call this model, so
  these examples are not evidence that automatic recovery is implemented or
  enabled. The current proposal also requests a redial at time zero when PPP is
  up, MQTT is disconnected, and there is no prior PUBACK; future Phase 2 work
  must define and test an initial grace period instead.

A future automatic-recovery implementation must exercise a production-used C
policy with deterministic time and then pass separately authorized hardware
acceptance. Porting the Python model tests verbatim is not sufficient. Current
hardware-only gaps include ordinary PPP-loss/APN/manual-restart restoration,
isolated SD spool replay, SoftAP browser behavior, MQTT lifecycle/resource
observation, shared boot-ID observation across HTTP/broker streams, and the
existing BMS-plus-SD workload acceptance.
