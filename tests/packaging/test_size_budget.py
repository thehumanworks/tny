#!/usr/bin/env python3
"""Artifact reporting has no byte ceiling; invalid/missing inputs still fail."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def leb128(value: int) -> bytes:
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


class SizePolicyTests(unittest.TestCase):
    def make(self, *args: str):
        env = dict(os.environ)
        for key in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "SIZE_MAX", "WASM_SIZE_MAX"):
            env.pop(key, None)
        return subprocess.run(
            ["make", "--no-print-directory", "-s", *args],
            cwd=ROOT,
            env=env,
            text=True,
            capture_output=True,
            timeout=30,
        )

    def test_native_accepts_larger_valid_artifacts_and_reports_exact_bytes(self):
        with tempfile.TemporaryDirectory(prefix="tny-size-native-") as root:
            path = Path(root) / "artifact"
            # A real host executable with harmless trailing data, not a fake header.
            for extra in (0, 6_000_000, 24_000_000):
                shutil.copy2(sys.executable, path)
                with path.open("ab") as file:
                    file.truncate(path.stat().st_size + extra)
                result = self.make(
                    "-o", "release", "size-check", f"BIN={path}", "SIZE_MAX=1"
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(f"{path.stat().st_size} {path}", result.stdout)

    def test_native_missing_empty_invalid_and_nonexecutable_fail(self):
        with tempfile.TemporaryDirectory(prefix="tny-size-native-") as root:
            path = Path(root) / "artifact"
            for data in (None, b"", b"not an executable"):
                if data is not None:
                    path.write_bytes(data)
                    path.chmod(0o755)
                result = self.make("-o", "release", "size-check", f"BIN={path}")
                self.assertNotEqual(result.returncode, 0, result.stdout)
            shutil.copy2(sys.executable, path)
            path.chmod(0o644)
            result = self.make("-o", "release", "size-check", f"BIN={path}")
            self.assertNotEqual(result.returncode, 0, result.stdout)

    def test_wasm_accepts_larger_valid_modules_and_reports_components(self):
        with tempfile.TemporaryDirectory(prefix="tny-size-wasm-") as root:
            javascript = Path(root) / "artifact.js"
            wasm = javascript.with_suffix(".wasm")
            javascript.write_bytes(b"// fixture glue\n")
            for padding in (0, 6_000_000, 24_000_000):
                # Valid v1 module with a custom section: empty name plus payload.
                with wasm.open("wb") as file:
                    file.write(b"\0asm\1\0\0\0\0" + leb128(padding + 1) + b"\0")
                    file.truncate(file.tell() + padding)
                result = self.make(
                    "-o",
                    "wasm",
                    "wasm-size-check",
                    f"WASM_NODE={javascript}",
                    "WASM_SIZE_MAX=1",
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                total = javascript.stat().st_size + wasm.stat().st_size
                self.assertIn(f"{total} wasm artifact", result.stdout)
                for path in (javascript, wasm):
                    self.assertIn(f"{path.stat().st_size} {path}", result.stdout)

    def test_wasm_missing_empty_and_invalid_fail(self):
        with tempfile.TemporaryDirectory(prefix="tny-size-wasm-") as root:
            javascript = Path(root) / "artifact.js"
            wasm = javascript.with_suffix(".wasm")
            for js, module in (
                (None, b"\0asm\1\0\0\0"),
                (b"// glue", None),
                (b"", b"\0asm\1\0\0\0"),
                (b"// glue", b""),
                (b"// glue", b"invalid module"),
                (b"// glue", b"\0asm\2\0\0\0"),
            ):
                for path, data in ((javascript, js), (wasm, module)):
                    path.unlink(missing_ok=True)
                    if data is not None:
                        path.write_bytes(data)
                result = self.make(
                    "-o", "wasm", "wasm-size-check", f"WASM_NODE={javascript}"
                )
                self.assertNotEqual(result.returncode, 0, result.stdout)

    def test_build_and_packaging_do_not_declare_size_ceilings(self):
        paths = [ROOT / "Makefile", ROOT / "nix/package.nix"]
        paths.extend((ROOT / ".github/workflows").glob("*.yml"))
        for path in paths:
            with self.subTest(path=path):
                text = path.read_text()
                self.assertNotRegex(text, r"\b(?:WASM_)?SIZE_MAX\b|\bsize_max:")
        package = (ROOT / "nix/package.nix").read_text()
        self.assertIn("(installed payload)", package)
        self.assertIn('size-check BIN="$payload"', package)
        for name in ("ci.yml", "release.yml"):
            self.assertIn(
                "make size-check", (ROOT / ".github/workflows" / name).read_text()
            )


if __name__ == "__main__":
    unittest.main()
