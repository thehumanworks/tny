from __future__ import annotations

import copy
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODES = ((), ("-O",), ("-OO",))


class ContractTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="tny-conformance-contract-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for name in ("sdk/conformance/check.py", "sdk/schema/events.json"):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        self.contract = json.loads((ROOT / "sdk/conformance/v1.json").read_text())

    def run_checker(self, contract: dict, flags: tuple[str, ...]):
        (self.root / "sdk/conformance/v1.json").write_text(json.dumps(contract))
        environment = dict(os.environ)
        environment.pop("PYTHONOPTIMIZE", None)
        environment["PYTHONDONTWRITEBYTECODE"] = "1"
        return subprocess.run(
            [sys.executable, *flags, str(self.root / "sdk/conformance/check.py")],
            env=environment,
            capture_output=True,
            text=True,
            timeout=20,
        )

    def reject_variants(self, path: tuple, variants: list) -> None:
        for field, value, diagnostic in variants:
            contract = copy.deepcopy(self.contract)
            target = contract
            for key in path:
                target = target[key]
            target[field] = value
            for flags in MODES:
                with self.subTest(path=path, field=field, value=value, flags=flags):
                    run = self.run_checker(contract, flags)
                    self.assertEqual(run.returncode, 1, run.stderr)
                    self.assertEqual(run.stdout, "")
                    self.assertIn(diagnostic, run.stderr)

    def scenario_path(self, name: str) -> tuple[str, int]:
        return "scenarios", next(
            index
            for index, scenario in enumerate(self.contract["scenarios"])
            if scenario["id"] == name
        )

    def test_valid_contract_preserves_output_in_all_modes(self) -> None:
        for flags in MODES:
            with self.subTest(flags=flags):
                run = self.run_checker(self.contract, flags)
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertEqual(run.stderr, "")
                self.assertEqual(
                    run.stdout, "conformance v1: 10 executable scenarios, schema 1\n"
                )

    def test_versions_are_checked_in_all_modes(self) -> None:
        self.reject_variants(
            (),
            [
                ("conformance_version", 2, "unsupported conformance_version"),
                ("adapter_protocol_version", 2, "unsupported adapter_protocol_version"),
                ("event_schema_version", 2, "event_schema_version mismatch"),
                ("minimum_abi", "0.0", "unsupported minimum_abi"),
            ],
        )

    def test_scenario_ids_are_checked_in_all_modes(self) -> None:
        scenarios = self.contract["scenarios"]
        unexpected = copy.deepcopy(scenarios)
        unexpected[0]["id"] = "unexpected_scenario"
        self.reject_variants(
            (),
            [
                ("scenarios", scenarios + [scenarios[0]], "duplicate scenario id"),
                (
                    "scenarios",
                    scenarios[:-1],
                    "missing or unexpected conformance scenario",
                ),
                ("scenarios", unexpected, "missing or unexpected conformance scenario"),
            ],
        )

    def test_scenario_evidence_is_checked_in_all_modes(self) -> None:
        self.reject_variants(
            self.scenario_path("success_two_turns"),
            [
                ("release_required", False, "release_required must be true"),
                ("release_required", 1, "release_required must be true"),
                ("events", ["not_an_event"], "unknown event"),
                ("requires", ["not_a_capability"], "unknown capability"),
                ("assertions", [], "assertions must not be empty"),
                ("assertions", ["same", "same"], "duplicate assertion"),
                ("terminal_count", 0, "terminal_count must be positive"),
                ("terminal_count", -1, "terminal_count must be positive"),
                ("terminal_reasons", ["done"], "terminal_reasons excludes"),
                ("allow_reopen_clock_reset", True, "clock reset requires multiple"),
            ],
        )

    def test_reopen_and_steer_evidence_is_checked_in_all_modes(self) -> None:
        self.reject_variants(
            self.scenario_path("resume_and_steer_rejection"),
            [
                ("terminal_reasons", [], "terminal_reasons must not be empty"),
                ("terminal_count", 1, "terminal_reasons excludes"),
                ("terminal_reasons", ["done"], "clock reset requires multiple"),
                ("rejected_text", "", "rejected_text requires text"),
                ("events", ["turn_end"], "rejected_text requires text"),
            ],
        )

    def test_report_fields_are_checked_in_all_modes(self) -> None:
        required = self.contract["report_required"]
        self.reject_variants(
            (),
            [
                ("report_required", [], "missing required report field"),
                ("report_required", required + ["api_key"], "forbidden report field"),
            ],
        )


if __name__ == "__main__":
    unittest.main()
