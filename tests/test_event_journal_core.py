import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


class EventJournalCoreHostTests(unittest.TestCase):
    def test_portable_fault_harness(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "event_journal_core_test"
            compile_result = subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-D_DARWIN_C_SOURCE",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(REPOSITORY_ROOT / "main"),
                    str(REPOSITORY_ROOT / "main" / "event_journal_core.c"),
                    str(REPOSITORY_ROOT / "tests" / "event_journal_core_test.c"),
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
            )
            self.assertEqual(test_result.returncode, 0, test_result.stderr)
            self.assertIn("event journal core tests: ok", test_result.stdout)


if __name__ == "__main__":
    unittest.main()
