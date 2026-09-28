"""Host-compile the explicit unsupported code-cell seams, with negative controls.

Native code cells act on the host with the OS user's authority (ADR 0180);
there is no confinement layer left to probe. What remains platform-specific
is the refusal: builds without the embedded interpreter (libtny, wasm,
MSYS2/Cygwin) link src/core/code_python_unsupported.c, and wasm's execution
host cannot start a cell process. Both must fail explicitly, never run code
some other way. Linux cells are checked for the absence of seccomp and
resource denials by tests/test_code_runtime.c through the production path.
"""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FLAGS = ["-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'src'}"]


def build_and_run(sources, probe, extra=()):
    compiler = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="tny-code-cell-seam-") as tmp:
        work = Path(tmp)
        (work / "probe.c").write_text(probe)
        binary = work / "probe"
        build = subprocess.run(
            [*compiler, *FLAGS, *extra, *map(str, sources), str(work / "probe.c"), "-o", str(binary)],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if build.returncode:
            return build.returncode, build.stderr
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
        return run.returncode, run.stdout + run.stderr


def mutant(source: Path, old: str, new: str, directory: Path) -> Path:
    text = source.read_text()
    assert text.count(old) == 1, f"{old!r} must occur once in {source}"
    path = directory / source.name
    path.write_text(text.replace(old, new, 1))
    return path


class CodeCellSeams(unittest.TestCase):
    def test_unsupported_interpreter_refuses_explicitly(self):
        source = ROOT / "src/core/code_python_unsupported.c"
        probe = (
            '#include "core/code_python.h"\n#include <stdlib.h>\n#include <string.h>\n'
            "int main(void) {\n"
            "  if (tny_code_python_available() || tny_code_python_init() != -1) return 1;\n"
            "  char *out = tny_code_python_run(\"print(1)\", NULL);\n"
            '  int ok = out && strcmp(out, "error: code: Python code cells are unavailable in '
            'this build") == 0;\n'
            "  free(out);\n  return ok ? 0 : 2;\n}\n"
        )
        rc, output = build_and_run([source], probe)
        self.assertEqual(rc, 0, output)
        with tempfile.TemporaryDirectory() as tmp:
            broken = mutant(
                source,
                "bool tny_code_python_available(void) { return false; }",
                "bool tny_code_python_available(void) { return true; }",
                Path(tmp),
            )
            rc, output = build_and_run([broken], probe)
            self.assertEqual(rc, 1, "a stub claiming an interpreter must be rejected")

    def test_wasm_host_cannot_start_a_cell(self):
        source = ROOT / "src/util/execution_host.c"
        probe = (
            '#include "util/execution_host.h"\n#include <errno.h>\n'
            "int main(void) {\n"
            "  tny_exec_host host;\n"
            "  if (tny_exec_host_start_cell(&host, 1) != ENOTSUP) return 1;\n"
            "  if (host.fd != -1 || host.pid != -1) return 2;\n"
            "  errno = 0;\n"
            '  if (tny_exec_host_receive_aux(3, NULL, 0, NULL, NULL) || errno != ENOTSUP) return 3;\n'
            "  return 0;\n}\n"
        )
        rc, output = build_and_run([source], probe, ["-D__EMSCRIPTEN__"])
        self.assertEqual(rc, 0, output)
        with tempfile.TemporaryDirectory() as tmp:
            broken = mutant(
                source,
                "    *host = (tny_exec_host){.fd = -1, .pid = -1};\n    return ENOTSUP;\n}\n"
                "int tny_exec_host_start_cell(",
                "    *host = (tny_exec_host){.fd = -1, .pid = -1};\n    return 0;\n}\n"
                "int tny_exec_host_start_cell(",
                Path(tmp),
            )
            rc, output = build_and_run([broken], probe, ["-D__EMSCRIPTEN__"])
            self.assertEqual(rc, 1, "a wasm seam that reports a started cell must be rejected")


if __name__ == "__main__":
    unittest.main()
