from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "sdk/conformance/run.py"
ADAPTER = ROOT / "tests/conformance/fake_adapter.py"
FIXTURES = ROOT / "tests/conformance/fixtures"


class RunnerTests(unittest.TestCase):
    def run_adapter(self, artifact: Path, report: Path, mutation: str | None = None):
        environment = dict(os.environ)
        if mutation:
            environment["TNY_CONFORMANCE_MUTATION"] = os.fspath(FIXTURES / mutation)
        return subprocess.run([
            sys.executable, os.fspath(RUNNER), "--artifact", os.fspath(artifact),
            "--report", os.fspath(report), "--", sys.executable, os.fspath(ADAPTER),
        ], cwd=ROOT, env=environment, capture_output=True, text=True, timeout=20)

    def test_valid_report_is_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact = root / "libtny.fixture"
            artifact.write_bytes(b"same executed artifact\n")
            first = root / "first.json"
            second = root / "second.json"
            for report in (first, second):
                run = self.run_adapter(artifact, report)
                self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertNotIn(b"secret", first.read_bytes().lower())

    def test_negative_fixtures_are_release_blocking_and_write_nothing(self) -> None:
        fixtures = sorted(path.name for path in FIXTURES.glob("*.json"))
        self.assertGreaterEqual(len(fixtures), 12)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact = root / "libtny.fixture"
            artifact.write_bytes(b"artifact\n")
            for fixture in fixtures:
                with self.subTest(fixture=fixture):
                    report = root / f"{fixture}.report"
                    run = self.run_adapter(artifact, report, fixture)
                    self.assertEqual(run.returncode, 1, run.stderr)
                    self.assertFalse(report.exists())
                    self.assertNotIn("tny-conformance-secret-", run.stderr)


if __name__ == "__main__":
    unittest.main()
