"""Offline checks for sequential prompts and per-turn accounting."""

import gzip
import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from adapters import Invocation, resume_id, session_invocation
from run import main, run_one, task_prompts, verify_task


class SessionTest(unittest.TestCase):
    def test_resume_commands_and_ids(self):
        call = Invocation(["agent", "ask", "--json", "next"], {})
        with patch("adapters.invocation", return_value=call):
            resumed = session_invocation(
                "tny", Path("run"), "http://proxy", "next", "model", "low", "tny", "sid"
            )
        self.assertEqual(resumed.command[-3:], ["--resume", "sid", "next"])
        call = Invocation(["codex", "exec", "--json", "next"], {})
        with patch("adapters.invocation", return_value=call):
            resumed = session_invocation(
                "codex",
                Path("run"),
                "http://proxy",
                "next",
                "model",
                "low",
                "tny",
                "tid",
            )
        self.assertEqual(resumed.command[-3:], ["resume", "tid", "next"])
        self.assertEqual(resume_id("tny", '{"session_id":"sid"}'), "sid")
        self.assertEqual(
            resume_id("codex", '{"type":"thread.started","thread_id":"tid"}\n'),
            "tid",
        )
        with self.assertRaises(ValueError):
            session_invocation(
                "pi", Path("run"), "http://proxy", "next", "model", "low", "tny", "sid"
            )
        with self.assertRaises(ValueError):
            task_prompts({"prompts": []})

    def test_two_turns_share_workspace_and_verify_once(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            task = root / "fixture"
            (task / "repo").mkdir(parents=True)
            (task / "task.json").write_text(
                json.dumps(
                    {
                        "id": "fixture",
                        "category": "test",
                        "prompts": ["first", "second"],
                        "timeout_s": 5,
                    }
                )
            )
            (task / "verify.sh").write_text(
                '#!/bin/sh\nset -eu\ntest "$(cat "$1/output.txt")" = second\necho "pass: done"\n'
            )
            args = SimpleNamespace(
                out=root / "runs",
                label="offline",
                auth_file=root / "auth.json",
                model="gpt-5.6-luna",
                effort="low",
                tny_bin="unused",
            )
            calls = []

            class FakeProxy:
                def __init__(self, path, auth_file):
                    self.path = path
                    self.path.mkdir(parents=True)
                    self.base_url = "http://127.0.0.1:1"
                    self.rows = []

                def __enter__(self):
                    return self

                def __exit__(self, *args):
                    return False

                def begin_turn(self, turn):
                    body_file = f"request-{turn}.json.gz"
                    body = {"model": args.model, "reasoning": {"effort": args.effort}}
                    (self.path / body_file).write_bytes(
                        gzip.compress(json.dumps(body).encode())
                    )
                    self.rows.append(
                        {
                            "turn": turn,
                            "body_file": body_file,
                            "sections": {
                                "instructions_chars": 0,
                                "tools_chars": 0,
                                "tool_output_chars": [],
                            },
                            "input_tokens": 20 * turn,
                            "cached_input_tokens": 0,
                            "cache_write_tokens": 0,
                            "output_tokens": 5,
                            "reasoning_tokens": 1,
                            "http_status": 200,
                            "output_items": [],
                        }
                    )

            def invoke(
                name, run_dir, proxy_url, prompt, model, effort, binary, session
            ):
                calls.append((prompt, session))
                code = (
                    "from pathlib import Path; import json; "
                    "Path('output.txt').write_text(" + repr(prompt) + "); "
                    "print(json.dumps({'session_id':'sid','output':'done'}))"
                )
                return Invocation([sys.executable, "-c", code], os.environ.copy())

            with (
                patch("run.RecordingProxy", FakeProxy),
                patch("run.session_invocation", side_effect=invoke),
                patch("run.verify_task", wraps=verify_task) as verifier,
            ):
                result = run_one(args, task, "tny", 1)
            self.assertEqual(result["status"], "pass")
            self.assertEqual(
                (result["turns_requested"], result["turns_completed"]), (2, 2)
            )
            self.assertEqual([row["turn"] for row in result["request_rows"]], [1, 2])
            self.assertEqual(
                [turn["input_tokens"] for turn in result["turns"]], [20, 40]
            )
            self.assertEqual(calls, [("first", None), ("second", "sid")])
            self.assertEqual(verifier.call_count, 1)

    def test_unverified_adapter_is_skipped_without_starting_a_run(self):
        with tempfile.TemporaryDirectory() as temp:
            tasks = Path(temp) / "tasks"
            task = tasks / "session"
            task.mkdir(parents=True)
            (task / "task.json").write_text(
                json.dumps({"id": "session", "prompts": ["one", "two"]})
            )
            output = io.StringIO()
            argv = [
                "run.py",
                "--tasks-dir",
                str(tasks),
                "--task",
                "session",
                "--harness",
                "pi",
                "--reps",
                "1",
            ]
            with patch.object(sys, "argv", argv), redirect_stdout(output):
                self.assertEqual(main(), 0)
            status = json.loads(output.getvalue())
            self.assertEqual(status["status"], "skipped")


if __name__ == "__main__":
    unittest.main()
