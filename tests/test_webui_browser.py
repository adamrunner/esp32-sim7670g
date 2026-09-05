import subprocess
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


class WebUiBrowserBehaviorTests(unittest.TestCase):
    def test_shipped_script_behavior(self):
        result = subprocess.run(
            ["node", str(REPOSITORY_ROOT / "tests" / "webui_browser_test.js")],
            cwd=REPOSITORY_ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("script load starts dashboard polling", result.stdout)
        self.assertIn("deadline abort clears state", result.stdout)
        self.assertIn("SoftAP blocks manual OTA", result.stdout)


if __name__ == "__main__":
    unittest.main()
