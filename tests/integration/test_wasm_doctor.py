#!/usr/bin/env python3
"""Host-check doctor and interactive-shell wasm seams; no emsdk needed.

These checks do not claim wasm link/runtime compatibility. The real wasm CI
job remains the end-to-end gate.
"""

import os
import shlex
import subprocess
import sys
import tempfile
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


class WasmShellSeamTests(unittest.TestCase):
    def test_shell_command_refuses_to_spawn(self):
        helper = r"""
#include "util/tui_shell_host.h"
#include <errno.h>
int main(void) {
    pid_t pid = 42;
    if (tui_shell_host_start("echo must-not-run", &pid) != -1) return 1;
    if (errno != ENOTSUP || pid != 42) return 2;
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="tny-wasm-shell-seam-") as temp:
            binary = str(Path(temp) / "shell-refusal")
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CC", "cc")),
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Isrc",
                    "src/util/tui_shell_host_wasm.c",
                    "-x",
                    "c",
                    "-",
                    "-o",
                    binary,
                ],
                input=helper,
                text=True,
                cwd=ROOT,
                check=True,
                capture_output=True,
                timeout=30,
            )
            subprocess.run([binary], check=True, capture_output=True, timeout=10)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
