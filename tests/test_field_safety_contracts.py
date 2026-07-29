import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


class FieldSafetyContractTests(unittest.TestCase):
    def test_ota_does_not_request_an_active_modem_redial(self):
        ota_source = (REPOSITORY_ROOT / "main" / "ota.c").read_text()
        self.assertNotIn("modem_request_redial(", ota_source)
        self.assertNotIn("modem_request_redial_from(", ota_source)
        self.assertIn("active_modem_action", ota_source)

    def test_web_api_does_not_allocate_a_full_printed_document(self):
        webui_source = (
            REPOSITORY_ROOT / "main" / "webui.c"
        ).read_text()
        self.assertNotRegex(
            webui_source,
            r"\bcJSON_PrintUnformatted\s*\(\s*root",
        )
        self.assertIn("httpd_resp_send_chunk", webui_source)
        self.assertIn("response_error_count", webui_source)
        self.assertIn("json_stream_status_fragment", webui_source)
        self.assertIn("event_journal_visit_events_json", webui_source)
        self.assertNotIn("event_journal_events_json(", webui_source)

    def test_routine_ota_observes_recent_http_control_plane_use(self):
        ota_source = (REPOSITORY_ROOT / "main" / "ota.c").read_text()
        self.assertIn("webui_last_request_uptime_ms()", ota_source)
        self.assertIn("http_quiet_period", ota_source)

    def test_supervisor_activation_boundary_remains_disabled(self):
        modem_source = (
            REPOSITORY_ROOT / "main" / "modem.c"
        ).read_text()
        self.assertIn(
            '"automatic_supervisor_actions_enabled", false',
            modem_source,
        )


if __name__ == "__main__":
    unittest.main()
