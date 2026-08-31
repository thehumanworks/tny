#!/usr/bin/env python3
"""CLI contract for opt-in MCP import and source-attributed JSON."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TNY = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", "build/tny"))
if not TNY.is_absolute():
    TNY = (ROOT / TNY).resolve()
FIXTURES = ROOT / "tests" / "fixtures" / "mcp-import"


class McpImportCliTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="tny-mcp-import-")
        self.home = Path(self.tmp.name)
        self.workspace = self.home / "workspace"
        self.workspace.mkdir()
        (self.home / ".tny").mkdir()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_mcp(self) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env["HOME"] = str(self.home)
        env["TNY_TEST_MCP_BIN"] = "/bin/echo"
        return subprocess.run(
            [str(TNY), "mcp", "list", "--json"],
            cwd=self.workspace,
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )

    def write_settings(self, value: object) -> None:
        (self.home / ".tny" / "settings.json").write_text(
            json.dumps(value), encoding="utf-8"
        )

    def test_default_off_does_not_import_project_file(self) -> None:
        project_dir = self.workspace / ".cursor"
        project_dir.mkdir()
        shutil.copy(FIXTURES / "cursor-project.json", project_dir / "mcp.json")
        self.write_settings({})

        result = self.run_mcp()
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["kind"], "mcp_servers")
        self.assertEqual(payload["servers"], [])
        self.assertEqual(result.stderr, "")

    def test_enabled_project_file_is_attributed_and_remote_is_skipped(self) -> None:
        project_dir = self.workspace / ".cursor"
        project_dir.mkdir()
        shutil.copy(FIXTURES / "cursor-project.json", project_dir / "mcp.json")
        self.write_settings({"mcp": {"import_from": ["cursor-agent"]}})

        result = self.run_mcp()
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        by_name = {server["name"]: server for server in payload["servers"]}
        self.assertEqual(by_name["cursor_project"]["source"], "cursor-agent")
        self.assertEqual(by_name["cursor_project"]["scope"], "project")
        self.assertEqual(by_name["cursor_project"]["transport"], "stdio")
        self.assertTrue(by_name["cursor_remote"]["skipped"])
        self.assertIn("unsupported transport", by_name["cursor_remote"]["skip_reason"])

    def test_malformed_enabled_source_warns_but_succeeds(self) -> None:
        cursor_dir = self.home / ".cursor"
        cursor_dir.mkdir()
        shutil.copy(FIXTURES / "malformed.json", cursor_dir / "mcp.json")
        self.write_settings({"mcp": {"import_from": ["cursor-agent"]}})

        result = self.run_mcp()
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["servers"], [])
        self.assertTrue(any("malformed" in notice for notice in payload["notices"]))
        self.assertIn("tny: warning:", result.stderr)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
