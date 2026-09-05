import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
FUNCTION_SIGNATURES = (
    "static void spool_load_state(void)",
    "static void spool_save_cursor(void)",
    "static void spool_reset(void)",
    "static void spool_replay_tick(void)",
)
MARKER = "/* PRODUCTION_SPOOL_FUNCTIONS */"


def _source_under_test():
    revision = os.environ.get("DATALOG_SOURCE_REV")
    if revision:
        result = subprocess.run(
            ["/opt/homebrew/bin/git", "show", f"{revision}:main/datalog.c"],
            cwd=REPOSITORY_ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode:
            raise RuntimeError(result.stderr)
        return result.stdout
    return (REPOSITORY_ROOT / "main" / "datalog.c").read_text()


def _extract_function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start + len(signature))
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise ValueError(f"unterminated function: {signature}")


class DatalogSpoolHostTests(unittest.TestCase):
    def test_production_replay_code_with_temporary_files(self):
        source = _source_under_test()
        production_functions = "\n\n".join(
            _extract_function(source, signature)
            for signature in FUNCTION_SIGNATURES
        )
        harness = (
            REPOSITORY_ROOT / "tests" / "datalog_spool_test.c"
        ).read_text()
        self.assertEqual(harness.count(MARKER), 1)
        harness = harness.replace(MARKER, production_functions)

        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness_path = directory / "datalog_spool_generated.c"
            executable = directory / "datalog_spool_test"
            harness_path.write_text(harness)
            compile_result = subprocess.run(
                [
                    os.environ.get("CC", "cc"),
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(harness_path),
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
            test_root = directory / "cases"
            test_root.mkdir()
            test_result = subprocess.run(
                [str(executable), str(test_root)],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(test_result.returncode, 0, test_result.stderr)
            self.assertIn(
                "datalog spool replay tests: ok", test_result.stdout
            )


if __name__ == "__main__":
    unittest.main()
