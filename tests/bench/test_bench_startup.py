#!/usr/bin/env python3
"""Deterministic policy checks and real child/PTY boundary checks."""

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "bench_startup", Path(__file__).with_name("bench_startup.py")
)
bench = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bench)


def metrics(value):
    return {
        mode: {
            "latency_ms": bench.summary([value] * 102),
            "rss_bytes": bench.summary([100] * 102),
        }
        for mode in bench.MODES
    }


class StartupTests(unittest.TestCase):
    def test_threshold_edges(self):
        self.assertTrue(
            all(g["pass"] for g in bench.gates(metrics(1), metrics(1.25)).values())
        )
        self.assertFalse(bench.gates(metrics(1), metrics(1.250001))["help"]["pass"])
        self.assertFalse(bench.gates(metrics(4.9), metrics(5))["help"]["pass"])
        self.assertTrue(bench.gates(metrics(6), metrics(6.6))["prompt"]["pass"])
        self.assertFalse(bench.gates(metrics(9.8), metrics(10))["prompt"]["pass"])
        self.assertFalse(bench.gates(metrics(1), metrics(1.500001))["prompt"]["pass"])

    def test_decimal_threshold_boundary(self):
        for mode in ("help", "version"):
            self.assertTrue(bench.gates(metrics(4), metrics(4.4))[mode]["pass"])
            self.assertFalse(bench.gates(metrics(4), metrics(4.400001))[mode]["pass"])

    def test_percentiles_and_raw_order(self):
        self.assertEqual(
            bench.summary([3, 1, 2]), {"median": 2, "p95": 3, "raw": [3, 1, 2]}
        )

    def test_real_launches_split_prompt_and_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            stub = Path(tmp) / "stub"
            stub.write_text(
                f"#!{sys.executable}\nimport os, sys, time\n"
                "assert 'OPENAI_API_KEY' not in os.environ\n"
                "assert not os.listdir(os.environ['HOME'])\n"
                "if '--provider' in sys.argv:\n"
                " assert os.isatty(0) and os.isatty(1)\n"
                f" for byte in {bench.PROMPT!r}:\n"
                "  os.write(1, bytes([byte]))\n"
                "  time.sleep(.001)\n"
                " time.sleep(10)\n"
            )
            stub.chmod(0o755)
            with patch.dict(bench.os.environ, {"OPENAI_API_KEY": "must-not-inherit"}):
                for mode in bench.MODES:
                    sample = bench.measure(stub, mode)
                    self.assertGreater(sample["ms"], 0)
                    self.assertGreater(sample["rss_bytes"], 0)
            stub.write_text(f"#!{sys.executable}\nraise SystemExit(7)\n")
            with self.assertRaisesRegex(RuntimeError, "exited 7"):
                bench.measure(stub, "help")
            stub.write_text(f"#!{sys.executable}\nimport time\ntime.sleep(10)\n")
            with self.assertRaisesRegex(RuntimeError, "timed out"):
                bench.measure(stub, "prompt", timeout=0.05)

    def test_order_counts(self):
        args = bench.argparse.Namespace(
            label="test",
            samples=100,
            prompt_samples=20,
            baseline=sys.executable,
            candidate=sys.executable,
            wasm_dir=None,
            build_metadata="fixture",
        )
        with (
            patch.object(bench, "size_report", return_value={}),
            patch.object(bench, "measure", return_value={"ms": 1, "rss_bytes": 20}),
        ):
            report = bench.benchmark(args)
        self.assertEqual(len(report["order"]), 440)
        self.assertEqual(
            report["order"][:2],
            [["help", 1, "baseline", 1], ["help", 1, "candidate", 1]],
        )
        self.assertEqual(report["order"][68][:3], ["help", 2, "candidate"])
        for role in ("baseline", "candidate"):
            self.assertEqual(len(report[role]["prompt"]["latency_ms"]["raw"]), 20)

    def test_size_report_preserves_input_and_flags_runtime(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "tny"
            binary.write_bytes(b"original")
            wasm = Path(tmp) / "wasm"
            wasm.mkdir()
            (wasm / "tny.wasm").write_bytes(b"wasm")
            (wasm / "tny.js").write_bytes(b"glue")

            def command(args, **kwargs):
                if args[0] == "strip":
                    Path(args[1]).write_bytes(b"small")
                return subprocess.CompletedProcess(
                    args, 0, "libc++.1.dylib\nlibstdc++.so.6", ""
                )

            with patch.object(bench.subprocess, "run", side_effect=command):
                report = bench.size_report(binary)
            self.assertEqual(binary.read_bytes(), b"original")
            self.assertEqual(report["stripped_bytes"], 5)
            self.assertEqual(len(report["cpp_runtime_dependencies"]), 2)
            self.assertEqual(report["wasm"]["total_bytes"], 8)

    def test_comparison_rejects_forged_statistics(self):
        report = {"schema": 1, "policy": bench.POLICY, "candidate": metrics(1)}
        report["candidate"]["help"]["latency_ms"]["median"] = 0
        with self.assertRaisesRegex(ValueError, "inconsistent"):
            bench.validate_report(report)

    def test_cli_rejects_small_sample(self):
        script = str(Path(bench.__file__))
        result = subprocess.run(
            [
                sys.executable,
                script,
                "--baseline",
                sys.executable,
                "--candidate",
                sys.executable,
                "--json",
                "unused.json",
                "--samples",
                "99",
            ],
            capture_output=True,
        )
        self.assertEqual(result.returncode, 2)

    def test_comparison_is_informational(self):
        for before, after in ((1, 8), (4.5, 3.5)):
            with self.subTest(before=before, after=after):
                self.check_comparison(before, after)

    def check_comparison(self, before, after):
        script = str(Path(bench.__file__))
        with tempfile.TemporaryDirectory() as tmp:
            files = []
            for index, value in enumerate((before, after)):
                item = metrics(value)
                item["size"] = {
                    "stripped_bytes": 1,
                    "dependencies": "none",
                    "cpp_runtime_dependencies": [],
                    "wasm": {},
                }
                report = {
                    "policy": bench.POLICY,
                    "schema": 1,
                    "candidate": item,
                    "baseline": metrics(3),
                    "host": "test",
                }
                path = Path(tmp) / f"{index}.json"
                path.write_text(json.dumps(report))
                files.append(str(path))
            result = subprocess.run(
                [sys.executable, script, "--compare", *files], capture_output=True
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(b"informational, not a contract verdict", result.stdout)
            self.assertIn(f"{after - before:+.4f}".encode(), result.stdout)
            self.assertNotIn(b"PASS", result.stdout)
            self.assertNotIn(b"FAIL", result.stdout)


if __name__ == "__main__":
    unittest.main()
