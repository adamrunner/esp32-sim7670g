#!/usr/bin/env python3
"""Build a machine-readable outage timeline from an SD event journal."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Iterable


SENSITIVE_KEYS = {
    "password",
    "passwd",
    "passphrase",
    "psk",
    "secret",
    "token",
    "authorization",
    "username",
    "mqtt_uri",
    "broker_uri",
}


def _journal_paths(directory: Path) -> list[Path]:
    archives = []
    for path in directory.glob("events.*.jsonl"):
        try:
            generation = int(path.name.split(".")[1])
        except (IndexError, ValueError):
            continue
        archives.append((generation, path))
    paths = [path for _, path in sorted(archives, reverse=True)]
    active = directory / "events.jsonl"
    if active.exists():
        paths.append(active)
    return paths


def _assert_redacted(value, path: str = "event") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            if key.lower() in SENSITIVE_KEYS and child != "[redacted]":
                raise ValueError(f"{path}.{key} is not redacted")
            _assert_redacted(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _assert_redacted(child, f"{path}[{index}]")


def load_events(directory: Path) -> list[dict]:
    events = []
    last_sequence_by_boot: dict[str, int] = {}
    for path in _journal_paths(directory):
        with path.open("r", encoding="utf-8") as handle:
            for line_number, line in enumerate(handle, 1):
                if not line.strip():
                    continue
                try:
                    event = json.loads(line)
                except json.JSONDecodeError as exc:
                    raise ValueError(
                        f"{path}:{line_number}: invalid JSONL record"
                    ) from exc
                if event.get("schema_version") != 1:
                    raise ValueError(
                        f"{path}:{line_number}: unsupported event schema"
                    )
                boot_id = event.get("boot_id")
                sequence = event.get("event_sequence")
                if not isinstance(boot_id, str) or type(sequence) is not int:
                    raise ValueError(
                        f"{path}:{line_number}: missing boot order identity"
                    )
                previous = last_sequence_by_boot.get(boot_id)
                if previous is not None and sequence <= previous:
                    raise ValueError(
                        f"{path}:{line_number}: non-monotonic event sequence"
                    )
                last_sequence_by_boot[boot_id] = sequence
                _assert_redacted(event)
                events.append(event)
    return events


def build_report(events: Iterable[dict]) -> dict:
    ordered = list(events)
    transitions = []
    open_outages: dict[str, dict] = {}
    completed_outages = []
    counters: dict[str, int] = {}

    for event in ordered:
        component = event["component"]
        name = event["event"]
        key = f"{component}.{name}"
        counters[key] = counters.get(key, 0) + 1
        transitions.append(
            {
                "boot_id": event["boot_id"],
                "event_sequence": event["event_sequence"],
                "uptime_ms": event["uptime_ms"],
                "wall_time": event.get("wall_time"),
                "event": key,
                "reason": event.get("reason", ""),
            }
        )

        if key in {"ppp.down", "mqtt.disconnected"}:
            open_outages.setdefault(component, transitions[-1])
        elif key in {"ppp.up", "mqtt.connected"} and component in open_outages:
            started = open_outages.pop(component)
            completed_outages.append(
                {
                    "component": component,
                    "boot_id": event["boot_id"],
                    "start_sequence": started["event_sequence"],
                    "end_sequence": event["event_sequence"],
                    "duration_ms": event["uptime_ms"] - started["uptime_ms"],
                }
            )

    return {
        "schema_version": 1,
        "event_count": len(ordered),
        "boot_count": len({event["boot_id"] for event in ordered}),
        "counters": counters,
        "completed_outages": completed_outages,
        "open_outages": open_outages,
        "transitions": transitions,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("events_directory", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        report = build_report(load_events(args.events_directory))
    except (OSError, ValueError) as exc:
        print(f"event_report: {exc}", file=sys.stderr)
        return 2
    output = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(output, encoding="utf-8")
    else:
        sys.stdout.write(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
