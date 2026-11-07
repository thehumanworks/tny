#!/usr/bin/env python3
"""Python tool extensions end to end (docs/extensions.md, ADR 0218).

One extension tree, every surface: `tny tool list|call|check|new`, the
top-level `tny MODULE ...` commands, code mode (`tools.call` and
`tools.jira.projects.list(...)`), `tny mcp serve`, permissions, the off
switches, proxies, failure isolation, the catalog cache and timeouts.
Fixture-only: no provider, network or credentials.
"""

from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TNY = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", "build/tny"))
if not TNY.is_absolute():
    TNY = ROOT / TNY
WASM = "wasm" in TNY.parts
FAKE_SERVER = ROOT / "tests" / "tool_extensions" / "fake_mcp_server.py"
PYTHON = shutil.which("python3") or sys.executable

JIRA_MAIN = '''
"""Jira tools."""
import tny
from . import projects

@tny.tool
def whoami() -> str:
    "Current user."
    return "ada"
'''

JIRA_PROJECTS = '''
import enum, os, time
from typing import Annotated
import tny

class Status(enum.Enum):
    OPEN = "open"
    DONE = "done"

@tny.tool(name="list")
def list_projects(status: Status = Status.OPEN, tags: list[str] | None = None,
                  limit: int = 10, archived: bool = False) -> dict:
    """List projects.

    Args:
        status: Which projects.
        tags: Only projects with these tags.
        limit: At most this many.
        archived: Include archived ones.
    """
    return {"status": status.value, "tags": tags, "limit": limit, "archived": archived}

@tny.tool
def create(key: Annotated[str, tny.Param(description="Project key")], title: str = "") -> str:
    "Create a project."
    return f"created {key} {title}".strip()

@tny.tool
def slow(seconds: float = 30) -> str:
    "Sleep."
    time.sleep(seconds)
    return "woke"

@tny.tool
def pid() -> int:
    "The host's pid."
    return os.getpid()
'''

ADVISOR = '''
import os, tny

marker = os.environ.get("TNY_TEST_IMPORT_MARKER")
if marker:
    with open(marker, "a") as f:
        f.write("imported\\n")

@tny.tool
def call_advisor(question: str, depth: int = 1) -> str:
    """Ask the advisor one question."""
    print("thinking")  # printed text goes to stderr when a value is returned
    return f"advice on {question} at {depth}"
'''


