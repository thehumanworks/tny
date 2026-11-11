"""The stdlib freezer replaces the host shell with the build's shell (ADR 0220).

A Nix build passes a store shell as TNY_SHELL_PATH and its package check
rejects any "/bin/sh" left in the binary. The frozen `subprocess` module is
the one stdlib user of that path, so the freezer must rewrite it, and must
leave an ordinary build byte-for-byte alone.
"""

from __future__ import annotations

import ast
import importlib.util
import marshal
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "cpython_freeze_stdlib", ROOT / "scripts" / "cpython_freeze_stdlib.py"
)
freezer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(freezer)

STORE_SHELL = "/nix/store/00000000000000000000000000000000-bash-5.3/bin/bash"
# The shapes CPython's subprocess.py uses: a docstring example, the Android
# fallback that contains the host path as a suffix, and the POSIX default.
MODULE = b'''"""Example: check_output(["/bin/sh", "-c", "true"])."""
ANDROID = '/system/bin/sh'
UNIX_SHELL = '/bin/sh'
'''


def frozen_code(directory: Path, symbol: str):
    """Decode the C string literal the freezer wrote back into a code object."""
    lines = (directory / f"{symbol}.c").read_text(encoding="ascii").splitlines()
    data = b"".join(
        ast.literal_eval("b" + line.strip().rstrip(";"))
        for line in lines
        if line.lstrip().startswith('"')
    )
    return marshal.loads(data)


class FreezeShellTests(unittest.TestCase):
    def freeze(self, shell: str | None) -> dict[str, object]:
        with tempfile.TemporaryDirectory() as directory:
            lib, out = Path(directory, "Lib"), Path(directory, "out")
            lib.mkdir()
            (lib / "sub.py").write_bytes(MODULE)
            environ = {} if shell is None else {"TNY_SHELL_PATH": shell}
            with mock.patch.dict(os.environ, environ, clear=True):
                self.assertEqual(freezer.main(["freeze", str(lib), str(out)]), 0)
            namespace: dict[str, object] = {}
            exec(frozen_code(out, "tny_frozen_0"), namespace)
            return namespace

    def test_default_build_keeps_the_stdlib_unchanged(self) -> None:
        for shell in (None, "", "/bin/sh"):
            module = self.freeze(shell)
            self.assertEqual(module["UNIX_SHELL"], "/bin/sh")
            self.assertEqual(module["ANDROID"], "/system/bin/sh")
        self.assertIs(freezer.with_build_shell(MODULE, b"/bin/sh"), MODULE)

    def test_build_shell_replaces_every_host_shell(self) -> None:
        module = self.freeze(STORE_SHELL)
        self.assertEqual(module["UNIX_SHELL"], STORE_SHELL)
        self.assertNotIn("/bin/sh", module["ANDROID"])
        self.assertIn(STORE_SHELL, module["__doc__"])
        rewritten = freezer.with_build_shell(MODULE, STORE_SHELL.encode())
        self.assertNotIn(b"/bin/sh", rewritten)

    def test_shell_must_be_a_plain_absolute_path(self) -> None:
        for shell in ("bash", "/bin/ba'sh", '/bin/ba"sh', "/bin/ba\\sh", "/bin/sh\n"):
            with self.assertRaises(SystemExit):
                freezer.build_shell({"TNY_SHELL_PATH": shell})


if __name__ == "__main__":
    unittest.main()
