#!/usr/bin/env python3
"""Synthetic-time reference model for the approved connectivity policy.

This host-only model makes Phase 2 decisions testable before any action is
connected to the modem owner. Firmware behavior must remain disabled until the
runtime supervisor is implemented and its decisions match these tests.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


MQTT_UNHEALTHY_SECONDS = 90
RECOVERY_ATTEMPT_SECONDS = 90
FAILED_REDIALS_BEFORE_RESTART = 2
MODEM_RESTART_COOLDOWN_SECONDS = 15 * 60
STABLE_RECOVERY_SECONDS = 5 * 60


class Decision(str, Enum):
    HEALTHY = "healthy"
    WIFI_UPLINK_HEALTHY = "wifi_uplink_healthy"
    WAITING_FOR_COVERAGE = "waiting_for_coverage"
    WAITING_FOR_MQTT = "waiting_for_mqtt"
    REQUEST_REDIAL = "request_redial"
    WAITING_FOR_RECOVERY = "waiting_for_recovery"
    REQUEST_MODEM_RESTART = "request_modem_restart"
    RESTART_COOLDOWN = "restart_cooldown"


@dataclass(frozen=True)
class ConnectivitySnapshot:
    wifi_working: bool
    registered: bool
    packet_attached: bool
    ppp_up: bool
    mqtt_connected: bool
    last_puback_at: int | None
    modem_responsive: bool = True


class RecoveryPolicy:
    def __init__(self) -> None:
        self.failed_redials = 0
        self.recovery_deadline: int | None = None
        self.last_restart_at: int | None = None
        self.stable_since: int | None = None

    def note_redial_result(self, *, now: int, recovered: bool) -> None:
        self.recovery_deadline = None
        if recovered:
            self.stable_since = now
        else:
            self.failed_redials += 1

    def note_modem_restart(self, *, now: int) -> None:
        self.last_restart_at = now
        self.recovery_deadline = now + RECOVERY_ATTEMPT_SECONDS

    def _mqtt_healthy(self, now: int, state: ConnectivitySnapshot) -> bool:
        return (
            state.mqtt_connected
            and state.last_puback_at is not None
            and now - state.last_puback_at < MQTT_UNHEALTHY_SECONDS
        )

    def evaluate(self, *, now: int, state: ConnectivitySnapshot) -> Decision:
        mqtt_healthy = self._mqtt_healthy(now, state)
        if mqtt_healthy:
            if self.stable_since is None:
                self.stable_since = now
            if now - self.stable_since >= STABLE_RECOVERY_SECONDS:
                self.failed_redials = 0
            return Decision.HEALTHY
        self.stable_since = None

        if state.wifi_working:
            return Decision.WIFI_UPLINK_HEALTHY
        if not state.registered or not state.packet_attached:
            self.recovery_deadline = None
            return Decision.WAITING_FOR_COVERAGE
        if not state.ppp_up:
            return Decision.WAITING_FOR_MQTT
        if (
            state.last_puback_at is not None
            and now - state.last_puback_at < MQTT_UNHEALTHY_SECONDS
        ):
            return Decision.WAITING_FOR_MQTT
        if self.recovery_deadline is not None and now < self.recovery_deadline:
            return Decision.WAITING_FOR_RECOVERY

        if (
            self.failed_redials >= FAILED_REDIALS_BEFORE_RESTART
            and state.modem_responsive
        ):
            if (
                self.last_restart_at is not None
                and now - self.last_restart_at < MODEM_RESTART_COOLDOWN_SECONDS
            ):
                return Decision.RESTART_COOLDOWN
            return Decision.REQUEST_MODEM_RESTART

        self.recovery_deadline = now + RECOVERY_ATTEMPT_SECONDS
        return Decision.REQUEST_REDIAL
