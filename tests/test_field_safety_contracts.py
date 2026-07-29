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

    def test_softap_dhcp_omits_router_and_dns_offers(self):
        wifi_source = (REPOSITORY_ROOT / "main" / "wifi.c").read_text()
        sdkconfig_defaults = (
            REPOSITORY_ROOT / "sdkconfig.defaults"
        ).read_text()

        self.assertIn(
            "ESP_NETIF_ROUTER_SOLICITATION_ADDRESS",
            wifi_source,
        )
        self.assertIn("ESP_NETIF_DOMAIN_NAME_SERVER", wifi_source)
        self.assertIn("uint8_t disabled = 0;", wifi_source)
        self.assertIn(
            '"ap_dhcp_router_offer", false',
            wifi_source,
        )
        self.assertIn('"ap_dhcp_dns_offer", false', wifi_source)
        self.assertIn(
            "# CONFIG_LWIP_DHCPS_ADD_DNS is not set",
            sdkconfig_defaults,
        )

    def test_webui_reserves_sockets_and_serializes_bounded_polling(self):
        webui_source = (
            REPOSITORY_ROOT / "main" / "webui.c"
        ).read_text()
        html = (
            REPOSITORY_ROOT / "main" / "www" / "index.html"
        ).read_text()
        sdkconfig_defaults = (
            REPOSITORY_ROOT / "sdkconfig.defaults"
        ).read_text()

        self.assertIn("cfg.max_open_sockets = 4;", webui_source)
        self.assertIn(
            "CONFIG_LWIP_MAX_SOCKETS=10",
            sdkconfig_defaults,
        )
        self.assertIn("POLL_REQUEST_TIMEOUT_MS", html)
        self.assertIn("new AbortController()", html)
        self.assertIn("statusRefreshActive", html)
        self.assertIn("wifiRefreshActive", html)
        self.assertIn("otaRefreshActive", html)
        self.assertIn("await refresh();", html)
        self.assertIn("await refreshWifi();", html)
        self.assertIn("await refreshOta();", html)
        self.assertNotIn("setInterval(refresh", html)

    def test_softap_webui_guards_manual_ota_without_changing_api(self):
        webui_source = (
            REPOSITORY_ROOT / "main" / "webui.c"
        ).read_text()
        html = (
            REPOSITORY_ROOT / "main" / "www" / "index.html"
        ).read_text()

        self.assertIn("softApControlPlaneActive", html)
        self.assertIn(
            "Manual checks are disabled during a SoftAP session",
            html,
        )
        self.assertIn('id="otaerrormsg"', html)
        self.assertIn('id="otadefermsg"', html)
        self.assertIn('"/api/ota/check"', webui_source)


if __name__ == "__main__":
    unittest.main()
