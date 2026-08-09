#!/usr/bin/env python3
"""Bench soak test for the local WebUI control plane.

Reproduces the browser's polling profile against a device (by default the
SoftAP address) and optionally adds the two client behaviors suspected of
destabilizing the field UI:

- ``--churn``: abort requests mid-flight the way a browser's fetch deadline
  does, leaving the server to discover the dead socket on its own;
- ``--slow-reader``: hold a session and read the response a few bytes at a
  time, the way a phone at the RF edge (or in WiFi power save) drains it.

The poll loop mirrors main/www/index.html: one sequential cycle of
``/api/status``, ``/api/wifi``, ``/api/ota`` every POLL_CYCLE_S seconds, each
request abandoned after DEADLINE_S. Results are judged against the Phase A
acceptance bar from docs/LOCAL_WEBUI_STABILITY_PLAN.md and the script exits
nonzero on failure, so a soak can gate later phases mechanically.

Read-only: only GET endpoints are used; nothing is configured or rebooted.

Example (30-minute acceptance run with both chaos clients):

    python3 tools/webui_soak.py --minutes 30 --churn --slow-reader
"""

import argparse
import http.client
import json
import socket
import sys
import threading
import time

POLL_ENDPOINTS = ("/api/status", "/api/wifi", "/api/ota")
POLL_CYCLE_S = 2.0     # matches POLL_CYCLE_DELAY_MS in index.html
DEADLINE_S = 2.5       # matches POLL_REQUEST_TIMEOUT_MS in index.html

ACCEPT_SUCCESS_RATE = 0.99
ACCEPT_P95_MS = 1000.0


class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.latencies_ms = []
        self.failures = []  # (uptime_s, endpoint, reason)
        self.started = time.monotonic()

    def record(self, endpoint, latency_ms, error=None):
        with self.lock:
            if error is None:
                self.latencies_ms.append(latency_ms)
            else:
                self.failures.append(
                    (time.monotonic() - self.started, endpoint, error)
                )

    def percentile(self, fraction):
        with self.lock:
            if not self.latencies_ms:
                return None
            ordered = sorted(self.latencies_ms)
        index = min(len(ordered) - 1, int(len(ordered) * fraction))
        return ordered[index]

    def summary(self):
        with self.lock:
            successes = len(self.latencies_ms)
            failures = len(self.failures)
        total = successes + failures
        return {
            "requests": total,
            "successes": successes,
            "failures": failures,
            "success_rate": successes / total if total else 0.0,
            "p50_ms": self.percentile(0.50),
            "p95_ms": self.percentile(0.95),
            "max_ms": max(self.latencies_ms) if successes else None,
        }


def fetch_json(host, path, deadline_s):
    connection = http.client.HTTPConnection(host, timeout=deadline_s)
    try:
        connection.request("GET", path, headers={"Connection": "close"})
        response = connection.getresponse()
        body = response.read()
        if response.status != 200:
            raise RuntimeError(f"HTTP {response.status}")
        return json.loads(body)
    finally:
        connection.close()


def poll_worker(host, stats, stop, label):
    """One browser-equivalent poll loop (sequential, fixed cadence)."""
    while not stop.is_set():
        cycle_started = time.monotonic()
        for endpoint in POLL_ENDPOINTS:
            if stop.is_set():
                return
            request_started = time.monotonic()
            try:
                fetch_json(host, endpoint, DEADLINE_S)
                latency_ms = (time.monotonic() - request_started) * 1000
                stats.record(endpoint, latency_ms)
            except Exception as error:  # noqa: BLE001 - every failure counts
                stats.record(endpoint, None, f"{label}: {error}")
        elapsed = time.monotonic() - cycle_started
        stop.wait(max(0.0, POLL_CYCLE_S - elapsed))


def churn_worker(host, port, stop, interval_s):
    """Abort a request mid-flight, like a browser hitting its fetch deadline.

    SO_LINGER 0 sends RST on close, which is the harshest form the server
    must tolerate: the socket dies with response bytes still in flight.
    """
    while not stop.wait(interval_s):
        try:
            raw = socket.create_connection((host, port), timeout=DEADLINE_S)
            raw.setsockopt(
                socket.SOL_SOCKET,
                socket.SO_LINGER,
                b"\x01\x00\x00\x00\x00\x00\x00\x00",
            )
            raw.sendall(
                b"GET /api/status HTTP/1.1\r\n"
                b"Host: device\r\n\r\n"
            )
            raw.recv(64)  # let the response start, then abandon it
            raw.close()
        except OSError:
            pass  # connection refused/reset is a result, not a script error


