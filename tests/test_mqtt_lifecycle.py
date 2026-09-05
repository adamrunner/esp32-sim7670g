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
    def test_production_mqtt_lifecycle_with_failure_injection(self):
        with tempfile.TemporaryDirectory() as directory:
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
            test_result = subprocess.run(
                [str(executable)],
                check=False,
                capture_output=True,
                text=True,
                timeout=5,
            )
            self.assertEqual(test_result.returncode, 0, test_result.stderr)
            self.assertIn("MQTT lifecycle tests: ok", test_result.stdout)


if __name__ == "__main__":
    unittest.main()
