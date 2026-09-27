"""Host-compile the explicit wasm no-capability seam, with a negative control."""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/util/code_sandbox.c"


class CodeSandboxWasm(unittest.TestCase):
    def test_native_os_denial_with_positive_controls(self):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory(prefix="tny-code-sandbox-native-") as tmp:
            binary = Path(tmp) / "native-probe"
            command = [
                *compiler,
                "-std=c11",
                "-D_GNU_SOURCE",
                "-D_DEFAULT_SOURCE",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'src'}",
                str(ROOT / "tests/fixtures/code_sandbox_host.c"),
                str(SOURCE),
                "-o",
                str(binary),
            ]
            build = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(build.returncode, 0, build.stderr)
            actual = subprocess.run(
                [str(binary)], capture_output=True, text=True, timeout=10
            )
            self.assertEqual(
                actual.returncode,
                0,
                f"native sandbox/control stage failed: {actual.returncode}: {actual.stderr}",
            )
            # A no-op sandbox must fail the same actual-effect oracle. This is
            # a counterexample, not an accepted unsupported or skipped result.
            stub = Path(tmp) / "no-sandbox.c"
            stub.write_text(
                "int tny_code_sandbox_limits(int n) {(void)n;return 0;}\n"
                "int tny_code_sandbox_enter(void) {return 0;}\n"
            )
            mutant = [str(stub) if arg == str(SOURCE) else arg for arg in command]
            subprocess.run(
                mutant, capture_output=True, text=True, timeout=30, check=True
            )
            rejected = subprocess.run(
                [str(binary)], capture_output=True, text=True, timeout=10
            )
            self.assertEqual(
                rejected.returncode,
                72,
                "removing OS confinement must be rejected when native open succeeds",
            )

    def test_clean_refusal_and_no_native_only_helper(self):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory(prefix="tny-code-sandbox-seam-") as tmp:
            work = Path(tmp)
            host = work / "probe.c"
            host.write_text(
                '#include "util/code_sandbox.h"\n#include <errno.h>\n'
                "int main(void) {\n"
                "  errno=0; if(tny_code_sandbox_limits(3)!=-1 || errno!=ENOTSUP) return 1;\n"
                "  errno=0; if(tny_code_sandbox_enter()!=-1 || errno!=ENOTSUP) return 2;\n"
                "  return 0;\n}\n"
            )
            flags = [
                "-std=c11",
                "-D_DEFAULT_SOURCE",
                "-D__EMSCRIPTEN__",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'src'}",
            ]
            binary = work / "probe"
            positive = subprocess.run(
                [*compiler, *flags, str(SOURCE), str(host), "-o", str(binary)],
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(positive.returncode, 0, positive.stderr)
            subprocess.run([str(binary)], check=True, timeout=5)
            text = SOURCE.read_text()
            marker = "#ifndef __EMSCRIPTEN__\nstatic int limit("
            self.assertEqual(text.count(marker), 1)
            mutant = work / "mutant.c"
            mutant.write_text(text.replace(marker, "#if 1\nstatic int limit(", 1))
            negative = subprocess.run(
                [*compiler, *flags, str(mutant), str(host), "-o", str(binary)],
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertNotEqual(
                negative.returncode,
                0,
                "unguarded native helper must fail the strict wasm branch build",
            )
            self.assertIn("unused", negative.stderr)
            self.assertIn("limit", negative.stderr)


if __name__ == "__main__":
    unittest.main()
