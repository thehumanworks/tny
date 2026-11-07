#!/usr/bin/env python3
"""Process-level contract for `tny tool`, `tny memory` and `tny skill` (docs/cli.md, ADR 0217).

Fixture-only: no provider, network or credentials are used.
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
TNY = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", "build/tny"))
if not TNY.is_absolute():
    TNY = ROOT / TNY
WASM = "wasm" in TNY.parts


@unittest.skipIf(WASM, "native process contract")
class ToolCliTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="tny-tool-cli-")
        base = Path(self.temp.name)
        self.ws = base / "ws"
        self.home = base / "home"
        self.ws.mkdir()
        self.home.mkdir()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("TNY_")}
        for key in list(self.env):
            if key.endswith("_API_KEY") or key.endswith("_BASE_URL"):
                del self.env[key]
        self.env["HOME"] = str(self.home)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def tny(
        self, *args: str, stdin: str | None = None
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(TNY), *args],
            input=stdin,
            text=True,
            capture_output=True,
            cwd=self.ws,
            env=self.env,
            timeout=60,
            stdin=None if stdin is not None else subprocess.DEVNULL,
        )

    def test_list_and_describe_use_the_nested_catalog(self) -> None:
        listed = self.tny("tool", "list", "--json")
        self.assertEqual(0, listed.returncode, listed.stderr)
        names = [item["function"]["name"] for item in json.loads(listed.stdout)]
        for name in (
            "read_file",
            "write_file",
            "grep_files",
            "terminal",
            "memory",
            "skill",
        ):
            self.assertIn(name, names)
        # Turn-bound tools are not offered outside a turn.
        for name in ("read_image", "ask_user_question", "read_tool_result"):
            self.assertNotIn(name, names)
        text = self.tny("tool", "list")
        self.assertEqual(0, text.returncode, text.stderr)
        self.assertRegex(text.stdout, r"(?m)^read_file\s+Read a text file$")

        described = self.tny("tool", "describe", "file_info")
        self.assertEqual(0, described.returncode, described.stderr)
        self.assertEqual("file_info", json.loads(described.stdout)["name"])
        missing = self.tny("tool", "describe", "read_image")
        self.assertEqual(2, missing.returncode)

    def test_call_round_trips_files_through_the_shared_tools(self) -> None:
        wrote = self.tny(
            "tool", "call", "write_file", '{"path":"a.txt","content":"x TODO\\n"}'
        )
        self.assertEqual(0, wrote.returncode, wrote.stderr)
        self.assertEqual("x TODO\n", (self.ws / "a.txt").read_text())

        piped = self.tny("tool", "call", "read_file", "-", stdin='{"path":"a.txt"}')
        self.assertEqual(0, piped.returncode, piped.stderr)
        self.assertIn("x TODO", piped.stdout)

        grep = self.tny("tool", "call", "grep_files", '{"pattern":"TODO"}', "--json")
        self.assertEqual(0, grep.returncode, grep.stderr)
        envelope = json.loads(grep.stdout)
        self.assertEqual(
            {"kind": "tool", "name": "grep_files", "ok": True},
            {k: envelope[k] for k in ("kind", "name", "ok")},
        )
        self.assertIn("a.txt:1:x TODO", envelope["result"])

    def test_errors_have_distinct_exit_codes(self) -> None:
        bad_json = self.tny("tool", "call", "read_file", "not json")
        self.assertEqual(1, bad_json.returncode)
        self.assertIn("one JSON object", bad_json.stderr)
        unknown = self.tny("tool", "call", "no_such_tool", "{}", "--json")
        self.assertEqual(2, unknown.returncode)
        self.assertFalse(json.loads(unknown.stdout)["ok"])
        missing = self.tny("tool", "call", "read_file", '{"path":"nope.txt"}')
        self.assertEqual(2, missing.returncode)
        self.assertTrue(missing.stderr.startswith("error:"), missing.stderr)
        turn = self.tny("tool", "call", "read_image", '{"path":"a.png"}')
        self.assertEqual(2, turn.returncode)
        self.assertIn("conversation turn", turn.stderr)
        usage = self.tny("tool", "frobnicate")
        self.assertEqual(1, usage.returncode)

    def test_permission_rules_still_apply(self) -> None:
        settings = self.home / ".tny"
        settings.mkdir()
        (settings / "settings.json").write_text(
            json.dumps(
                {"permission_mode": "ask", "permission": {"edit": {"*deny*": "deny"}}}
            )
        )
        # ask mode with no reviewer fails closed, as for tny ask.
        blocked = self.tny(
            "tool", "call", "write_file", '{"path":"b.txt","content":"no"}'
        )
        self.assertEqual(2, blocked.returncode, blocked.stdout)
        self.assertIn("permission required for write_file", blocked.stderr)
        self.assertFalse((self.ws / "b.txt").exists())
        denied = self.tny(
            "tool", "call", "write_file", '{"path":"deny.txt","content":"no"}'
        )
        self.assertEqual(2, denied.returncode, denied.stdout)
        self.assertIn("permission denied for write_file", denied.stderr)
        # Safe reads stay free in ask mode.
        (self.ws / "c.txt").write_text("ok\n")
        read = self.tny("tool", "call", "read_file", '{"path":"c.txt"}')
        self.assertEqual(0, read.returncode, read.stderr)

    def test_memory_and_skill_work_outside_a_turn(self) -> None:
        self.assertEqual(0, self.tny("memory", "set", "colour", "blue").returncode)
        self.assertEqual(
            0, self.tny("memory", "set", "piped", stdin="from stdin").returncode
        )
        got = self.tny("memory", "get", "colour")
        self.assertEqual(("blue\n", 0), (got.stdout, got.returncode), got.stderr)
        self.assertEqual("from stdin\n", self.tny("memory", "get", "piped").stdout)
        listed = self.tny("memory", "list")
        self.assertIn("colour: blue", listed.stdout)
        self.assertEqual(1, self.tny("memory", "frob").returncode)

        skill_dir = self.ws / "skills" / "review"
        skill_dir.mkdir(parents=True)
        (skill_dir / "SKILL.md").write_text(
            "---\nname: review\ndescription: Review code\n---\nCheck the diff.\n"
        )
        shown = self.tny("skill", "show", "review")
        self.assertEqual(0, shown.returncode, shown.stderr)
        self.assertIn("Check the diff.", shown.stdout)
        self.assertEqual(2, self.tny("skill", "show", "absent").returncode)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
