#!/usr/bin/env python3
"""Unit tests for the embedded ``tny`` package (python/tny, ADR 0218).

These pin the contract an extension author relies on: the schema inferred
from a signature, argument binding and its error text, naming (collapse,
groups, shadowing), failure isolation, and the stdio MCP proxy. Every schema
test runs twice, with pydantic and with the built-in mapper, so both paths
publish the same shape. The binary-level surfaces are covered by
tests/integration/test_tool_extensions.py.
"""

from __future__ import annotations

import dataclasses
import enum
import io
import json
import os
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from typing import Annotated, Literal, Optional, TypedDict

ROOT = Path(__file__).resolve().parents[2]
# python/ first: sdk/python also ships a package called tny.
sys.path.insert(0, str(ROOT / "python"))

import tny  # noqa: E402
from tny import _host, _registry, _schema  # noqa: E402

HAVE_PYDANTIC = _schema.pydantic is not None
FAKE_SERVER = Path(__file__).with_name("fake_mcp_server.py")


class Color(enum.Enum):
    RED = "red"
    BLUE = "blue"


@dataclasses.dataclass
class Point:
    x: int
    y: int = 0


class Opts(TypedDict):
    verbose: bool


if HAVE_PYDANTIC:
    import pydantic

    class Issue(pydantic.BaseModel):
        title: str
        labels: list[str] = []


def compile_fn(fn, description=None, schema=None):
    return _schema.Compiled(fn, description, schema)


class SchemaCases:
    """Mixed into one TestCase per mode (pydantic / built-in)."""

    use_pydantic = False

    def setUp(self) -> None:
        self._saved = _schema.pydantic
        if not self.use_pydantic:
            _schema.pydantic = None

    def tearDown(self) -> None:
        _schema.pydantic = self._saved

    def test_primitives_defaults_and_required(self) -> None:
        def f(name: str, count: int = 2, ratio: float = 0.5, on: bool = False):
            """Do f."""

        c = compile_fn(f)
        s = c.schema
        self.assertEqual(c.description, "Do f.")
        self.assertEqual(s["type"], "object")
        self.assertEqual(s["required"], ["name"])
        self.assertFalse(s["additionalProperties"])
        p = s["properties"]
        self.assertEqual(p["name"]["type"], "string")
        self.assertEqual((p["count"]["type"], p["count"]["default"]), ("integer", 2))
        self.assertEqual(p["ratio"]["type"], "number")
        self.assertEqual((p["on"]["type"], p["on"]["default"]), ("boolean", False))
        self.assertNotIn("title", json.dumps(s))

    def test_google_numpy_and_sphinx_docstrings(self) -> None:
        def g(a: str, b: int = 1):
            """Summary line.

            More text.

            Args:
                a: The a.
                b (int): The b.
            """

        def n(a: str):
            """Summary.

            Parameters
            ----------
            a : str
                The a.
            """

        def s(a: str):
            """Summary.

            :param a: The a.
            """

        for fn in (g, n, s):
            with self.subTest(fn=fn.__name__):
                c = compile_fn(fn)
                self.assertTrue(c.description.startswith("Summary"))
                self.assertEqual(c.schema["properties"]["a"]["description"], "The a.")
        self.assertEqual(
            compile_fn(g).schema["properties"]["b"]["description"], "The b."
        )

    def test_annotated_param_literal_enum_optional(self) -> None:
        def f(
            key: Annotated[str, tny.Param(description="Project key")],
            note: Annotated[str, "Free text"] = "",
            mode: Literal["fast", "slow"] = "fast",
            color: Color = Color.RED,
            limit: Optional[int] = None,
        ):
            pass

        p = compile_fn(f).schema["properties"]
        self.assertEqual(p["key"]["description"], "Project key")
        self.assertEqual(p["note"]["description"], "Free text")
        self.assertEqual(p["mode"]["enum"], ["fast", "slow"])
        self.assertEqual(p["color"]["enum"], ["red", "blue"])
        self.assertEqual(p["color"]["default"], "red")
        limit = json.dumps(p["limit"])
        self.assertIn("integer", limit)
        self.assertIn("null", limit)

    def test_containers_dataclass_typeddict(self) -> None:
        def f(tags: list[str], counts: dict[str, int], at: Point, opts: Opts):
            pass

        p = compile_fn(f).schema["properties"]
        self.assertEqual(p["tags"], {"type": "array", "items": {"type": "string"}})
        self.assertEqual(p["counts"]["type"], "object")
        self.assertEqual(p["counts"]["additionalProperties"], {"type": "integer"})
        self.assertEqual(p["at"]["type"], "object")
        self.assertEqual(p["at"]["required"], ["x"])
        self.assertEqual(p["opts"]["properties"]["verbose"]["type"], "boolean")
        self.assertNotIn("$ref", json.dumps(p))

    def test_explicit_schema_wins(self) -> None:
        schema = {"type": "object", "properties": {"q": {"type": "string"}}}
        c = compile_fn(lambda **kw: kw, "desc", schema)
        self.assertEqual(c.schema, schema)
        self.assertEqual(c.bind({"q": "x", "extra": 1}), {"q": "x", "extra": 1})

    def test_bind_converts_and_reports(self) -> None:
        def f(at: Point, color: Color = Color.RED, n: int = 0):
            pass

        c = compile_fn(f)
        kwargs = c.bind({"at": {"x": 1}, "color": "blue", "n": 3})
        self.assertEqual(kwargs["at"], Point(1, 0))
        self.assertIs(kwargs["color"], Color.BLUE)
        self.assertEqual(kwargs["n"], 3)
        for bad, needle in (
            ({}, "at"),
            ({"at": {"x": 1}, "n": "many"}, "n"),
            ({"at": {"x": 1}, "color": "green"}, "color"),
            ({"at": {"x": 1}, "zzz": 1}, "zzz"),
        ):
            with self.subTest(bad=bad):
                with self.assertRaises(tny.ToolError) as cm:
                    c.bind(bad)
                self.assertIn(needle, str(cm.exception))


