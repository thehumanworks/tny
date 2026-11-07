"""The tny package carries the tool-extension API (docs/extensions.md).

Editors resolve `import tny` in extension code to this package, so its
decorators must be the very modules the tny binary embeds for its tool host.
"""

from __future__ import annotations

import unittest
from pathlib import Path

import tny

ROOT = Path(__file__).resolve().parents[3]
PACKAGE = Path(tny.__file__).resolve().parent


class ExtensionApi(unittest.TestCase):
    def test_exports(self) -> None:
        for name in (
            "tool",
            "group",
            "proxy",
            "Param",
            "ToolError",
            "Group",
            "Proxy",
            "Tool",
        ):
            self.assertIn(name, tny.__all__)
            self.assertTrue(hasattr(tny, name), name)

    def test_modules_are_the_embedded_ones(self) -> None:
        source = ROOT / "python" / "tny"
        if not source.is_dir():
            self.skipTest("installed package outside a checkout")
        for module in ("_registry.py", "_schema.py", "_mcp.py"):
            self.assertEqual(
                (source / module).read_bytes(), (PACKAGE / module).read_bytes(), module
            )

    def test_decorators_keep_functions_callable(self) -> None:
        @tny.tool
        def double(x: int) -> int:
            """Double a number."""
            return x * 2

        @tny.tool(name="greet", description="Say hello.")
        def hello(who: str = "world") -> str:
            return f"hello {who}"

        self.assertEqual(4, double(2))
        self.assertEqual("hello ada", hello("ada"))
        self.assertEqual("greet", hello.__tny_tool__.name)  # type: ignore[attr-defined]
        self.assertIsInstance(tny.ToolError("x"), Exception)


if __name__ == "__main__":
    unittest.main()
