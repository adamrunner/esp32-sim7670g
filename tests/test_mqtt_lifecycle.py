import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
IDF_CJSON = Path(
    "/Users/adamrunner/esp/v5.5/esp-idf/components/json/cJSON"
)


class MqttLifecycleHostTests(unittest.TestCase):
    def build_harness(self, directory):
        executable = Path(directory) / "mqtt_lifecycle_test"
        environment = os.environ.copy()
        environment["DEVELOPER_DIR"] = "/Library/Developer/CommandLineTools"
        compile_result = subprocess.run(
            [
                "cc",
                "-std=c11",
                "-D_DARWIN_C_SOURCE",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-parameter",
                "-I",
                str(REPOSITORY_ROOT / "tests" / "mqtt_fakes"),
                "-I",
                str(REPOSITORY_ROOT / "main"),
                "-I",
                str(IDF_CJSON),
                str(REPOSITORY_ROOT / "main" / "mqtt.c"),
                str(REPOSITORY_ROOT / "main" / "event_journal.c"),
                str(REPOSITORY_ROOT / "tests" / "mqtt_lifecycle_test.c"),
                str(IDF_CJSON / "cJSON.c"),
                "-o",
                str(executable),
            ],
            check=False,
            capture_output=True,
            text=True,
            env=environment,
            timeout=20,
        )
        self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
        return executable

    def test_production_mqtt_lifecycle_with_failure_injection(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = self.build_harness(directory)
            test_result = subprocess.run(
                [str(executable), "a1"],
                check=False,
                capture_output=True,
                text=True,
                timeout=5,
            )
            self.assertEqual(test_result.returncode, 0, test_result.stderr)
            self.assertIn("MQTT lifecycle tests: ok", test_result.stdout)

    def test_shared_boot_identity_survives_journal_setup_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = self.build_harness(directory)
            boot_ids = []
            for scenario, seed in [
                ("boot-normal", "1"),
                ("boot-journal-fail", "2"),
                ("boot-normal", "3"),
            ]:
                result = subprocess.run(
                    [str(executable), scenario, seed],
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=5,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                boot_line = next(
                    line for line in result.stdout.splitlines()
                    if line.startswith("BOOT_ID=")
                )
                boot_ids.append(boot_line.removeprefix("BOOT_ID="))
            self.assertEqual(len(set(boot_ids)), 3)

    def test_time_status_task_retries_then_exits(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = self.build_harness(directory)
            for scenario in [
                "status-delayed-time",
                "status-disconnected",
                "status-enqueue-failure",
            ]:
                result = subprocess.run(
                    [str(executable), scenario],
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=5,
                )
                self.assertEqual(
                    result.returncode, 0, f"{scenario}: {result.stderr}"
                )

if __name__ == "__main__":
    unittest.main()
