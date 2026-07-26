# OTA Reliability and Failure Traceability Plan

## Context

Firmware `dc64501` successfully discovered release `e4f4794`, but a later
manual check failed before receiving the manifest:

```json
{
  "state": "error",
  "error": "check failed: connect failed: ESP_ERR_HTTP_CONNECT",
  "bytes_read": 0,
  "image_size": 0,
  "free_heap": 49732,
  "largest_free_block": 31744,
  "minimum_free_heap": 32
}
```

Rebooting the ESP32 restored the update path; the device then fetched,
installed, verified, and booted `e4f4794`. This proves the published artifact,
manifest, OTA partitions, and rollback verification path work. The remaining
problem is resilience to transient runtime transport or memory state.

The existing downloader already retries four times and requests a clean PPP
redial between failed download attempts. Manifest discovery has only one
connection attempt, however, so it can fail before reaching that recovery
logic. It also collapses the ESP-TLS, mbedTLS, socket, and heap evidence into
the generic `ESP_ERR_HTTP_CONNECT`.

## Goals

1. Recover from transient manifest connection failures without requiring a
   power cycle or manual reboot.
2. Make an OTA failure diagnosable from `/api/ota` even when serial logs are
   unavailable.
3. Retry a failed manual check automatically after the same short interval as
   a failed scheduled check.
4. Reduce avoidable TLS heap residency without reducing protocol compatibility.
5. Preserve the existing manifest contract, resumable download behavior,
   SHA-256 verification, rollback protection, and release workflow.

## Implementation

### 1. Structured connection diagnostics

- Capture the top-level ESP-IDF error, ESP-TLS error, mbedTLS error, certificate
  flags, socket `errno`, failure stage, and heap state before the HTTP client is
  destroyed.
- Keep the human-readable `error` field for the UI and compatibility.
- Add a structured `failure` object to `/api/ota`, plus manifest/download
  attempt counters and the next scheduled check delay.
- Clear stale failure details when a new check begins or an operation succeeds.

### 2. Manifest recovery

- Give manifest discovery a bounded attempt loop.
- After the first connection failure, use the existing transport recovery path:
  wait on Wi-Fi when it is the active route, otherwise request a clean PPP
  redial and wait for it to return.
- Keep modem status/GNSS polling suspended for the whole check/update cycle so
  AT command windows cannot interrupt either HTTPS connection.
- Record each attempt and stop immediately on non-transport errors such as a
  malformed manifest or non-200 response; those will not improve after redial.

### 3. Retry scheduling

- Schedule any failed check/update for the five-minute retry interval,
  regardless of whether it was automatic or manually requested.
- Continue allowing a manual request to wake the OTA task immediately.
- Expose the remaining delay as `next_check_in_s`.

### 4. TLS memory posture

- Enable ESP-IDF's dynamic TLS TX/RX buffers so idle TLS connections do not
  retain buffers unnecessarily.
- Keep the existing 16 KB incoming and 4 KB outgoing content limits. Reducing
  the incoming limit could reject a valid full-size TLS record from the update
  server.
- Retain heap snapshots at failure time so a subsequent field report can show
  whether peak allocation or fragmentation remains a problem.

## Validation

1. Run host-side checks for retry classification, retry scheduling, and JSON/UI
   syntax where practical.
2. Run `git diff --check`.
3. Build the complete ESP-IDF firmware using the pinned IDF 5.5 Python 3.10
   release environment.
4. Inspect the generated configuration to confirm dynamic TLS buffers are
   enabled and the TLS content limits are unchanged.
5. Do not publish or activate a production OTA release until separately
   requested.

## Follow-up

Add a web UI reboot control in a separate focused change. It can use
`esp_restart()` after an authenticated/local POST endpoint acknowledges the
request and delays briefly so the HTTP response reaches the browser.
