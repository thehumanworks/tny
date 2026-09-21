#!/usr/bin/env python3
"""Run every SDK example end to end against the scripted offline provider.

The native runtime, tools, permissions and workflow scheduler are real; only
the model is scripted (offline_provider.py). `make test-sdk-examples` supplies
TNY_TEST_LIBRARY. The TypeScript half needs Node 24+ (it runs the .ts files
directly) and the SDK addon built by `npm --prefix sdk/typescript run build`.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
VERIFY = "python3 main.py Ada | grep -q 'Hello, Ada!'"
# (model, effort as sent on the chat wire) for the three tiers in models.json
LUNA = ("gpt-5.6-Luna", "low")
SOL = ("gpt-5.6-sol", "medium")
ASTRA = ("gpt-6-Astra", "high")


def node_major() -> int:
    node = shutil.which("node")
    if node is None:
        return 0
    version = subprocess.run(
        [node, "--version"], capture_output=True, text=True, check=True
    )
    return int(version.stdout.strip().lstrip("v").split(".")[0])


class ExamplesTest(unittest.TestCase):
    provider: subprocess.Popen[str]
    request_log: Path
    environment: dict[str, str]

    @classmethod
    def setUpClass(cls) -> None:
        log_directory = tempfile.TemporaryDirectory(prefix="tny-sdk-examples-log-")
        cls.addClassCleanup(log_directory.cleanup)
        cls.request_log = Path(log_directory.name) / "requests.jsonl"
        cls.provider = subprocess.Popen(
            [sys.executable, str(HERE / "offline_provider.py"), "0"],
            env={**os.environ, "OFFLINE_REQUEST_LOG": str(cls.request_log)},
            stdout=subprocess.PIPE,
            text=True,
        )
        assert cls.provider.stdout is not None
        base_url = cls.provider.stdout.readline().strip()
        library = os.environ.get("TNY_TEST_LIBRARY") or os.environ.get(
            "TNY_LIBRARY_PATH"
        )
        if not library:
            raise unittest.SkipTest("set TNY_TEST_LIBRARY to the built libtny")
        cls.environment = {
            **os.environ,
            "OPENAI_BASE_URL": base_url,
            "OPENAI_API_KEY": "offline",
            "OPENAI_WIRE_API": "chat",
            "OPENAI_MODEL": "",
            "TNY_LIBRARY_PATH": str(Path(library).resolve()),
            "PYTHONPATH": str(ROOT / "sdk" / "python" / "src"),
        }

    @classmethod
    def tearDownClass(cls) -> None:
        cls.provider.terminate()
        cls.provider.wait(timeout=10)
        if cls.provider.stdout is not None:
            cls.provider.stdout.close()

    def setUp(self) -> None:
        self.request_log.write_text("", encoding="utf-8")
        scratch = tempfile.TemporaryDirectory(prefix="tny-sdk-examples-")
        self.addCleanup(scratch.cleanup)
        self.scratch = Path(scratch.name)
        (self.scratch / "ws").mkdir()
        (self.scratch / "ws" / "README.md").write_text("# demo\n", encoding="utf-8")

    def launch(self, command: list[str]) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            command,
            cwd=self.scratch,
            env=self.environment,
            capture_output=True,
            text=True,
            timeout=300,
            check=False,
        )

    def wire(self) -> dict[str, set[tuple[str, str]]]:
        """What each role actually sent: {role: {(model, wire effort)}}."""
        seen: dict[str, set[tuple[str, str]]] = {}
        for line in self.request_log.read_text(encoding="utf-8").splitlines():
            entry = json.loads(line)
            seen.setdefault(entry["role"], set()).add((entry["model"], entry["effort"]))
        return seen

    def check_research(self, command: list[str]) -> None:
        first = self.launch([*command, "How is it organised?", "--workspace", "ws"])
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertIn("## Summary", first.stdout)
        # the low first score sends the critic's gap round again
        self.assertIn("scores by round: 0.55 -> 0.90", first.stderr)
        self.assertIn("learned 1 new lesson(s)", first.stderr)
        self.assertEqual(
            [path.name for path in (self.scratch / "ws").iterdir()], ["README.md"]
        )
        # models.json reached the wire: cheap breadth, the strongest model
        # only for the critic that steers the loop ("light" is sent as "low")
        self.assertEqual(
            self.wire(),
            {
                "research-planner": {SOL},
                "research-investigator": {LUNA},
                "research-synthesiser": {SOL},
                "research-critic": {ASTRA},
                "research-retro": {LUNA},
            },
        )

        second = self.launch([*command, "How is it organised?", "--workspace", "ws"])
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertIn("recalled 1 lesson(s)", second.stderr)
        self.assertIn("learned 0 new lesson(s)", second.stderr)  # deduplicated

    def check_codegen(self, command: list[str]) -> None:
        done = self.launch(
            [*command, "A greeting CLI", "--workspace", "out", "--verify", VERIFY]
            + ["--max-repairs", "1"]  # exactly the one repair the run needs
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("invalid reply", done.stderr)  # the JSON re-ask path ran
        self.assertIn("verify attempt 1: FAILED", done.stderr)
        self.assertIn("verify attempt 2: passed", done.stderr)  # the fixer repaired it
        self.assertIn("No blocking findings", done.stdout)
        written = sorted(
            path.name
            for path in (self.scratch / "out").iterdir()
            if path.name != "__pycache__"
        )
        self.assertEqual(written, ["greet.py", "main.py"])  # no runtime state leaked
        self.assertEqual(
            self.wire(),
            {
                "codegen-decomposer": {SOL},
                "codegen-architect": {ASTRA},
                "codegen-generator": {SOL},
                "review": {ASTRA},
                "codegen-fixer": {SOL},
                "codegen-retro": {LUNA},
            },
        )

        # A repair the usual tier cannot make escalates to the critical model.
        self.request_log.write_text("", encoding="utf-8")
        stubborn = self.launch(
            [*command, "A greeting CLI [stubborn]", "--workspace", "out3"]
            + ["--verify", VERIFY, "--max-repairs", "2"]
        )
        self.assertEqual(stubborn.returncode, 0, stubborn.stderr)
        self.assertIn("verify attempt 2: FAILED", stubborn.stderr)
        self.assertIn("verify attempt 3: passed", stubborn.stderr)
        self.assertEqual(self.wire()["codegen-fixer"], {SOL, ASTRA})

        # One flag pins every role to a single model and effort.
        self.request_log.write_text("", encoding="utf-8")
        pinned = self.launch(
            [*command, "A greeting CLI", "--workspace", "out4", "--verify", VERIFY]
            + ["--model", "one-model", "--effort", "default"]
        )
        self.assertEqual(pinned.returncode, 0, pinned.stderr)
        self.assertEqual(set().union(*self.wire().values()), {("one-model", None)})

        unrepaired = self.launch(
            [*command, "A greeting CLI", "--workspace", "out2", "--verify", VERIFY]
            + ["--max-repairs", "0"]
        )
        self.assertEqual(unrepaired.returncode, 1, unrepaired.stderr)
        self.assertIn("verification FAILED after 1 attempt(s)", unrepaired.stderr)

    def test_python_auto_research(self) -> None:
        self.check_research([sys.executable, str(HERE / "python" / "auto_research.py")])

    def test_python_codegen(self) -> None:
        self.check_codegen([sys.executable, str(HERE / "python" / "codegen.py")])

    def typescript(self, script: str) -> list[str]:
        if node_major() < 24:
            self.skipTest("the TypeScript examples need Node 24+")
        package = HERE / "typescript"
        if not (package / "node_modules" / "@thehumanworks" / "tny").exists():
            subprocess.run(
                ["npm", "install", "--omit=dev", "--no-audit", "--no-fund"],
                cwd=package,
                check=True,
                capture_output=True,
            )
        return ["node", str(package / script)]

    def test_typescript_auto_research(self) -> None:
        self.check_research(self.typescript("auto_research.ts"))

    def test_typescript_codegen(self) -> None:
        self.check_codegen(self.typescript("codegen.ts"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
