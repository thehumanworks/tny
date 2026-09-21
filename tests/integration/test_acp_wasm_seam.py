#!/usr/bin/env python3
"""Host-link the real ACP wasm process seam and verify its fail-closed path.

This is deliberately NOT an emcc build or wasm execution claim. It links every
ordinary native release object except acp_proc.o, replacing that one process seam
with the unchanged acp_proc_wasm.c. Neither help nor an attempted turn may spawn.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class AcpWasmSeam(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        configured = os.environ.get("TNY_ACP_WASM_SEAM_BIN")
        if configured:
            cls.binary = Path(configured).resolve()
        else:
            build_env = dict(os.environ)
            for key in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL"):
                build_env.pop(key, None)
            result = subprocess.run(
                ["make", "acp-wasm-seam-fixture"],
                cwd=ROOT,
                env=build_env,
                capture_output=True,
                text=True,
                timeout=300,
                check=False,
            )
            if result.returncode:
                raise AssertionError(
                    f"host wasm seam fixture build failed: {result.returncode}\n"
                    + result.stdout[-6000:]
                    + result.stderr[-6000:]
                )
            cls.binary = (
                ROOT
                / "build"
                / ("tny-acp-wasm-seam.exe" if os.name == "nt" else "tny-acp-wasm-seam")
            )

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="tny-acp-wasm-seam-")
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.marker = self.home / "agent-spawned"
        self.agent = self.home / "must-not-spawn.py"
        self.agent.write_text(
            "import pathlib, sys\npathlib.Path(sys.argv[1]).write_text('spawned')\n"
        )
        self.env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith(("TNY_", "OPENAI_", "CODEX_", "ACP_FIXTURE_"))
        }
        self.env.update(
            HOME=str(self.home),
            TNY_SETTINGS_PATH=str(self.home / "settings.json"),
            TNY_SELF_IMPROVE="0",
            TNY_ISOLATE="0",
        )
        self.command = [
            str(self.binary),
            "--cwd",
            str(self.home),
            "--provider",
            "acp",
            "--agent",
            sys.executable,
            "--",
            str(self.agent),
            str(self.marker),
            "--",
        ]

    def invoke(self, args):
        return subprocess.run(
            self.command + args,
            cwd=self.home,
            env=self.env,
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )

    def test_help_and_version_do_not_spawn(self):
        for option in ("--help", "--version"):
            with self.subTest(option=option):
                result = self.invoke([option])
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertFalse(self.marker.exists())

    def test_turn_and_model_catalog_fail_before_spawn(self):
        for args in (["--ephemeral", "ask", "hello"], ["models"]):
            with self.subTest(command=args):
                result = self.invoke(args)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(
                    "external agent processes are unsupported on WebAssembly",
                    result.stdout + result.stderr,
                )
                self.assertFalse(self.marker.exists())
                self.assertFalse((self.home / "mcp.sock").exists())

    def test_capability_mask_matches_selected_process_seam(self):
        suffix = ".exe" if os.name == "nt" else ""
        for seam, available in (("native", 9), ("wasm", 1)):
            probe = self.binary.parent / f"acp-{seam}-capabilities{suffix}"
            for provider, selected in (("openai", 1), ("acp", 4)):
                with self.subTest(seam=seam, provider=provider):
                    result = subprocess.run(
                        [str(probe), str(self.home), provider],
                        cwd=self.home,
                        env=self.env,
                        capture_output=True,
                        text=True,
                        timeout=15,
                        check=False,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(
                        json.loads(result.stdout),
                        {
                            "available": available,
                            "selected": selected,
                            "initialized": 0,
                        },
                    )
                    self.assertFalse(self.marker.exists())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
