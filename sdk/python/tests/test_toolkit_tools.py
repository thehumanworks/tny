"""Toolkit.list_tools / call_tool: embedded native tools (ADR 0223)."""

from __future__ import annotations

import asyncio
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

import tny

ROOT = Path(__file__).resolve().parents[3]
LIBRARY = os.environ.get(
    "TNY_TEST_LIBRARY",
    str(
        ROOT
        / "build/lib"
        / ("libtny.1.dylib" if sys.platform == "darwin" else "libtny.so.1")
    ),
)


class ToolkitToolTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name).resolve()
        (self.path / "settings.json").write_text("{}")
        self.config = tny.ToolkitConfig(
            workspace=self.path, settings_path="settings.json"
        )
        self.toolkit = tny.Toolkit(self.config, library=LIBRARY)

    def test_catalog_is_the_embedded_nested_catalog(self):
        tools = {spec.name: spec for spec in self.toolkit.list_tools()}
        for name in ("read_file", "write_file", "edit_file", "grep_files", "web_fetch"):
            self.assertIn(name, tools)
        self.assertEqual("object", tools["read_file"].parameters["type"])
        self.assertTrue(tools["read_file"].description)
        # Turn-bound and process-owning tools are not offered (ADR 0215/0217).
        for name in ("read_image", "ask_user_question", "terminal", "subagent"):
            self.assertNotIn(name, tools)

    def test_file_tools_round_trip_in_the_workspace(self):
        wrote = self.toolkit.call_tool(
            "write_file", {"path": "notes.txt", "content": "ship TODO\n"}
        )
        self.assertEqual(("write_file", True), (wrote.name, wrote.ok))
        self.assertEqual("ship TODO\n", (self.path / "notes.txt").read_text())
        found = self.toolkit.call_tool("grep_files", {"pattern": "TODO"})
        self.assertTrue(found.ok, found.text)
        self.assertIn("notes.txt:1:ship TODO", found.text)
        edited = self.toolkit.call_tool(
            "edit_file",
            {"path": "notes.txt", "old_string": "TODO", "new_string": "done"},
        )
        self.assertTrue(edited.ok, edited.text)
        self.assertEqual("ship done\n", (self.path / "notes.txt").read_text())

    def test_tool_errors_are_results_not_exceptions(self):
        missing = self.toolkit.call_tool("read_file", {"path": "absent.txt"})
        self.assertFalse(missing.ok)
        self.assertTrue(missing.text.startswith("error:"), missing.text)
        for name in ("terminal", "read_image", "no_such_tool"):
            with self.subTest(name=name):
                result = self.toolkit.call_tool(name, {})
                self.assertFalse(result.ok, result.text)

    def test_invalid_requests_raise_before_any_tool_runs(self):
        for name, arguments in ((1, None), ("read_file", ["x"]), ("", None)):
            with self.subTest(name=name), self.assertRaises(tny.InvalidArgumentError):
                self.toolkit.call_tool(name, arguments)  # type: ignore[arg-type]
        with self.assertRaises(tny.InvalidArgumentError):
            self.toolkit.call_tool("read_file", {}, permission_mode="never")  # type: ignore[arg-type]

    def test_permission_mode_and_settings_rules_apply(self):
        asked = self.toolkit.call_tool(
            "write_file", {"path": "a.txt", "content": "x"}, permission_mode="ask"
        )
        self.assertFalse(asked.ok)
        self.assertIn("permission required for write_file", asked.text)
        self.assertFalse((self.path / "a.txt").exists())
        # Settings choose the mode when the call does not; rules then apply.
        (self.path / "settings.json").write_text(
            json.dumps(
                {"permission_mode": "auto", "permission": {"edit": {"*deny*": "deny"}}}
            )
        )
        denied = self.toolkit.call_tool(
            "write_file", {"path": "deny.txt", "content": "x"}
        )
        self.assertFalse(denied.ok)
        self.assertIn("permission denied for write_file", denied.text)
        allowed = self.toolkit.call_tool(
            "write_file", {"path": "ok.txt", "content": "x"}
        )
        self.assertTrue(allowed.ok, allowed.text)
        # Reads stay free in ask mode.
        read = self.toolkit.call_tool(
            "read_file", {"path": "ok.txt"}, permission_mode="ask"
        )
        self.assertEqual("x", read.text.strip())

    def test_pre_cancelled_call_runs_nothing(self):
        token = tny.CancellationToken()
        token.cancel()
        with self.assertRaises(tny.CancelledError):
            self.toolkit.call_tool(
                "write_file", {"path": "c.txt", "content": "x"}, cancellation=token
            )
        self.assertFalse((self.path / "c.txt").exists())

    def test_async_toolkit_matches(self):
        toolkit = tny.AsyncToolkit(self.config, library=LIBRARY)

        async def run():
            names = [spec.name for spec in await toolkit.list_tools()]
            result = await toolkit.call_tool(
                "write_file", {"path": "async.txt", "content": "a"}
            )
            return names, result

        names, result = asyncio.run(run())
        self.assertIn("read_file", names)
        self.assertTrue(result.ok, result.text)
        self.assertEqual("a", (self.path / "async.txt").read_text())


if __name__ == "__main__":
    unittest.main()