def slow_reader_worker(host, port, stop, interval_s, hold_s):
    """Occupy a session like an RF-degraded client: read a trickle."""
    while not stop.wait(interval_s):
        try:
            raw = socket.create_connection((host, port), timeout=hold_s)
            raw.sendall(
                b"GET /api/status HTTP/1.1\r\n"
                b"Host: device\r\n\r\n"
            )
            deadline = time.monotonic() + hold_s
            while time.monotonic() < deadline and not stop.is_set():
                raw.settimeout(1.0)
                try:
                    if not raw.recv(16):
                        break
                except socket.timeout:
                    pass
                time.sleep(0.2)
            raw.close()
        except OSError:
            pass


def print_http_observability(host):
    try:
        status = fetch_json(host, "/api/status", 10)
    except Exception as error:  # noqa: BLE001
        print(f"could not fetch final /api/status: {error}")
        return
    http_status = status.get("http", {})
    print("\ndevice /api/status.http after soak:")
    print(json.dumps(http_status, indent=2, sort_keys=True))


def main():
    parser = argparse.ArgumentParser(
        description="Soak the local WebUI with a browser-profile load"
    )
    parser.add_argument("--host", default="192.168.4.1",
                        help="device address (default: SoftAP 192.168.4.1)")
    parser.add_argument("--minutes", type=float, default=30.0,
                        help="soak duration (default 30)")
    parser.add_argument("--clients", type=int, default=2,
                        help="parallel poll loops, e.g. phone + laptop "
                             "(default 2)")
    parser.add_argument("--churn", action="store_true",
                        help="periodically abort requests mid-flight")
    parser.add_argument("--churn-interval", type=float, default=20.0,
                        help="seconds between aborted requests (default 20)")
    parser.add_argument("--slow-reader", action="store_true",
                        help="periodically hold a session reading slowly")
    parser.add_argument("--slow-interval", type=float, default=45.0,
                        help="seconds between slow-reader sessions "
                             "(default 45)")
    parser.add_argument("--slow-hold", type=float, default=15.0,
                        help="seconds each slow-reader session lasts "
                             "(default 15)")
    args = parser.parse_args()

    stats = Stats()
    stop = threading.Event()
    workers = []
    for client in range(args.clients):
        workers.append(threading.Thread(
            target=poll_worker,
            args=(args.host, stats, stop, f"poll{client}"),
            daemon=True,
        ))
    if args.churn:
        workers.append(threading.Thread(
            target=churn_worker,
            args=(args.host, 80, stop, args.churn_interval),
            daemon=True,
        ))
    if args.slow_reader:
        workers.append(threading.Thread(
            target=slow_reader_worker,
            args=(args.host, 80, stop, args.slow_interval, args.slow_hold),
            daemon=True,
        ))

    print(f"soaking http://{args.host}/ for {args.minutes:g} min: "
          f"{args.clients} poll client(s)"
          f"{', churn' if args.churn else ''}"
          f"{', slow-reader' if args.slow_reader else ''}")
    for worker in workers:
        worker.start()
    try:
        deadline = time.monotonic() + args.minutes * 60
        while time.monotonic() < deadline:
            time.sleep(10)
            live = stats.summary()
            print(f"  {live['requests']} requests, "
                  f"{live['failures']} failed, "
                  f"p95 {live['p95_ms'] and round(live['p95_ms'])} ms",
                  flush=True)
    except KeyboardInterrupt:
        print("interrupted — reporting on partial run")
    stop.set()
    for worker in workers:
        worker.join(timeout=DEADLINE_S + 1)

    result = stats.summary()
    print("\nsoak result:")
    print(json.dumps(result, indent=2))
    if result["failures"]:
        print("\nfirst failures:")
        with stats.lock:
            for uptime_s, endpoint, reason in stats.failures[:10]:
                print(f"  t+{uptime_s:7.1f}s {endpoint}: {reason}")

    print_http_observability(args.host)

    passed = (
        result["requests"] > 0
        and result["success_rate"] >= ACCEPT_SUCCESS_RATE
        and result["p95_ms"] is not None
        and result["p95_ms"] < ACCEPT_P95_MS
    )
    print(f"\nacceptance (≥{ACCEPT_SUCCESS_RATE:.0%} success, "
          f"p95 < {ACCEPT_P95_MS:g} ms): {'PASS' if passed else 'FAIL'}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
