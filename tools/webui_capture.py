#!/usr/bin/env python3
"""Snapshot the device's observability endpoints while joined to its SoftAP.

Writes one timestamped directory per capture holding the raw JSON bodies of
/api/status, /api/events, /api/wifi and /api/ota, then prints the Phase A
WebUI counters and any warn/error events so a field capture can be read
without opening the files.

The counters in /api/status.http and the 48-slot event ring live in RAM and
are cleared by a reboot; only the SD journal (events.jsonl and its rotated
archives, read with tools/event_report.py) spans boots.
"""

from __future__ import annotations

import argparse
import http.client
import json
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ENDPOINTS = {
    "status": "/api/status",
    "events": "/api/events?limit=48",
    "wifi": "/api/wifi",
    "ota": "/api/ota",
}


def fetch(host: str, path: str, timeout: float) -> bytes:
    connection = http.client.HTTPConnection(host, timeout=timeout)
    try:
        connection.request("GET", path, headers={"Connection": "close"})
        response = connection.getresponse()
        body = response.read()
        if response.status != 200:
            raise RuntimeError(f"HTTP {response.status}")
        return body
    finally:
        connection.close()


def capture(host: str, timeout: float, retries: int) -> dict[str, bytes]:
    """Fetch every endpoint sequentially, matching the browser poll profile."""
    bodies: dict[str, bytes] = {}
    for name, path in ENDPOINTS.items():
        for attempt in range(1, retries + 1):
            try:
                bodies[name] = fetch(host, path, timeout)
                break
            except Exception as error:  # noqa: BLE001 - report, keep going
                if attempt == retries:
                    print(f"{name}: failed after {retries} attempts: {error}",
                          file=sys.stderr)
                else:
                    time.sleep(1.0)
    return bodies


def summarize(bodies: dict[str, bytes]) -> None:
    status_raw = bodies.get("status")
    if status_raw:
        status = json.loads(status_raw)
        http_status = status.get("http", {})
        print("\n/api/status.http (RAM, since last boot):")
        print(json.dumps(http_status, indent=2, sort_keys=True))

        journal = status.get("event_journal", {})
        if journal:
            print("\nevent journal:")
            print(json.dumps(journal, indent=2, sort_keys=True))

    events_raw = bodies.get("events")
    if events_raw:
        events = json.loads(events_raw).get("events", [])
        print(f"\nring events: {len(events)}")
        for event in events:
            severity = event.get("severity", "")
            if severity in ("warn", "error", "critical"):
                details = event.get("details", {})
                print(f"  [{severity}] {event.get('component')}/"
                      f"{event.get('event')} @{event.get('uptime_ms')}ms "
                      f"reason={event.get('reason')} "
                      f"{json.dumps(details, sort_keys=True)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.4.1",
                        help="device address on the SoftAP")
    parser.add_argument("--output", type=Path, default=Path("captures"),
                        help="directory to hold timestamped captures")
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="per-request timeout in seconds")
    parser.add_argument("--retries", type=int, default=3,
                        help="attempts per endpoint before giving up")
    args = parser.parse_args()

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    destination = args.output / stamp
    destination.mkdir(parents=True, exist_ok=True)

    bodies = capture(args.host, args.timeout, args.retries)
    for name, body in bodies.items():
        (destination / f"{name}.json").write_bytes(body)

    print(f"captured {len(bodies)}/{len(ENDPOINTS)} endpoints to {destination}")
    summarize(bodies)
    return 0 if len(bodies) == len(ENDPOINTS) else 1


if __name__ == "__main__":
    sys.exit(main())
