import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


class ConnectivityPolicyCoreHostTests(unittest.TestCase):
    def test_portable_synthetic_time_harness(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "connectivity_policy_core_test"
            compile_result = subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(REPOSITORY_ROOT / "main"),
                    str(
                        REPOSITORY_ROOT
                        / "main"
                        / "connectivity_policy_core.c"
                    ),
                    str(
                        REPOSITORY_ROOT
                        / "tests"
                        / "connectivity_policy_core_test.c"
                    ),
                    "-o",
                    str(executable),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                compile_result.returncode, 0, compile_result.stderr
            )
            test_result = subprocess.run(
                [str(executable)],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(test_result.returncode, 0, test_result.stderr)
            self.assertIn(
                "connectivity policy core tests: ok", test_result.stdout
            )


if __name__ == "__main__":
    unittest.main()