class BuiltinSchemaTest(SchemaCases, unittest.TestCase):
    use_pydantic = False


@unittest.skipUnless(HAVE_PYDANTIC, "pydantic not installed")
class PydanticSchemaTest(SchemaCases, unittest.TestCase):
    use_pydantic = True

    def test_pydantic_model_parameter(self) -> None:
        def f(issue: Issue):
            pass

        c = compile_fn(f)
        p = c.schema["properties"]["issue"]
        self.assertEqual(p["properties"]["title"]["type"], "string")
        self.assertIsInstance(c.bind({"issue": {"title": "t"}})["issue"], Issue)


class RegistryTest(unittest.TestCase):
    def setUp(self) -> None:
        _registry.reset()

    def tearDown(self) -> None:
        _registry.reset()

    def test_decorator_keeps_function_callable(self) -> None:
        @tny.tool
        def add(a: int, b: int) -> int:
            return a + b

        self.assertEqual(add(2, 3), 5)
        self.assertEqual(add.__tny_tool__.name, "add")

    def test_names_are_checked_and_unique(self) -> None:
        g = _registry.Group("ext")
        g.tool(lambda: 1, name="one")
        with self.assertRaises(ValueError):
            g.tool(lambda: 1, name="one")
        with self.assertRaises(ValueError):
            g.tool(lambda: 1, name="has.dot")
        with self.assertRaises(ValueError):
            g.group("one")

    def test_proxy_needs_one_transport(self) -> None:
        with self.assertRaises(ValueError):
            _registry.Proxy("p")
        with self.assertRaises(ValueError):
            _registry.Proxy("p", command=["x"], url="http://x")
        with self.assertRaises(ValueError):
            _registry.Proxy("p", command="not-a-list")  # type: ignore[arg-type]


