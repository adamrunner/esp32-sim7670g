import re
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


def _code(source):
    return "\n".join(
        line.split("//", 1)[0] for line in source.splitlines()
    )


class ModemCleanupContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (REPOSITORY_ROOT / "main" / "modem.c").read_text()
        cls.header = (REPOSITORY_ROOT / "main" / "modem.h").read_text()
        cls.code = _code(cls.source)

    def assertCall(self, function, object_name, key, value):
        self.assertRegex(
            self.code,
            rf"{function}\s*\(\s*{object_name}\s*,\s*\"{key}\"\s*,"
            rf"\s*{re.escape(value)}\s*\)",
        )

    def test_inactive_explicit_redial_api_is_not_exposed_or_consumed(self):
        for symbol in (
            "modem_action_source_t",
            "modem_request_redial",
            "modem_request_redial_from",
            "s_redial_requested",
            "s_redial_source",
        ):
            self.assertNotIn(symbol, self.code)
            self.assertNotIn(symbol, self.header)

    def test_recovery_json_retains_compatibility_fields_and_types(self):
        self.assertCall(
            "cJSON_AddNumberToObject", "recovery", "redial_request_count", "0"
        )
        self.assertRegex(
            self.code,
            r"cJSON_AddObjectToObject\s*\(\s*recovery\s*,"
            r"\s*\"redial_sources\"\s*\)",
        )
        for source_name in ("manual", "ota_transport", "supervisor"):
            self.assertCall(
                "cJSON_AddNumberToObject", "redial_sources", source_name, "0"
            )
        self.assertCall(
            "cJSON_AddStringToObject", "redial_sources", "last", '"none"'
        )
        self.assertCall(
            "cJSON_AddNumberToObject",
            "recovery",
            "last_redial_request_uptime_ms",
            "0",
        )
        self.assertCall(
            "cJSON_AddBoolToObject",
            "recovery",
            "automatic_actions_enabled",
            "false",
        )
        self.assertCall(
            "cJSON_AddBoolToObject",
            "recovery",
            "automatic_supervisor_actions_enabled",
            "false",
        )

    def test_active_recovery_paths_remain_present(self):
        self.assertIn("if (was_up && !ppp_up)", self.code)
        self.assertIn("if (apn_dirty && st.at_ok)", self.code)
        self.assertIn("if (restart_requested)", self.code)
        self.assertIn("esp_err_t modem_request_restart(void)", self.code)
        self.assertIn("s_restart_requested = true", self.code)


if __name__ == "__main__":
    unittest.main()
