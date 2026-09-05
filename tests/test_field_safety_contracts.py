import re
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


class FieldSafetyContractTests(unittest.TestCase):
    def test_ota_does_not_request_an_active_modem_redial(self):
        ota_source = (REPOSITORY_ROOT / "main" / "ota.c").read_text()
        self.assertNotIn("modem_request_redial(", ota_source)
        self.assertNotIn("modem_request_redial_from(", ota_source)
        self.assertIn("active_modem_action", ota_source)

    def test_static_streaming_wiring_avoids_full_document_print(self):
        webui_source = (
            REPOSITORY_ROOT / "main" / "webui.c"
        ).read_text()
        self.assertNotRegex(
            webui_source,
            r"\bcJSON_PrintUnformatted\s*\(\s*root",
        )
        self.assertIn(
            "webui_json_stream_object_fragments(", _code(webui_source)
        )
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
        self.assertTrue(
            _config_is_disabled(
                sdkconfig_defaults, "CONFIG_LWIP_DHCPS_ADD_DNS"
            )
        )

    def test_static_socket_capacity_configuration(self):
        webui_source = (
            REPOSITORY_ROOT / "main" / "webui.c"
        ).read_text()
        sdkconfig_defaults = (
            REPOSITORY_ROOT / "sdkconfig.defaults"
        ).read_text()
        defaults = _config_assignments(sdkconfig_defaults)

        self.assertIn("#define WEBUI_MAX_CLIENT_SESSIONS 4", webui_source)
        self.assertIn(
            "cfg.max_open_sockets = WEBUI_MAX_CLIENT_SESSIONS;",
            webui_source,
        )
        self.assertEqual(defaults.get("CONFIG_LWIP_MAX_SOCKETS"), "10")

    def test_static_manual_ota_api_route_is_registered(self):
        webui_source = (
            REPOSITORY_ROOT / "main" / "webui.c"
        ).read_text()
        self.assertIn('"/api/ota/check"', webui_source)

    def test_webui_instruments_the_session_and_socket_layer(self):
        webui_source = (
            REPOSITORY_ROOT / "main" / "webui.c"
        ).read_text()

        # Session lifecycle hooks are wired and the custom close_fn owns
        # the descriptor (the server does not close it when close_fn is set).
        self.assertIn("cfg.open_fn = session_open_fn;", webui_source)
        self.assertIn("cfg.close_fn = session_close_fn;", webui_source)
        self.assertIn("close(sockfd);", webui_source)

        # A dead client may not pin the single server task for the default
        # five seconds per send/recv call.
        self.assertIn("cfg.recv_wait_timeout = 2;", webui_source)
        self.assertIn("cfg.send_wait_timeout = 2;", webui_source)

        # Distinct journal evidence for each suspected field mechanism:
        # table saturation (LRU purge precondition), transport errors,
        # slow-client send stalls, and polling during OTA transport.
        self.assertIn('"session_table_full"', webui_source)
        self.assertIn("HTTP_SERVER_EVENT_ERROR", webui_source)
        self.assertIn('"send_stall"', webui_source)
        self.assertIn('"request_during_ota"', webui_source)

        # The counters behind the soak-test acceptance metrics are exposed.
        self.assertIn('"session_high_water"', webui_source)
        self.assertIn('"session_table_full_count"', webui_source)
        self.assertIn('"send_stall_count"', webui_source)
        self.assertIn('"max_chunk_send_ms"', webui_source)
        self.assertIn('"request_during_ota_count"', webui_source)
        self.assertIn('"request_duration_counts"', webui_source)

    def test_psram_is_enabled_in_octal_mode_with_internal_reserves(self):
        sdkconfig_defaults = (
            REPOSITORY_ROOT / "sdkconfig.defaults"
        ).read_text()
        defaults = _config_assignments(sdkconfig_defaults)

        required = {
            "CONFIG_SPIRAM": "y",
            "CONFIG_SPIRAM_MODE_OCT": "y",
            "CONFIG_SPIRAM_USE_MALLOC": "y",
            "CONFIG_SPIRAM_MEMTEST": "y",
            "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL": "16384",
            "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL": "32768",
        }
        for option, value in required.items():
            self.assertEqual(defaults.get(option), value)

        # Still a separate evidence-gated measurement, not yet taken.
        self.assertNotEqual(
            defaults.get("CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP"), "y"
        )

        # mbedTLS in PSRAM was the first gated extra, enabled once an OTA
        # download under polling reproduced MBEDTLS_ERR_SSL_ALLOC_FAILED with
        # PSRAM on. It replaces the internal-alloc default, so both halves of
        # the choice must agree or the build silently keeps TLS internal.
        self.assertEqual(defaults.get("CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC"), "y")
        self.assertTrue(
            _config_is_disabled(
                sdkconfig_defaults, "CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC"
            )
        )
        required["CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC"] = "y"

        # The standing gotcha: sdkconfig.defaults does not propagate into an
        # already-generated sdkconfig. When one exists locally it must agree,
        # or the build silently keeps PSRAM off. The file is gitignored, so
        # this half of the contract only runs where it is present.
        generated = REPOSITORY_ROOT / "sdkconfig"
        if generated.exists():
            sdkconfig = generated.read_text()
            generated_values = _config_assignments(sdkconfig)
            for option, value in required.items():
                self.assertEqual(
                    generated_values.get(option),
                    value,
                    f"{option}={value} missing from the generated sdkconfig; "
                    "run idf.py fullclean or set it there too",
                )

    def test_resource_evidence_separates_internal_ram_from_psram(self):
        webui_source = (
            REPOSITORY_ROOT / "main" / "webui.c"
        ).read_text()
        ota_source = (REPOSITORY_ROOT / "main" / "ota.c").read_text()
        modem_source = (
            REPOSITORY_ROOT / "main" / "modem.c"
        ).read_text()

        # With PSRAM enabled these queries span both pools, which would put an
        # 8 MB number where the field baseline expects internal-RAM headroom.
        for source in (webui_source, ota_source, modem_source):
            self.assertNotIn("esp_get_free_heap_size()", _code(source))
            self.assertNotIn(
                "esp_get_minimum_free_heap_size()", _code(source)
            )
            self.assertNotIn(
                "heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)",
                _code(source),
            )

        self.assertIn(
            "#define WEBUI_INTERNAL_CAPS "
            "(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)",
            webui_source,
        )

        # Both pools are observable in the field from /api/status.http.
        for field in (
            '"min_free_heap"',
            '"min_largest_free_block"',
            '"min_free_heap_all_time"',
            '"psram_total"',
            '"psram_free"',
            '"min_free_psram"',
            '"min_largest_free_psram_block"',
        ):
            self.assertIn(field, webui_source)


def _code(source):
    """Source with // comment bodies stripped, so prose about a call does
    not read as the call itself."""
    return "\n".join(
        line.split("//", 1)[0] for line in source.splitlines()
    )


def _config_assignments(source):
    """Return only active CONFIG_NAME=value assignments."""
    assignments = {}
    for line in source.splitlines():
        match = re.fullmatch(r"(CONFIG_[A-Z0-9_]+)=(.*)", line.strip())
        if match:
            assignments[match.group(1)] = match.group(2)
    return assignments


def _config_is_disabled(source, name):
    return any(
        line.strip() == f"# {name} is not set"
        for line in source.splitlines()
    )


if __name__ == "__main__":
    unittest.main()