@unittest.skipIf(WASM, "tool extensions need a native tool host (clean error on wasm)")
class ToolExtensionsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="tny-tool-ext-")
        base = Path(self.temp.name)
        self.ws = base / "ws"
        self.home = base / "home"
        self.global_dir = self.home / ".tny" / "extensions" / "tools"
        self.project_dir = self.ws / ".tny" / "extensions" / "tools"
        self.global_dir.mkdir(parents=True)
        self.project_dir.mkdir(parents=True)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("TNY_")}
        for key in list(self.env):
            if key.endswith("_API_KEY") or key.endswith("_BASE_URL"):
                del self.env[key]
        self.env["HOME"] = str(self.home)
        self.marker = base / "imports.log"
        self.env["TNY_TEST_IMPORT_MARKER"] = str(self.marker)
        self.write(self.global_dir, "call_advisor.py", ADVISOR)
        self.write(self.project_dir, "jira/__main__.py", JIRA_MAIN)
        self.write(self.project_dir, "jira/projects.py", JIRA_PROJECTS)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write(self, base: Path, rel: str, text: str) -> None:
        path = base / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(text))

    def tny(
        self,
        *args: str,
        stdin: str | None = None,
        env: dict | None = None,
        timeout: float = 60,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(TNY), *args],
            input=stdin,
            text=True,
            capture_output=True,
            cwd=self.ws,
            env={**self.env, **(env or {})},
            timeout=timeout,
            stdin=None if stdin is not None else subprocess.DEVNULL,
        )

    def names(self, *flags: str, env: dict | None = None) -> list[str]:
        listed = self.tny(*flags, "tool", "list", "--json", env=env)
        self.assertEqual(0, listed.returncode, listed.stderr)
        return [item["function"]["name"] for item in json.loads(listed.stdout)]

    # -- discovery and naming ------------------------------------------------

    def test_catalog_names_collapse_and_nest(self) -> None:
        names = self.names()
        for name in (
            "call_advisor",
            "jira.whoami",
            "jira.projects.list",
            "jira.projects.create",
        ):
            self.assertIn(name, names)
        self.assertIn("read_file", names)  # native tools stay
        described = self.tny("tool", "describe", "jira.projects.list")
        self.assertEqual(0, described.returncode, described.stderr)
        schema = json.loads(described.stdout)["parameters"]
        self.assertEqual(["open", "done"], schema["properties"]["status"]["enum"])
        self.assertEqual(
            "Which projects.", schema["properties"]["status"]["description"]
        )

    def test_native_tool_names_win(self) -> None:
        self.write(
            self.global_dir,
            "read_file.py",
            """
            import tny
            @tny.tool
            def read_file(path: str) -> str:
                return "shadowed"
        """,
        )
        (self.ws / "a.txt").write_text("real\n")
        call = self.tny("tool", "call", "read_file", '{"path":"a.txt"}')
        self.assertEqual(0, call.returncode, call.stderr)
        self.assertIn("real", call.stdout)
        self.assertEqual(1, self.names().count("read_file"))

    # -- tny tool call / check / new -----------------------------------------

    def test_tool_call_results_and_errors(self) -> None:
        ok = self.tny("tool", "call", "call_advisor", '{"question":"why"}')
        self.assertEqual(0, ok.returncode, ok.stderr)
        self.assertEqual("advice on why at 1\n", ok.stdout)
        self.assertIn("thinking", ok.stderr)
        bad = self.tny(
            "tool", "call", "jira.projects.list", '{"status":"lost"}', "--json"
        )
        self.assertEqual(2, bad.returncode)
        envelope = json.loads(bad.stdout)
        self.assertFalse(envelope["ok"])
        self.assertIn("status", envelope["result"])
        missing = self.tny("tool", "call", "jira.projects.create", "{}")
        self.assertEqual(2, missing.returncode)
        self.assertIn("key", missing.stderr)

    def test_broken_module_is_reported_and_isolated(self) -> None:
        self.write(
            self.global_dir, "broken.py", "import tny\nraise RuntimeError('boom')\n"
        )
        self.assertIn("call_advisor", self.names())
        check = self.tny("tool", "check")
        self.assertEqual(1, check.returncode)
        self.assertIn("broken", check.stderr)
        self.assertIn("boom", check.stderr)
        self.assertIn("Traceback", check.stderr)
        report = json.loads(self.tny("tool", "check", "--json").stdout)
        self.assertEqual(["broken"], [e["module"] for e in report["errors"]])
        (self.global_dir / "broken.py").unlink()
        self.assertEqual(0, self.tny("tool", "check").returncode)

    def test_tool_new_scaffolds_a_working_tool(self) -> None:
        made = self.tny("tool", "new", "greet")
        self.assertEqual(0, made.returncode, made.stderr)
        self.assertTrue((self.project_dir / "greet.py").is_file())
        ran = self.tny("greet", "hi", "--times", "2")
        self.assertEqual(0, ran.returncode, ran.stderr)
        self.assertEqual("hi hi\n", ran.stdout)
        again = self.tny("tool", "new", "greet")
        self.assertEqual(1, again.returncode)
        self.assertEqual(
            1, self.tny("tool", "new", "ask").returncode
        )  # a built-in command
        made = self.tny("tool", "new", "mine", "--global")
        self.assertEqual(0, made.returncode, made.stderr)
        self.assertTrue((self.global_dir / "mine.py").is_file())

    def test_tool_new_folder_scaffolds_a_grouped_module(self) -> None:
        made = self.tny("tool", "new", "greeting", "--folder")
        self.assertEqual(0, made.returncode, made.stderr)
        folder = self.project_dir / "greeting"
        self.assertTrue((folder / "__main__.py").is_file())
        self.assertTrue((folder / "notes.py").is_file())
        self.assertIn("greeting.hello", self.names())
        self.assertIn("greeting.notes.add", self.names())
        ran = self.tny("greeting", "hello", "hi", "--times", "2")
        self.assertEqual(0, ran.returncode, ran.stderr)
        self.assertEqual("hi hi\n", ran.stdout)
        noted = self.tny("greeting", "notes", "add", "milk")
        self.assertEqual(0, noted.returncode, noted.stderr)
        self.assertIn("noted: milk", noted.stdout)
        check = self.tny("tool", "check")
        self.assertEqual(0, check.returncode, check.stdout + check.stderr)
        self.assertEqual(1, self.tny("tool", "new", "greeting").returncode)
        made = self.tny("tool", "new", "--folder", "--global", "kit")
        self.assertEqual(0, made.returncode, made.stderr)
        self.assertTrue((self.global_dir / "kit" / "__main__.py").is_file())

    # -- top-level commands ---------------------------------------------------

    def test_module_commands_take_schema_flags(self) -> None:
        run = self.tny(
            "jira",
            "projects",
            "list",
            "--status",
            "done",
            "--tags",
            "a",
            "--tags",
            "b",
            "--limit=3",
            "--archived",
        )
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(
            {"status": "done", "tags": ["a", "b"], "limit": 3, "archived": True},
            json.loads(run.stdout),
        )
        neg = self.tny("jira", "projects", "list", "--no-archived")
        self.assertFalse(json.loads(neg.stdout)["archived"])
        positional = self.tny("jira", "projects", "create", "TNY", "--title", "Tiny")
        self.assertEqual("created TNY Tiny\n", positional.stdout)
        piped = self.tny(
            "jira",
            "projects",
            "list",
            "-",
            "--limit",
            "5",
            stdin='{"status":"done","limit":1}',
        )
        self.assertEqual(
            {"status": "done", "limit": 5},
            {
                k: v
                for k, v in json.loads(piped.stdout).items()
                if k in ("status", "limit")
            },
        )
        envelope = self.tny("jira", "whoami", "--json")
        self.assertEqual(
            {"kind": "tool", "name": "jira.whoami", "ok": True, "result": "ada"},
            json.loads(envelope.stdout),
        )

    def test_module_command_help_and_usage_errors(self) -> None:
        group = self.tny("jira", "--help")
        self.assertEqual(0, group.returncode, group.stderr)
        self.assertRegex(group.stdout, r"(?m)^  projects list\s+List projects\.$")
        self.assertRegex(group.stdout, r"(?m)^  whoami\s+Current user\.$")
        tool = self.tny("jira", "projects", "list", "--help")
        self.assertEqual(0, tool.returncode)
        self.assertIn("--status open|done", tool.stdout)
        self.assertIn("--archived, --no-archived", tool.stdout)
        self.assertIn("Only projects with these tags.", tool.stdout)
        self.assertEqual(1, self.tny("jira", "projects", "list", "--bogus").returncode)
        self.assertEqual(
            1, self.tny("jira", "projects", "list", "--limit", "x").returncode
        )
        self.assertEqual(1, self.tny("jira", "nope").returncode)
        unknown = self.tny("no_such_module")
        self.assertEqual(1, unknown.returncode)
        self.assertIn("unknown command", unknown.stderr)

    def test_builtin_commands_win_over_modules(self) -> None:
        self.write(
            self.global_dir,
            "status.py",
            """
            import tny
            @tny.tool
            def status() -> str:
                return "from the extension"
        """,
        )
        builtin = self.tny("status", "--json")
        self.assertNotIn("from the extension", builtin.stdout)
        self.assertIn("status", self.names())  # still callable as a tool

    # -- code mode -------------------------------------------------------------

    def test_code_mode_calls_extensions(self) -> None:
        code = "\n".join(
            [
                'print(tools.call("call_advisor", json.dumps({"question": "q"})))',
                'print(tools.jira.projects.list(status="done", limit=2))',
                'print(json.loads(tools.search("jira"))["total"])',
                'print(tools.jira.projects.list(status="nope"))',
            ]
        )
        run = self.tny("tool", "call", "run_code", json.dumps({"code": code}))
        self.assertEqual(0, run.returncode, run.stderr)
        lines = run.stdout.splitlines()
        self.assertEqual("advice on q at 1", lines[0])
        self.assertEqual("done", json.loads(lines[1])["status"])
        self.assertEqual("5", lines[2])
        self.assertTrue(lines[3].startswith("error:"), lines[3])

    # -- MCP -------------------------------------------------------------------

    def test_mcp_serve_exposes_extensions(self) -> None:
        requests = [
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {"protocolVersion": "2025-06-18"},
            },
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "tools/call",
                "params": {"name": "jira_projects_create", "arguments": {"key": "K"}},
            },
            {
                "jsonrpc": "2.0",
                "id": 4,
                "method": "tools/call",
                "params": {"name": "read_file", "arguments": {}},
            },
        ]
        served = self.tny(
            "mcp", "serve", stdin="".join(json.dumps(r) + "\n" for r in requests)
        )
        self.assertEqual(0, served.returncode, served.stderr)
        replies = {r["id"]: r for r in map(json.loads, served.stdout.splitlines())}
        names = [t["name"] for t in replies[2]["result"]["tools"]]
        self.assertIn("jira_projects_list", names)
        self.assertNotIn("read_file", names)  # native tools only with --all
        self.assertEqual("created K", replies[3]["result"]["content"][0]["text"])
        self.assertIn("error", replies[4])
        everything = self.tny(
            "mcp",
            "serve",
            "--all",
            stdin=json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/list"})
            + "\n",
        )
        names = [t["name"] for t in json.loads(everything.stdout)["result"]["tools"]]
        self.assertIn("read_file", names)
        self.assertNotIn("run_code", names)

    def test_proxy_exposes_an_existing_mcp_server(self) -> None:
        self.write(
            self.global_dir,
            "fake.py",
            f"""
            import tny
            tny.proxy([{PYTHON!r}, {str(FAKE_SERVER)!r}], exclude=["secret"])
        """,
        )
        names = self.names()
        self.assertIn("fake.echo", names)
        self.assertNotIn("fake.secret", names)
        echo = self.tny("fake", "echo", "--text", "hi")
        self.assertEqual(0, echo.returncode, echo.stderr)
        self.assertEqual("echo:hi\n", echo.stdout)
        self.assertEqual(2, self.tny("fake", "fail").returncode)

    def test_skill_examples_work(self) -> None:
        """skills/tny-tool-extensions teaches agents; every example must run."""
        shutil.rmtree(self.global_dir)
        shutil.rmtree(self.project_dir)
        self.global_dir.mkdir(parents=True)
        skill = (ROOT / "skills" / "tny-tool-extensions" / "SKILL.md").read_text()
        prefix = "# .tny/extensions/tools/"
        files = {}
        for block in skill.split("```python\n")[1:]:
            code = block.split("```", 1)[0]
            if code.startswith(prefix):
                files[code.splitlines()[0][len(prefix) :]] = code
        self.assertEqual(
            {
                "call_advisor.py",
                "deploy.py",
                "issues.py",
                "jira/__main__.py",
                "jira/projects.py",
                "jira/_client.py",
                "catalog.py",
                "github.py",
                "linear.py",
            },
            set(files),
        )
        for rel, code in files.items():
            compile(code, rel, "exec")  # proxies need real servers: syntax only
            if "tny.proxy(" not in code:
                self.write(self.project_dir, rel, code)
        check = self.tny("tool", "check")
        self.assertEqual(0, check.returncode, check.stdout + check.stderr)
        self.assertEqual(
            [
                "call_advisor",
                "catalog.order",
                "catalog.raw",
                "catalog.search",
                "deploy.start",
                "deploy.status",
                "deploy.wait",
                "issues.labels.add",
                "issues.labels.remove",
                "issues.list",
                "jira.projects.create",
                "jira.projects.list",
                "jira.whoami",
            ],
            sorted(
                n
                for n in self.names()
                if n.split(".")[0]
                in {"call_advisor", "catalog", "deploy", "issues", "jira"}
            ),
        )
        cases = [
            (("call_advisor", "why?", "--depth", "2"), 0, "advice on why? (depth 2)"),
            (("call_advisor", "why?", "--depth", "9"), 2, "depth must be 1, 2 or 3"),
            (("deploy", "status", "--env", "prod"), 0, '"state":"healthy"'),
            (
                (
                    "deploy",
                    "start",
                    "v1.2",
                    "--env",
                    "prod",
                    "--strategy",
                    "blue-green",
                ),
                0,
                "deploying v1.2 to prod (blue-green)",
            ),
            (("deploy", "start", "wip-x", "--env", "prod"), 2, "refusing to deploy"),
            (("deploy", "start", "v1", "--env", "dev"), None, "expected one of"),
            (("deploy", "wait"), 0, "settled"),
            (("issues", "labels", "add", "7", "bug"), 0, "#7 +bug"),
            (("issues", "labels", "remove", "7", "bug"), 0, "#7 -bug"),
            (("jira", "whoami"), 0, "ada"),
            (("jira", "projects", "list"), 0, "TNY"),
            (("jira", "projects", "create", "TNY"), 0, "created TNY"),
            (("catalog", "search", "shoes", "--limit", "2"), 0, "shoes:1"),
            (
                (
                    "tool",
                    "call",
                    "catalog.order",
                    '{"items":[{"sku":"a","quantity":2}]}',
                ),
                0,
                '"count":2',
            ),
            (("tool", "call", "catalog.raw", '{"payload":{"x":1}}'), 0, '"x":1'),
        ]
        for args, code, needle in cases:
            with self.subTest(args=args):
                run = self.tny(*args)
                out = run.stdout + run.stderr
                if code is None:
                    self.assertNotEqual(0, run.returncode, out)
                else:
                    self.assertEqual(code, run.returncode, out)
                self.assertIn(needle, out.replace(" ", "") if needle[0] == '"' else out)

    # -- policy -----------------------------------------------------------------

    def test_permission_rules_match_dotted_names_as_paths(self) -> None:
        (self.home / ".tny" / "settings.json").write_text(
            json.dumps(
                {"permission": {"tool": {"jira/*": "allow", "jira/projects/*": "deny"}}}
            )
        )
        denied = self.tny(
            "--permission-mode", "ask", "tool", "call", "jira.projects.list", "{}"
        )
        self.assertEqual(2, denied.returncode)
        self.assertIn("denied", denied.stderr)
        allowed = self.tny("--permission-mode", "ask", "jira", "whoami")
        self.assertEqual(0, allowed.returncode, allowed.stderr)
        prompt = self.tny("--permission-mode", "ask", "call_advisor", "q")
        self.assertEqual(2, prompt.returncode)  # no rule, no reviewer: fails closed

    def test_off_switches(self) -> None:
        self.assertNotIn("jira.whoami", self.names("--no-project-tools"))
        self.assertIn("call_advisor", self.names("--no-project-tools"))
        self.assertNotIn("call_advisor", self.names("--no-tool-extensions"))
        self.assertNotIn("call_advisor", self.names(env={"TNY_TOOL_EXTENSIONS": "0"}))
        self.assertNotIn(
            "jira.whoami", self.names(env={"TNY_TOOL_EXTENSIONS": "global"})
        )
        (self.home / ".tny" / "settings.json").write_text(
            json.dumps({"extensions": {"tools": {"project": False}}})
        )
        names = self.names()
        self.assertNotIn("jira.whoami", names)
        self.assertIn("call_advisor", names)
        off = self.tny("--no-tool-extensions", "call_advisor", "q")
        self.assertEqual(1, off.returncode)
        self.assertIn("unknown command", off.stderr)

    # -- cache, timeout, cancellation -------------------------------------------

    def test_shipped_examples_work(self) -> None:
        """examples/extensions/tools is what docs/extensions.md points users at."""
        shutil.rmtree(self.global_dir)
        shutil.rmtree(self.project_dir)
        shutil.copytree(ROOT / "examples" / "extensions" / "tools", self.global_dir)
        notes = {"NOTES_FILE": str(Path(self.temp.name) / "notes.json")}
        check = self.tny("tool", "check")
        self.assertEqual(0, check.returncode, check.stdout + check.stderr)
        self.assertEqual(
            ["notes.add", "notes.list", "notes.tags.list", "weather"],
            sorted(n for n in self.names() if n.startswith(("notes", "weather"))),
        )
        weather = self.tny("weather", "Lisbon", "--days", "3")
        self.assertEqual(0, weather.returncode, weather.stderr)
        self.assertIn("Lisbon: sunny for 3 day(s)", weather.stdout)
        bad = self.tny("weather", "Lisbon", "--days", "9")
        self.assertNotEqual(0, bad.returncode)
        self.assertIn("days must be between 1 and 7", bad.stdout + bad.stderr)
        added = self.tny("notes", "add", "buy milk", "--tag", "home", env=notes)
        self.assertEqual(0, added.returncode, added.stderr)
        tags = self.tny("notes", "tags", "list", "--json", env=notes)
        self.assertEqual(0, tags.returncode, tags.stderr)
        self.assertIn("home", tags.stdout)

    def test_catalog_cache_skips_python_until_a_module_changes(self) -> None:
        self.names()
        first = self.marker.read_text().count("imported")
        self.names()
        self.assertEqual(
            first,
            self.marker.read_text().count("imported"),
            "cache hit imported modules",
        )
        cache = list((self.home / ".tny" / "cache" / "tool-extensions").glob("*.json"))
        self.assertEqual(1, len(cache))
        self.write(
            self.global_dir,
            "extra.py",
            """
            import tny
            @tny.tool
            def extra() -> str:
                return "new"
        """,
        )
        self.assertIn("extra", self.names())

    def test_timeout_stops_the_host_and_the_next_call_works(self) -> None:
        start = time.monotonic()
        slow = self.tny("jira", "projects", "slow", env={"TNY_TOOL_TIMEOUT_MS": "700"})
        self.assertLess(time.monotonic() - start, 20)
        self.assertEqual(2, slow.returncode)
        self.assertIn("timed out", slow.stderr)
        self.assertEqual(0, self.tny("jira", "whoami").returncode)

    def test_interrupt_cancels_and_kills_the_host(self) -> None:
        proc = subprocess.Popen(
            [
                str(TNY),
                "tool",
                "call",
                "run_code",
                json.dumps(
                    {
                        "code": "\n".join(
                            [
                                "print(tools.jira.projects.pid(), flush=True)",
                                "print(tools.jira.projects.slow(seconds=60))",
                            ]
                        )
                    }
                ),
            ],
            cwd=self.ws,
            env=self.env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        time.sleep(3)
        proc.send_signal(signal.SIGINT)
        out, err = proc.communicate(timeout=30)
        self.assertNotEqual(0, proc.returncode)
        host = int(out.split()[0]) if out.split() and out.split()[0].isdigit() else None
        if host is not None:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                try:
                    os.kill(host, 0)
                except ProcessLookupError:
                    break
                time.sleep(0.1)
            else:
                self.fail(f"tool host {host} survived the interrupt")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
