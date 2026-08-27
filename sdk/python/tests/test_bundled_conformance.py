"""Canonical clean-environment certification of a bundled SDK wheel."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import venv

ROOT = Path(__file__).resolve().parents[3]


class BundledWheelConformanceTests(unittest.TestCase):
    def test_installed_wheel_passes_protocol_v1(self) -> None:
        wheel_value = os.environ.get("TNY_TEST_BUNDLED_WHEEL")
        if not wheel_value:
            self.skipTest("TNY_TEST_BUNDLED_WHEEL is not set")
        wheel = Path(wheel_value).resolve(strict=True)
        with tempfile.TemporaryDirectory() as root_value:
            root = Path(root_value)
            environment = root / "venv"
            venv.EnvBuilder(with_pip=True).create(environment)
            python = environment / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
            installed_environment = dict(
                os.environ, TNY_CONFORMANCE_USE_INSTALLED="1"
            )
            installed_environment.pop("PYTHONPATH", None)
            subprocess.run(
                [str(python), "-m", "pip", "install", "--quiet",
                 "--force-reinstall", str(wheel)],
                cwd=ROOT,
                env=installed_environment,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=True,
                timeout=180,
            )
            report = root / "report.json"
            subprocess.run(
                [
                    sys.executable,
                    "sdk/conformance/run.py",
                    "--artifact",
                    str(wheel),
                    "--report",
                    str(report),
                    "--",
                    str(python),
                    "sdk/python/conformance_adapter.py",
                ],
                cwd=ROOT,
                env=installed_environment,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=True,
                timeout=180,
            )
            result = json.loads(report.read_text(encoding="utf-8"))
            expected_sha = hashlib.sha256(wheel.read_bytes()).hexdigest()
            self.assertEqual(result["artifact"], {
                "kind": "wheel", "sha256": expected_sha,
            })
            self.assertEqual(len(result["scenarios"]), 10)
            self.assertTrue(all(
                scenario["status"] == "pass"
                for scenario in result["scenarios"]
            ))
            execution_ids = {
                execution["id"] for execution in result["executions"]
            }
            self.assertIn("python_installed_package_smoke", execution_ids)


if __name__ == "__main__":
    unittest.main()
