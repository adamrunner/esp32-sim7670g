import json
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY_ROOT / "tools"))

import event_report


def event(sequence, uptime_ms, component, name, wall_time=None, details=None):
    return {
        "schema_version": 1,
        "boot_id": "boot-a",
        "event_sequence": sequence,
        "uptime_ms": uptime_ms,
        "wall_time": wall_time,
        "time_source": "none" if wall_time is None else "sntp",
        "component": component,
        "event": name,
        "severity": "info",
        "reason": "synthetic_test",
        "details": details or {},
    }


class EventReportTests(unittest.TestCase):
    def test_zero_time_outage_is_reconstructed_by_boot_sequence(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            archived = [
                event(1, 0, "system", "boot", wall_time=None),
                event(2, 1000, "mqtt", "disconnected", wall_time=None),
                event(3, 1500, "spool", "appended", wall_time=None),
                event(4, 2000, "ppp", "down", wall_time=None),
            ]
            active = [
                event(5, 92000, "recovery", "redial_requested", wall_time=100),
                event(6, 95000, "ppp", "up", wall_time=103),
                event(7, 97000, "mqtt", "connected", wall_time=105),
                event(8, 100000, "spool", "drained", wall_time=108),
            ]
            (path / "events.1.jsonl").write_text(
                "".join(json.dumps(item) + "\n" for item in archived),
                encoding="utf-8",
            )
            (path / "events.jsonl").write_text(
                "".join(json.dumps(item) + "\n" for item in active),
                encoding="utf-8",
            )

            events = event_report.load_events(path)
            report = event_report.build_report(events)

            self.assertEqual(
                [item["event_sequence"] for item in events],
                list(range(1, 9)),
            )
            self.assertEqual(report["event_count"], 8)
            self.assertEqual(report["counters"]["spool.appended"], 1)
            self.assertEqual(report["counters"]["spool.drained"], 1)
            self.assertEqual(
                {
                    (outage["component"], outage["duration_ms"])
                    for outage in report["completed_outages"]
                },
                {("mqtt", 96000), ("ppp", 93000)},
            )
            self.assertEqual(report["open_outages"], {})

    def test_unredacted_sensitive_detail_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            unsafe = event(
                1,
                1,
                "wifi",
                "configured",
                details={"password": "not-redacted"},
            )
            (path / "events.jsonl").write_text(
                json.dumps(unsafe) + "\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(ValueError, "not redacted"):
                event_report.load_events(path)


if __name__ == "__main__":
    unittest.main()
