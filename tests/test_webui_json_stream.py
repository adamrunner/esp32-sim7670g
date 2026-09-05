import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
IDF_PATH = Path(
    os.environ.get("IDF_PATH", "/Users/adamrunner/esp/v5.5/esp-idf")
)


class WebUiJsonStreamHostTests(unittest.TestCase):
    def test_production_streamer(self):
        cjson_directory = IDF_PATH / "components" / "json" / "cJSON"
        self.assertTrue(cjson_directory.is_dir(), f"cJSON not found at {cjson_directory}")
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "webui_json_stream_test"
            compile_result = subprocess.run(
                [
                    os.environ.get("CC", "cc"),
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(REPOSITORY_ROOT / "main"),
                    "-I",
                    str(cjson_directory),
                    str(REPOSITORY_ROOT / "main" / "webui_json_stream.c"),
                    str(cjson_directory / "cJSON.c"),
                    str(REPOSITORY_ROOT / "tests" / "webui_json_stream_test.c"),
                    "-o",
                    str(executable),
                ],
                check=False,
                capture_output=True,
                text=True,
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
            self.assertIn("webui JSON stream tests: ok", test_result.stdout)
            encoded = next(
                line.removeprefix("encoded-json:")
                for line in test_result.stdout.splitlines()
                if line.startswith("encoded-json:")
            )
            value = json.loads(encoded)
            self.assertEqual(
                value["escaped"], 'quote " slash \\ line\n\t\x01'
            )
            self.assertEqual(value["number"], 12.5)
            self.assertIsNone(value["nothing"])
            self.assertIs(value["enabled"], True)
            self.assertEqual(value["nested"]["values"], [-3, "value"])


if __name__ == "__main__":
    unittest.main()
