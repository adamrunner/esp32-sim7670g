import sys
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY_ROOT / "tools"))

from recovery_policy import (
    ConnectivitySnapshot,
    Decision,
    MODEM_RESTART_COOLDOWN_SECONDS,
    RecoveryPolicy,
    STABLE_RECOVERY_SECONDS,
)


def snapshot(**overrides) -> ConnectivitySnapshot:
    values = {
        "wifi_working": False,
        "registered": True,
        "packet_attached": True,
        "ppp_up": True,
        "mqtt_connected": False,
        "last_puback_at": None,
        "modem_responsive": True,
    }
    values.update(overrides)
    return ConnectivitySnapshot(**values)


class RecoveryPolicyTests(unittest.TestCase):
    def test_no_coverage_waits_without_resetting_modem(self):
        policy = RecoveryPolicy()
        state = snapshot(registered=False, packet_attached=False, ppp_up=False)
        self.assertEqual(
            policy.evaluate(now=1000, state=state),
            Decision.WAITING_FOR_COVERAGE,
        )
        self.assertEqual(policy.failed_redials, 0)

    def test_dead_ppp_path_requests_one_redial_after_stale_puback(self):
        policy = RecoveryPolicy()
        state = snapshot(mqtt_connected=True, last_puback_at=800)
        self.assertEqual(
            policy.evaluate(now=1000, state=state), Decision.REQUEST_REDIAL
        )
        self.assertEqual(
            policy.evaluate(now=1010, state=state),
            Decision.WAITING_FOR_RECOVERY,
        )

    def test_healthy_wifi_suppresses_cellular_recovery(self):
        policy = RecoveryPolicy()
        self.assertEqual(
            policy.evaluate(now=1000, state=snapshot(wifi_working=True)),
            Decision.WIFI_UPLINK_HEALTHY,
        )

    def test_two_failed_redials_escalate_to_modem_restart(self):
        policy = RecoveryPolicy()
        state = snapshot()
        self.assertEqual(
            policy.evaluate(now=1000, state=state), Decision.REQUEST_REDIAL
        )
        policy.note_redial_result(now=1090, recovered=False)
        self.assertEqual(
            policy.evaluate(now=1090, state=state), Decision.REQUEST_REDIAL
        )
        policy.note_redial_result(now=1180, recovered=False)
        self.assertEqual(
            policy.evaluate(now=1180, state=state),
            Decision.REQUEST_MODEM_RESTART,
        )

    def test_modem_restart_cooldown_blocks_restart_loop(self):
        policy = RecoveryPolicy()
        policy.failed_redials = 2
        policy.last_restart_at = 1000
        self.assertEqual(
            policy.evaluate(
                now=1000 + MODEM_RESTART_COOLDOWN_SECONDS - 1,
                state=snapshot(),
            ),
            Decision.RESTART_COOLDOWN,
        )
        self.assertEqual(
            policy.evaluate(
                now=1000 + MODEM_RESTART_COOLDOWN_SECONDS,
                state=snapshot(),
            ),
            Decision.REQUEST_MODEM_RESTART,
        )

    def test_stable_pubacks_reset_recovery_failures_only_after_five_minutes(self):
        policy = RecoveryPolicy()
        policy.failed_redials = 2
        healthy = snapshot(mqtt_connected=True, last_puback_at=1000)
        self.assertEqual(policy.evaluate(now=1000, state=healthy), Decision.HEALTHY)
        self.assertEqual(policy.failed_redials, 2)

        still_healthy = snapshot(
            mqtt_connected=True,
            last_puback_at=1000 + STABLE_RECOVERY_SECONDS,
        )
        self.assertEqual(
            policy.evaluate(
                now=1000 + STABLE_RECOVERY_SECONDS,
                state=still_healthy,
            ),
            Decision.HEALTHY,
        )
        self.assertEqual(policy.failed_redials, 0)


if __name__ == "__main__":
    unittest.main()
