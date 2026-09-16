#!/usr/bin/env python3
"""Keep measured per-platform artifact budgets enforced, not merely documented."""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ARM64_LIMIT = 1_052_672


class SizeBudgetTests(unittest.TestCase):
    def make(
        self, *arguments: str, input_text: str | None = None
    ) -> subprocess.CompletedProcess[str]:
        environment = dict(os.environ)
        for name in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "SIZE_MAX"):
            environment.pop(name, None)
        return subprocess.run(
            ["make", "--no-print-directory", "-s", *arguments],
            cwd=ROOT,
            env=environment,
            text=True,
            capture_output=True,
            input=input_text,
            timeout=30,
        )

    def budget(self, system: str, architecture: str, *overrides: str) -> int:
        result = self.make(
            f"UNAME_S={system}",
            f"UNAME_M={architecture}",
            *overrides,
            "-f",
            "Makefile",
            "-f",
            "-",
            "tny-budget-probe",
            input_text="tny-budget-probe:\n\t@echo $(SIZE_MAX)\n",
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "")
        return int(result.stdout.strip())

    def test_only_linux_arm64_receives_the_measured_allowance(self) -> None:
        cases = (
            ("Linux", "aarch64", ARM64_LIMIT),
            ("Linux", "arm64", ARM64_LIMIT),
            ("Linux", "x86_64", 1_048_576),
            ("Linux", "riscv64", 1_048_576),
            ("Darwin", "arm64", 1_887_436),
            ("Darwin", "x86_64", 1_887_436),
            ("MSYS_NT-10.0", "x86_64", 2_097_152),
        )
        for system, architecture, expected in cases:
            with self.subTest(system=system, architecture=architecture):
                self.assertEqual(self.budget(system, architecture), expected)

    def test_explicit_caller_override_is_not_overwritten(self) -> None:
        self.assertEqual(self.budget("Linux", "aarch64", "SIZE_MAX=12345"), 12_345)
        self.assertEqual(
            self.budget("Linux", "aarch64", "STATIC=1", "SIZE_MAX=1572864"),
            1_572_864,
        )

    def test_ci_and_release_mirror_both_linux_architectures(self) -> None:
        for name in ("ci.yml", "release.yml"):
            text = (ROOT / ".github/workflows" / name).read_text()
            for architecture, expected in (
                ("aarch64", ARM64_LIMIT),
                ("x86_64", 1_048_576),
            ):
                with self.subTest(workflow=name, architecture=architecture):
                    block = text.split(f"- name: linux-{architecture}\n", 1)[1]
                    block = block.split("- name:", 1)[0]
                    match = re.search(r'size_max: "(\d+)"', block)
                    self.assertIsNotNone(match)
                    assert match is not None
                    self.assertEqual(int(match.group(1)), expected)

    def test_real_size_check_accepts_boundary_and_rejects_one_byte_over(self) -> None:
        with tempfile.TemporaryDirectory(prefix="tny-size-boundary-") as root:
            artifact = Path(root) / "artifact"
            for size in (ARM64_LIMIT, ARM64_LIMIT + 1):
                with self.subTest(bytes=size):
                    with artifact.open("wb") as file:
                        file.truncate(size)
                    result = self.make(
                        "-o",
                        "release",
                        "size-check",
                        f"BIN={artifact}",
                        "UNAME_S=Linux",
                        "UNAME_M=aarch64",
                    )
                    self.assertIn(f"limit {ARM64_LIMIT}", result.stdout)
                    if size == ARM64_LIMIT:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(result.stderr, "")
                    else:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn(
                            f"over the {ARM64_LIMIT}-byte budget", result.stderr
                        )


if __name__ == "__main__":
    unittest.main()
