#!/usr/bin/env python3
"""Enforce the user's strict decimal-six-MB guardrail from one build policy."""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LIMIT = 6_000_000
MAXIMUM = LIMIT - 1


class SizeBudgetTests(unittest.TestCase):
    def make(self, *args: str, input_text: str | None = None):
        env = dict(os.environ)
        for key in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "SIZE_MAX", "WASM_SIZE_MAX"):
            env.pop(key, None)
        return subprocess.run(
            ["make", "--no-print-directory", "-s", *args],
            cwd=ROOT,
            env=env,
            text=True,
            capture_output=True,
            input=input_text,
            timeout=30,
        )

    def budget(self, system: str, architecture: str, *overrides: str):
        result = self.make(
            f"UNAME_S={system}",
            f"UNAME_M={architecture}",
            *overrides,
            "-f",
            "Makefile",
            "-f",
            "-",
            "budget-probe",
            input_text="budget-probe:\n\t@echo $(SIZE_MAX) $(WASM_SIZE_MAX)\n",
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "")
        return tuple(map(int, result.stdout.split()))

    def test_one_ceiling_for_all_platforms_and_link_modes(self):
        for system, arch in (
            ("Linux", "aarch64"),
            ("Linux", "arm64"),
            ("Linux", "x86_64"),
            ("Linux", "riscv64"),
            ("Darwin", "arm64"),
            ("MSYS_NT-10.0", "x86_64"),
        ):
            for static in (0, 1):
                with self.subTest(system=system, architecture=arch, static=static):
                    self.assertEqual(
                        self.budget(system, arch, f"STATIC={static}"),
                        (MAXIMUM, MAXIMUM),
                    )

    def test_explicit_stricter_downstream_override_survives(self):
        self.assertEqual(
            self.budget("Linux", "aarch64", "SIZE_MAX=12345"), (12345, 12345)
        )
        self.assertEqual(
            self.budget("Linux", "aarch64", "WASM_SIZE_MAX=9876"), (MAXIMUM, 9876)
        )

    def test_workflows_do_not_fork_the_product_size_policy(self):
        for name in ("ci.yml", "release.yml"):
            text = (ROOT / ".github/workflows" / name).read_text()
            with self.subTest(workflow=name):
                self.assertIn("make size-check", text)
                self.assertNotRegex(text, r"\b(?:WASM_)?SIZE_MAX\s*=")
                self.assertNotRegex(text, r"\bsize_max:")
        self.assertIn("$(SIZE_MAX)", (ROOT / "nix/package.nix").read_text())

    def test_native_boundary_rejects_exactly_six_megabytes(self):
        with tempfile.TemporaryDirectory(prefix="tny-size-native-") as root:
            path = Path(root) / "artifact"
            for size in (MAXIMUM, LIMIT):
                with path.open("wb") as file:
                    file.truncate(size)
                result = self.make("-o", "release", "size-check", f"BIN={path}")
                self.assertIn(f"limit {MAXIMUM}", result.stdout)
                self.assertEqual(result.returncode == 0, size < LIMIT, result.stderr)
                if size < LIMIT:
                    self.assertEqual(result.stderr, "")
                else:
                    self.assertIn("over the", result.stderr)

    def test_wasm_and_glue_together_reject_exactly_six_megabytes(self):
        with tempfile.TemporaryDirectory(prefix="tny-size-wasm-") as root:
            javascript = Path(root) / "artifact.js"
            wasm = javascript.with_suffix(".wasm")
            javascript.write_bytes(b"fixture-glue")
            for size in (MAXIMUM, LIMIT):
                with wasm.open("wb") as file:
                    file.truncate(size - javascript.stat().st_size)
                result = self.make(
                    "-o", "wasm", "wasm-size-check", f"WASM_NODE={javascript}"
                )
                self.assertIn(f"{size} wasm artifact", result.stdout)
                self.assertEqual(result.returncode == 0, size < LIMIT, result.stderr)
                if size < LIMIT:
                    self.assertEqual(result.stderr, "")
                else:
                    self.assertIn("over the", result.stderr)

    def test_wasm_budget_is_declared_from_same_source(self):
        makefile = (ROOT / "Makefile").read_text()
        self.assertRegex(
            makefile, re.compile(r"^WASM_SIZE_MAX\s*\?=\s*\$\(SIZE_MAX\)$", re.M)
        )
        self.assertIn('"$$bytes" -gt "$(WASM_SIZE_MAX)"', makefile)


if __name__ == "__main__":
    unittest.main()