class HostTest(unittest.TestCase):
    """Discovery, naming and calls through a real Host on temp directories."""

    def setUp(self) -> None:
        _registry.reset()
        for name in [m for m in sys.modules if m.startswith("_tny_tools")]:
            del sys.modules[name]
        self.tmp = tempfile.TemporaryDirectory(prefix="tny-tool-ext-")
        self.project = Path(self.tmp.name) / "project"
        self.global_ = Path(self.tmp.name) / "global"
        self.project.mkdir()
        self.global_.mkdir()

    def tearDown(self) -> None:
        _registry.reset()
        self.tmp.cleanup()

    def write(self, base: Path, rel: str, text: str) -> None:
        path = base / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(text))

    def host(self) -> _host.Host:
        return _host.Host(
            [("project", str(self.project)), ("global", str(self.global_))]
        )

    def names(self, host: _host.Host) -> list[str]:
        return sorted(t["name"] for t in host.catalog()["tools"])

    def test_single_tool_module_collapses_to_module_name(self) -> None:
        self.write(
            self.global_,
            "call_advisor.py",
            """
            import tny
            @tny.tool
            def ask(question: str) -> str:
                "Ask."
                return "a:" + question
        """,
        )
        h = self.host()
        self.assertEqual(self.names(h), ["call_advisor"])
        self.assertEqual(
            h.call("call_advisor", {"question": "q"}), ("a:q", False, None)
        )

    def test_folder_module_groups_and_submodules(self) -> None:
        self.write(
            self.project,
            "jira/__main__.py",
            """
            import tny
            from . import projects
            @tny.tool
            def whoami() -> str:
                return "me"
        """,
        )
        self.write(
            self.project,
            "jira/projects.py",
            """
            import tny
            @tny.tool(name="list")
            def list_projects() -> list:
                return ["A"]
            issues = tny.group("issues")
            @issues.tool
            def close(key: str) -> str:
                return key
        """,
        )
        h = self.host()
        self.assertEqual(
            self.names(h),
            ["jira.projects.issues.close", "jira.projects.list", "jira.whoami"],
        )
        text, err, structured = h.call("jira.projects.list", {})
        self.assertFalse(err)
        self.assertEqual(json.loads(text), ["A"])

    def test_project_shadows_global_with_warning(self) -> None:
        for base, value in ((self.project, "project"), (self.global_, "global")):
            self.write(
                base,
                "same.py",
                f"""
                import tny
                @tny.tool
                def same() -> str:
                    return "{value}"
            """,
            )
        h = self.host()
        self.assertEqual(h.call("same", {})[0], "project")
        self.assertTrue(any("shadows" in w["warning"] for w in h.catalog()["warnings"]))

    def test_broken_module_is_isolated(self) -> None:
        self.write(self.global_, "broken.py", "raise RuntimeError('boom')\n")
        self.write(
            self.global_,
            "fine.py",
            """
            import tny
            @tny.tool
            def fine() -> str:
                return "ok"
        """,
        )
        cat = self.host().catalog()
        self.assertEqual([t["name"] for t in cat["tools"]], ["fine"])
        self.assertEqual(len(cat["errors"]), 1)
        self.assertEqual(cat["errors"][0]["module"], "broken")
        self.assertIn("boom", cat["errors"][0]["error"])

    def test_hidden_and_non_modules_are_skipped(self) -> None:
        self.write(self.global_, "_private.py", "raise SystemExit(1)\n")
        self.write(self.global_, ".hidden.py", "raise SystemExit(1)\n")
        self.write(self.global_, "notes.txt", "x")
        self.write(self.global_, "folder/helper.py", "x = 1\n")  # no __main__.py
        cat = self.host().catalog()
        self.assertEqual(cat["tools"], [])
        self.assertEqual(cat["errors"], [])

    def test_errors_prints_and_results(self) -> None:
        self.write(
            self.global_,
            "t.py",
            """
            import tny
            @tny.tool
            def refuse() -> str:
                raise tny.ToolError("nope")
            @tny.tool
            def crash() -> str:
                return 1 / 0
            @tny.tool
            def talk() -> None:
                print("printed")
            @tny.tool
            def data() -> dict:
                return {"a": 1}
            @tny.tool
            async def later() -> str:
                return "async"
        """,
        )
        h = self.host()
        self.assertEqual(h.call("t.refuse", {}), ("nope", True, None))
        text, err, _ = h.call("t.crash", {})
        self.assertTrue(err)
        self.assertTrue(text.startswith("ZeroDivisionError:"), text)
        self.assertIn("t.py:", text)
        self.assertEqual(h.call("t.talk", {}), ("printed\n", False, None))
        text, err, structured = h.call("t.data", {})
        self.assertEqual(
            (json.loads(text), err, structured), ({"a": 1}, False, {"a": 1})
        )
        self.assertEqual(h.call("t.later", {})[0], "async")
        self.assertTrue(h.call("t.unknown", {})[1])

    def test_validation_error_is_a_tool_error(self) -> None:
        self.write(
            self.global_,
            "v.py",
            """
            import tny
            @tny.tool
            def v(n: int) -> int:
                return n
        """,
        )
        text, err, _ = self.host().call("v", {"n": "x"})
        self.assertTrue(err)
        self.assertIn("n", text)

    def test_proxy_over_stdio(self) -> None:
        self.write(
            self.global_,
            "fake.py",
            f"""
            import sys, tny
            tny.proxy([sys.executable, {str(FAKE_SERVER)!r}], exclude=["secret"],
                      rename={{"fail": "broken"}})
        """,
        )
        h = self.host()
        self.assertEqual(self.names(h), ["fake.broken", "fake.echo"])
        self.assertEqual(h.call("fake.echo", {"text": "hi"})[:2], ("echo:hi", False))
        self.assertEqual(h.call("fake.broken", {})[:2], ("it failed", True))
        for p in _host._proxies(_registry.roots()["_tny_tools.fake"]):
            if p.client:
                p.client.close()

    def test_proxy_start_failure_is_reported(self) -> None:
        self.write(
            self.global_,
            "gone.py",
            """
            import tny
            tny.proxy(["/nonexistent/server"])
        """,
        )
        cat = self.host().catalog()
        self.assertEqual(cat["tools"], [])
        self.assertTrue(cat["errors"][0]["error"].startswith("proxy:"))

    def test_server_speaks_newline_mcp(self) -> None:
        self.write(
            self.global_,
            "s.py",
            """
            import tny
            @tny.tool
            def s(x: int = 1) -> int:
                "Square."
                return x * x
        """,
        )
        out = io.BytesIO()
        server = _host.Server(self.host(), out)
        lines = [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "tools/call",
                "params": {"name": "s", "arguments": {"x": 3}},
            },
            {"jsonrpc": "2.0", "id": 4, "method": "bogus"},
        ]
        server.serve(
            io.StringIO("\n".join(json.dumps(m) for m in lines) + "\nnot json\n")
        )
        replies = [json.loads(line) for line in out.getvalue().decode().splitlines()]
        self.assertEqual([r.get("id") for r in replies], [1, 2, 3, 4, None])
        self.assertEqual(replies[1]["result"]["tools"][0]["name"], "s")
        self.assertEqual(replies[2]["result"]["content"][0]["text"], "9")
        self.assertEqual(replies[3]["error"]["code"], -32601)
        self.assertEqual(replies[4]["error"]["code"], -32700)


if __name__ == "__main__":
    os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
    unittest.main(verbosity=2)
