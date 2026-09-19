#!/usr/bin/env python3
"""Compile the doctor's wasm branch with host warnings; no emsdk needed.

This catches unused native-only probes, not wasm link/runtime compatibility.
The real wasm CI job remains the end-to-end gate.
"""

import os
import shlex
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class WasmDoctorTests(unittest.TestCase):
    def test_native_probe_is_absent_from_wasm_branch(self):
        subprocess.run(
            [
                *shlex.split(os.environ.get("CC", "cc")),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-D__EMSCRIPTEN__",
                "-D_DARWIN_C_SOURCE",
                "-D_DEFAULT_SOURCE",
                "-Iinclude",
                "-Isrc",
                "-Ithird_party",
                "-Ithird_party/yyjson",
                "-Ibuild/generated",
                "-fsyntax-only",
                "src/cli/cmd_doctor.c",
            ],
            cwd=ROOT,
            check=True,
            capture_output=True,
            timeout=30,
        )


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
