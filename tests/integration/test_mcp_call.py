#!/usr/bin/env python3
"""CLI contract for `tny mcp call SERVER/TOOL` (issue #97, ADR 0057).

Arguments ride stdin, the result is one `mcp_call` object with --json, and the
exit code separates config mistakes (1) from a call the server refused (2).
"""

from __future__ import annotations

import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TNY = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", "build/tny"))
if not TNY.is_absolute():
    TNY = (ROOT / TNY).resolve()

FAKE_SERVER = r"""#!/bin/sh
n=0
while IFS= read -r line; do
  case "$line" in
  *'"method":"initialize"'*) n=$((n+1));
    printf '{"jsonrpc":"2.0","id":%s,"result":{"protocolVersion":"2025-06-18"}}\n' "$n" ;;
  *'"method":"tools/list"'*) n=$((n+1));
    printf '{"jsonrpc":"2.0","id":%s,"result":{"tools":[{"name":"echo","description":"Echo"}]}}\n' "$n" ;;
  *'"method":"tools/call"'*) n=$((n+1));
    case "$line" in
    *'"name":"boom"'*)
      printf '{"jsonrpc":"2.0","id":%s,"result":{"isError":true,"content":[{"type":"text","text":"server said no"}]}}\n' "$n" ;;
    *)
      # quotes would break the JSON we are hand-printing: echo the request
      # back with ' in place of " so the test can still see the arguments
      seen=$(printf '%s' "$line" | tr '"' "'")
      printf '{"jsonrpc":"2.0","id":%s,"result":{"content":[{"type":"text","text":"ARGS %s"}]}}\n' "$n" "$seen" ;;
    esac ;;
  esac
done
"""


class McpCallCliTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="tny-mcp-call-")
        self.home = Path(self.tmp.name)
        self.workspace = self.home / "workspace"
        self.workspace.mkdir()
        (self.home / ".tny").mkdir()
        self.server = self.home / "fake-mcp.sh"
        self.server.write_text(FAKE_SERVER, encoding="utf-8")
        self.server.chmod(0o755)
        (self.home / ".tny" / "mcp.json").write_text(
            json.dumps({"servers": {"srv": {"command": [str(self.server)]}}}),
            encoding="utf-8",
        )
        self.write_settings({})

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def write_settings(self, value: object) -> None:
        (self.home / ".tny" / "settings.json").write_text(
            json.dumps(value), encoding="utf-8"
        )

    def run_call(self, *args: str, stdin: str = "") -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env["HOME"] = str(self.home)
        return subprocess.run(
            [str(TNY), *args],
            cwd=self.workspace,
            env=env,
            input=stdin,
            text=True,
            capture_output=True,
            check=False,
            timeout=60,
        )

    def test_arguments_ride_stdin_and_result_reaches_stdout(self) -> None:
        result = self.run_call("mcp", "call", "srv/echo", stdin='{"path":"a b/c"}')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("'path':'a b/c'", result.stdout)
        self.assertEqual(result.stderr, "")

    def test_json_emits_one_mcp_call_object(self) -> None:
        result = self.run_call("--json", "mcp", "call", "srv/echo", stdin="{}")
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["kind"], "mcp_call")
        self.assertEqual(payload["server"], "srv")
        self.assertEqual(payload["tool"], "echo")
        self.assertTrue(payload["ok"])
        self.assertFalse(payload["truncated"])

    def test_empty_stdin_means_no_arguments(self) -> None:
        result = self.run_call("mcp", "call", "srv/echo", stdin="")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("'arguments':{}", result.stdout)

    def test_tool_error_is_exit_2_on_stderr(self) -> None:
        result = self.run_call("mcp", "call", "srv/boom", stdin="{}")
        self.assertEqual(result.returncode, 2)
        self.assertIn("server said no", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_unknown_server_and_bad_stdin_are_exit_1(self) -> None:
        unknown = self.run_call("mcp", "call", "ghost/echo", stdin="{}")
        self.assertEqual(unknown.returncode, 1)
        self.assertIn("no MCP server named ghost", unknown.stderr)

        malformed = self.run_call("mcp", "call", "srv/echo", stdin="{oops")
        self.assertEqual(malformed.returncode, 1)
        self.assertIn("JSON object", malformed.stderr)

        missing = self.run_call("mcp", "call")
        self.assertEqual(missing.returncode, 1)
        self.assertIn("SERVER/TOOL", missing.stderr)

    def test_ask_mode_without_a_rule_fails_closed(self) -> None:
        self.write_settings({"permission_mode": "ask"})
        denied = self.run_call("mcp", "call", "srv/echo", stdin="{}")
        self.assertEqual(denied.returncode, 2)
        self.assertIn("mcp:srv/echo", denied.stderr)

        self.write_settings(
            {"permission_mode": "ask", "permission": {"mcp:srv/echo": "allow"}}
        )
        allowed = self.run_call("mcp", "call", "srv/echo", stdin="{}")
        self.assertEqual(allowed.returncode, 0, allowed.stderr)
        self.assertIn("ARGS", allowed.stdout)

    def test_large_result_spills_to_a_0600_file(self) -> None:
        big = json.dumps({"pad": "x" * 40000})
        result = self.run_call("--json", "mcp", "call", "srv/echo", stdin=big)
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["truncated"])
        self.assertGreater(payload["bytes"], 32768)
        spilled = Path(payload["result_file"])
        self.assertTrue(spilled.is_file())
        self.assertEqual(stat.S_IMODE(spilled.stat().st_mode) & 0o077, 0)
        self.assertIn("ARGS", spilled.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
