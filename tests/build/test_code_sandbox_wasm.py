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
