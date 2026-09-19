#!/usr/bin/env python3
"""Offline integration checks for the optional bounded improvement controller."""

from __future__ import annotations

import hashlib
import json
import os
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))
import tny_improve as improve

# One independent evaluator, with synthetic per-case outcomes. No provider I/O.
FAKE = r"""
import json, os, sys
from pathlib import Path
assert float(os.environ["TNY_IMPROVE_TIMEOUT_S"]) > 0
request = json.load(sys.stdin)
config = json.loads(Path("config.json").read_text())
with Path("calls.jsonl").open("a") as log:
    log.write(json.dumps(request) + "\n")
if sys.argv[1] == "proposer":
    body = config["proposals"][request["round"] - 1]
    result = {"instructions": body, "rationale": "synthetic proposal"}
else:
    case = request["case"]
    body = request["instructions"]
    outcomes = config.get("outcomes", {}).get(body, {})
    passed, cost = outcomes.get(case["id"], [body != "base", 10])
    result = {"passed": passed, "cost": cost, "feedback": case["input"]}
print(json.dumps(result))
print("synthetic stderr", file=sys.stderr)
"""


class ImprovementTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.baseline = self.root / "task.md"
        self.baseline.write_text("base", encoding="utf-8")
        (self.root / "fake.py").write_text(FAKE, encoding="utf-8")
        self.spec_path = self.root / "spec.json"
        self.out = self.root / "run"
        self.digest = hashlib.sha256(b"base").hexdigest()
        self.spec = {
            "version": 1,
            "baseline": "task.md",
            "rounds": 1,
            "timeout_s": 2,
            "cost_unit": "synthetic tokens",
            "proposer": [sys.executable, "fake.py", "proposer"],
            "evaluator": [sys.executable, "fake.py", "evaluator"],
            "cases": {
                split: [{"id": split, "input": split + "-secret"}]
                for split in ("train", "validation", "test")
            },
        }
        self.config = {"proposals": ["better"]}

    def save(self):
        self.spec_path.write_text(json.dumps(self.spec), encoding="utf-8")
        (self.root / "config.json").write_text(
            json.dumps(self.config), encoding="utf-8"
        )

    def run_search(self):
        self.save()
        return improve.run(self.spec_path, self.out)

    def failed(self):
        with self.assertRaises(improve.ImprovementError):
            self.run_search()
        report = json.loads((self.out / "report.json").read_text())
        self.assertEqual(report["status"], "failed")
        self.assertFalse(report["eligible"])
        with self.assertRaises(improve.ImprovementError):
            improve.promote(self.out, self.baseline, self.digest)
        self.assertEqual(self.baseline.read_text(), "base")

    def test_multiround_parent_reuse_and_holdout_isolation(self):
        self.spec["rounds"] = 4
        self.config = {
            "proposals": ["first", "regression", "cheaper", "tie"],
            "outcomes": {
                "regression": {"validation": [False, 10]},
                "cheaper": {"train": [True, 9]},
                "tie": {"train": [True, 9]},
            },
        }
        report = self.run_search()
        self.assertEqual(
            [row["accepted"] for row in report["rounds"]], [True, False, True, False]
        )
        calls = [
            json.loads(line)
            for line in (self.root / "calls.jsonl").read_text().splitlines()
        ]
        proposals = [call for call in calls if "round" in call]
        self.assertEqual(
            [call["parent_instructions"] for call in proposals],
            ["base", "first", "first", "cheaper"],
        )
        self.assertEqual(
            [call["training_feedback"][0]["cost"] for call in proposals],
            [10, 10, 10, 9],
        )
        for call in proposals:
            self.assertEqual(
                set(call), {"parent_instructions", "round", "training_feedback"}
            )
            self.assertNotIn("validation", json.dumps(call))
            self.assertNotIn("test-secret", json.dumps(call))
        holdout = [call for call in calls if call.get("case", {}).get("id") == "test"]
        self.assertEqual(
            [call["instructions"] for call in holdout], ["base", "cheaper"]
        )
        self.assertEqual(calls[-2:], holdout)
        self.assertFalse(report["holdout_selection_evidence"])
        self.assertEqual((self.out / "final.md").read_text(), "cheaper")
        self.assertEqual(self.baseline.read_text(), "base")
        self.assertEqual(stat.S_IMODE(self.out.stat().st_mode), 0o700)
        for index in range(1, 5):
            label = f"round-{index:02d}"
            self.assertTrue((self.out / (label + ".decision.json")).is_file())
            self.assertEqual(
                (self.out / (label + ".proposal.stderr")).read_text(),
                "synthetic stderr\n",
            )
        self.assertEqual(
            report["rounds"][2]["parent_sha256"], hashlib.sha256(b"first").hexdigest()
        )

    def test_pareto_gate_regressions_inflation_ties(self):
        def result(passed, cost):
            return {"passed": passed, "cost": cost, "feedback": ""}

        parent = {
            "train": [result(True, 5), result(False, 5)],
            "validation": [result(True, 10)],
        }
        trials = [
            (
                {
                    "train": [result(False, 1), result(True, 1)],
                    "validation": [result(True, 1)],
                },
                False,
            ),
            (
                {
                    "train": [result(True, 5), result(True, 6)],
                    "validation": [result(True, 10)],
                },
                False,
            ),
            (
                {
                    "train": [result(True, 5), result(True, 5)],
                    "validation": [result(True, 11)],
                },
                False,
            ),
            (
                {
                    "train": [result(True, 5), result(True, 5)],
                    "validation": [result(False, 1)],
                },
                False,
            ),
            (parent, False),
            (
                {
                    "train": [result(True, 5), result(True, 5)],
                    "validation": [result(True, 10)],
                },
                True,
            ),
            (
                {
                    "train": [result(True, 4), result(False, 5)],
                    "validation": [result(True, 10)],
                },
                True,
            ),
        ]
        for candidate, expected in trials:
            with self.subTest(candidate=candidate):
                self.assertEqual(improve.gate(parent, candidate)[0], expected)

    def test_holdout_quality_is_not_selection_evidence(self):
        self.config["outcomes"] = {
            "base": {"test": [True, 1]},
            "better": {"test": [False, 100]},
        }
        report = self.run_search()
        self.assertTrue(report["eligible"])
        self.assertFalse(report["holdout"]["final"][0]["passed"])
        improve.promote(self.out, self.baseline, self.digest)

    def test_promotion_success_mode_rollback_no_command_reexecution(self):
        self.baseline.chmod(0o640)
        report = self.run_search()
        (self.root / "fake.py").unlink()
        self.spec_path.unlink()
        with mock.patch.object(improve.os, "fsync", wraps=os.fsync) as sync:
            promoted = improve.promote(self.out, self.baseline, self.digest)
        self.assertGreaterEqual(sync.call_count, 2)
        self.assertEqual(promoted["sha256"], report["final_sha256"])
        self.assertEqual(self.baseline.read_text(), "better")
        self.assertEqual(stat.S_IMODE(self.baseline.stat().st_mode), 0o640)
        self.assertEqual((self.out / "baseline.md").read_text(), "base")
        self.assertFalse(list(self.root.glob(".tny-improve-*")))
        with self.assertRaises(improve.ImprovementError):
            improve.promote(self.out, self.baseline, self.digest)

    def test_stale_wrong_missing_and_symlink_targets(self):
        self.run_search()
        self.baseline.write_text("changed")
        with self.assertRaises(improve.ImprovementError):
            improve.promote(self.out, self.baseline, self.digest)
        self.assertEqual(self.baseline.read_text(), "changed")
        self.baseline.write_text("base")
        with self.assertRaises(improve.ImprovementError):
            improve.promote(self.out, self.baseline, "0" * 64)
        with self.assertRaises(improve.ImprovementError):
            improve.promote(self.out, self.root / "absent.md", self.digest)
        link = self.root / "link.md"
        link.symlink_to(self.baseline)
        with self.assertRaises(improve.ImprovementError):
            improve.promote(self.out, link, self.digest)
        parent = self.root / "parent-link"
        parent.symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(improve.ImprovementError):
            improve.promote(self.out, parent / "task.md", self.digest)
        archive_link = self.root / "archive-link"
        archive_link.symlink_to(self.out, target_is_directory=True)
        with self.assertRaises(improve.ImprovementError):
            improve.promote(archive_link, self.baseline, self.digest)

    def test_dotdot_alias_cannot_overwrite_rollback_archive(self):
        self.run_search()
        (self.root / "alias").mkdir()
        before = {p.name: p.read_bytes() for p in self.out.iterdir()}
        for archive, target in (
            (self.root / "alias/../run", self.out / "baseline.md"),
            (self.out, self.root / "alias/../run/baseline.md"),
        ):
            with self.subTest(archive=archive, target=target):
                with self.assertRaises(improve.ImprovementError):
                    improve.promote(archive, target, self.digest)
                self.assertEqual(
                    {p.name: p.read_bytes() for p in self.out.iterdir()}, before
                )

    def test_archive_tampering_and_missing_evidence(self):
        self.run_search()
        for name in (
            "baseline.md",
            "spec.json",
            "inputs.json",
            "final.md",
            "report.json",
            "round-01.decision.json",
            "round-01.train.0000.stdout",
            "round-01.proposal.input.json",
            "baseline.train.0000.stderr",
            "baseline.train.0000.command.json",
        ):
            with self.subTest(name=name):
                path = self.out / name
                data = path.read_bytes()
                path.write_bytes(data + b" ")
                with self.assertRaises(improve.ImprovementError):
                    improve.promote(self.out, self.baseline, self.digest)
                path.unlink()
                with self.assertRaises(improve.ImprovementError):
                    improve.promote(self.out, self.baseline, self.digest)
                path.symlink_to(self.baseline)
                with self.assertRaises(improve.ImprovementError):
                    improve.promote(self.out, self.baseline, self.digest)
                path.unlink()
                path.write_bytes(data)
        self.assertEqual(self.baseline.read_text(), "base")

    def test_rehashed_inconsistent_decision_still_refused(self):
        self.run_search()
        path = self.out / "round-01.decision.json"
        decision = json.loads(path.read_text())
        decision["parent_sha256"] = "0" * 64
        path.write_text(json.dumps(decision))
        manifest_path = self.out / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["sha256"][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaises(improve.ImprovementError):
            improve.promote(self.out, self.baseline, self.digest)

    def test_rejected_only_and_identical_body_not_promotable(self):
        self.config["proposals"] = ["base"]
        report = self.run_search()
        self.assertFalse(report["eligible"])
        self.assertIn("identical", report["rounds"][0]["reason"])
        with self.assertRaises(improve.ImprovementError):
            improve.promote(self.out, self.baseline, self.digest)

    def test_duplicate_ids_across_splits(self):
        self.spec["cases"]["test"][0]["id"] = "train"
        self.failed()
        self.assertFalse((self.root / "calls.jsonl").exists())

    def test_duplicate_inputs_with_different_ids_across_splits(self):
        self.spec["cases"]["test"][0]["input"] = "train-secret"
        self.failed()

    def test_exact_cost_gate_does_not_hide_small_increase(self):
        parent = {
            "train": [{"passed": False, "cost": 10**20}],
            "validation": [
                {"passed": True, "cost": 10**20},
                {"passed": True, "cost": 1},
            ],
        }
        candidate = {
            "train": [{"passed": True, "cost": 10**20}],
            "validation": [
                {"passed": True, "cost": 10**20},
                {"passed": True, "cost": 2},
            ],
        }
        self.assertFalse(improve.gate(parent, candidate)[0])
        candidate = {
            "train": [{"passed": False, "cost": 10**20 - 1}],
            "validation": parent["validation"],
        }
        self.assertTrue(improve.gate(parent, candidate)[0])

    def test_invalid_spec_bounds(self):
        for key, value in [
            ("version", True),
            ("rounds", 0),
            ("rounds", 21),
            ("rounds", True),
            ("timeout_s", 0),
            ("timeout_s", 3601),
            ("timeout_s", True),
            ("cost_unit", " "),
            ("proposer", "echo nope"),
            ("evaluator", []),
        ]:
            with self.subTest(key=key, value=value):
                spec = dict(self.spec, **{key: value})
                with self.assertRaises(improve.ImprovementError):
                    improve._spec(spec)

    def test_invalid_evaluator_responses(self):
        responses = [
            "{",
            "{}",
            '{"passed":true,"cost":NaN,"feedback":""}',
            '{"passed":true,"cost":Infinity,"feedback":""}',
            '{"passed":true,"cost":1e999,"feedback":""}',
            '{"passed":true,"cost":true,"feedback":""}',
            '{"passed":true,"cost":-1,"feedback":""}',
            '{"passed":1,"cost":0,"feedback":""}',
            '{"passed":true,"cost":0,"feedback":null}',
            '{"passed":true,"cost":0,"cost":1,"feedback":""}',
            '{"passed":true,"cost":0,"feedback":""} {}',
        ]
        for index, response in enumerate(responses):
            with self.subTest(response=response):
                self.out = self.root / f"run-{index}"
                self.spec["evaluator"] = [
                    sys.executable,
                    "-c",
                    "print(" + repr(response) + ")",
                ]
                self.failed()
                self.assertEqual(
                    (self.out / "baseline.train.0000.stdout").read_text().strip(),
                    response,
                )

    def test_invalid_proposal_bodies_and_fields(self):
        for index, value in enumerate(
            [
                "",
                " ",
                "a\0b",
                "\ufeff",
                "\ufeff \n",
                "---\ntitle: bad\n---\nbody",
                "+++\na=1",
                "x" * 65537,
                "\ud800",
            ]
        ):
            with self.subTest(index=index):
                self.out = self.root / f"run-{index}"
                self.config["proposals"] = [value]
                self.failed()
                self.assertTrue((self.out / "round-01.proposal.stdout").exists())
        for value in ({"instructions": "ok"}, {"instructions": "ok", "rationale": 1}):
            with self.assertRaises(improve.ImprovementError):
                improve._proposal(value)

    def test_timeouts_nonzero_and_bounded_stdout_stderr(self):
        scripts = [
            "import time; print('partial', flush=True); time.sleep(10)",
            "import sys; print('partial'); sys.exit(7)",
            "import sys; sys.stdout.write('x' * (1024 * 1024 + 1))",
            "import sys; sys.stderr.write('x' * (1024 * 1024 + 1))",
            "import os, time; os.close(1); os.close(2); time.sleep(10)",
        ]
        for index, script in enumerate(scripts):
            with self.subTest(index=index):
                self.out = self.root / f"run-{index}"
                self.spec["evaluator"] = [sys.executable, "-c", script]
                self.spec["timeout_s"] = 0.2
                started = time.monotonic()
                self.failed()
                self.assertLess(time.monotonic() - started, 3)
                for suffix in (".stdout", ".stderr"):
                    self.assertLessEqual(
                        (self.out / ("baseline.train.0000" + suffix)).stat().st_size,
                        improve.MAX_JSON,
                    )

    def test_holdout_error_fails_previously_accepted_run(self):
        self.spec["evaluator"] = [
            sys.executable,
            "-c",
            "import json,sys; x=json.load(sys.stdin); "
            "sys.exit(5) if x['case']['id']=='test' else "
            "print(json.dumps(dict(passed=x['instructions']!='base',cost=1,feedback='')))",
        ]
        self.failed()
        self.assertTrue(
            json.loads((self.out / "round-01.decision.json").read_text())["accepted"]
        )

    def test_launch_failure_and_invalid_utf8_are_archived(self):
        commands = [
            [str(self.root / "missing-command")],
            [sys.executable, "-c", "import os; os.write(1, b'\\xff')"],
        ]
        for index, command in enumerate(commands):
            with self.subTest(command=command):
                self.out = self.root / f"run-{index}"
                self.spec["evaluator"] = command
                self.failed()
                self.assertTrue(
                    (self.out / "baseline.train.0000.command.json").is_file()
                )

    @unittest.skipUnless(os.name == "posix", "POSIX process group deadline")
    def test_descendant_holding_pipes_cannot_escape_deadline(self):
        self.spec["timeout_s"] = 0.2
        self.spec["evaluator"] = [
            sys.executable,
            "-c",
            "import os,time; child=os.fork(); "
            "time.sleep(10) if child==0 else os._exit(0)",
        ]
        started = time.monotonic()
        self.failed()
        self.assertLess(time.monotonic() - started, 3)

    def cleanup_program(self):
        path = self.root / "cleanup.py"
        path.write_text(
            "import signal,time,sys\nfrom pathlib import Path\n"
            "def stop(*args):\n"
            " Path('cleanup-started').touch()\n time.sleep(0.4)\n"
            " Path('cleanup-done').touch()\n sys.exit(0)\n"
            "signal.signal(signal.SIGTERM,stop)\nprint('ready',flush=True)\ntime.sleep(10)\n"
        )
        return path

    def test_wrapper_exit_does_not_skip_descendant_cleanup_grace(self):
        helper = self.cleanup_program()
        self.spec["timeout_s"] = 0.2
        self.spec["evaluator"] = [
            sys.executable,
            "-c",
            "import subprocess,sys,time;subprocess.Popen([sys.executable,sys.argv[1]]);time.sleep(10)",
            str(helper),
        ]
        self.failed()
        self.assertTrue((self.root / "cleanup-done").exists())
        self.assertTrue((self.out / "baseline.train.0000.command.json").is_file())

    def test_first_interrupt_during_timeout_cleanup_preserves_evidence(self):
        for requested_signal in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=requested_signal):
                self.out = self.root / f"run-signal-{requested_signal}"
                for name in ("cleanup-started", "cleanup-done"):
                    (self.root / name).unlink(missing_ok=True)
                helper = self.cleanup_program()
                self.spec["timeout_s"] = 0.2
                self.spec["evaluator"] = [sys.executable, str(helper)]
                self.save()
                child = subprocess.Popen(
                    [
                        sys.executable,
                        str(ROOT / "python/tny_improve.py"),
                        "run",
                        "--spec",
                        str(self.spec_path),
                        "--out",
                        str(self.out),
                    ],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    start_new_session=True,
                )
                try:
                    deadline = time.monotonic() + 5
                    while (
                        not (self.root / "cleanup-started").exists()
                        and time.monotonic() < deadline
                    ):
                        time.sleep(0.01)
                    self.assertTrue((self.root / "cleanup-started").exists())
                    child.send_signal(requested_signal)
                    output, errors = child.communicate(timeout=5)
                    self.assertNotEqual(child.returncode, 0, (output, errors))
                    self.assertTrue((self.root / "cleanup-done").exists())
                    self.assertTrue(
                        (self.out / "baseline.train.0000.command.json").is_file()
                    )
                finally:
                    if child.returncode is None:
                        child.send_signal(signal.SIGINT)
                        child.communicate(timeout=25)
                    child.stdout.close()
                    child.stderr.close()

    def test_atomic_replace_failure_preserves_target_and_cleans_temporary(self):
        self.run_search()
        with mock.patch.object(
            improve.os, "replace", side_effect=OSError("injected replace failure")
        ):
            with self.assertRaises(improve.ImprovementError):
                improve.promote(self.out, self.baseline, self.digest)
        self.assertEqual(self.baseline.read_text(), "base")
        self.assertFalse(list(self.root.glob(".tny-improve-*")))

    def test_target_change_while_staging_is_not_overwritten(self):
        self.run_search()
        original = os.fchmod

        def change_target(fd, mode):
            original(fd, mode)
            self.baseline.write_text("concurrent change")

        with mock.patch.object(improve.os, "fchmod", side_effect=change_target):
            with self.assertRaises(improve.ImprovementError):
                improve.promote(self.out, self.baseline, self.digest)
        self.assertEqual(self.baseline.read_text(), "concurrent change")
        self.assertFalse(list(self.root.glob(".tny-improve-*")))

    def test_existing_directory_and_symlink_refused_without_changes(self):
        self.save()
        self.out.mkdir()
        marker = self.out / "marker"
        marker.write_text("keep")
        with self.assertRaises(improve.ImprovementError):
            improve.run(self.spec_path, self.out)
        self.assertEqual(list(self.out.iterdir()), [marker])
        link = self.root / "link"
        link.symlink_to(self.out, target_is_directory=True)
        with self.assertRaises(improve.ImprovementError):
            improve.run(self.spec_path, link)
        self.assertEqual(marker.read_text(), "keep")

    def test_cli_run_promote_and_error_exit(self):
        self.save()
        command = [sys.executable, str(ROOT / "python" / "tny_improve.py")]
        result = subprocess.run(
            command + ["run", "--spec", str(self.spec_path), "--out", str(self.out)],
            capture_output=True,
            timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)["eligible"])
        promote = command + [
            "promote",
            "--run",
            str(self.out),
            "--target",
            str(self.baseline),
            "--expected-sha256",
            self.digest,
        ]
        result = subprocess.run(promote, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout)
        result = subprocess.run(promote, capture_output=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)["status"], "failed")
        result = subprocess.run(
            command + ["run", "--spec", str(self.spec_path), "--out", str(self.out)],
            capture_output=True,
            timeout=15,
        )
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
