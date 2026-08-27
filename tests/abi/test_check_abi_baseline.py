#!/usr/bin/env python3
"""Focused compatibility-policy checks for the proposed ABI 1 baseline."""
from __future__ import annotations

import copy
import importlib.util
import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "scripts" / "check_abi_baseline.py"
BASELINE = ROOT / "abi" / "baseline-v1.json"
INCOMPATIBLE = ROOT / "tests" / "abi" / "fixtures" / "incompatible-constant.json"

spec = importlib.util.spec_from_file_location("check_abi_baseline", CHECKER)
assert spec is not None and spec.loader is not None
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class AbiBaselineTests(unittest.TestCase):
    def run_checker(self, candidate: Path | None = None) -> subprocess.CompletedProcess[str]:
        command = [sys.executable, str(CHECKER), "--baseline", str(BASELINE)]
        if candidate is not None:
            command.extend(["--candidate", str(candidate)])
        return subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False)

    def test_baseline_schema_and_self_comparison_pass(self) -> None:
        schema = self.run_checker()
        self.assertEqual(schema.returncode, 0, schema.stderr)
        self.assertEqual(schema.stdout, "libtny ABI 1 baseline schema: ok\n")
        same = self.run_checker(BASELINE)
        self.assertEqual(same.returncode, 0, same.stderr)
        self.assertEqual(same.stdout, "libtny ABI 1 baseline comparison: ok\n")

    def test_deliberate_incompatible_constant_drift_fails(self) -> None:
        first = self.run_checker(INCOMPATIBLE)
        second = self.run_checker(INCOMPATIBLE)
        self.assertNotEqual(first.returncode, 0)
        self.assertEqual(first.stderr, second.stderr)
        self.assertIn("constant TNY_STATUS_OK: expected 0, got 99", first.stderr)

    def test_later_minor_can_append_a_tail_and_versioned_symbol(self) -> None:
        candidate = copy.deepcopy(json.loads(BASELINE.read_text(encoding="utf-8")))
        candidate["abi"] = {
            "major": 1,
            "minor": 1,
            "encoded": 65537,
            "elf_version_node": "LIBTNY_1.1",
        }
        candidate["artifacts"]["linux-glibc"]["elf_version_nodes"].append("LIBTNY_1.1")
        candidate["constants"]["TNY_EVENT_FUTURE"] = 14
        candidate["exports"]["tny_future_query"] = "LIBTNY_1.1"
        view = candidate["structs"]["tny_event_view_v0"]
        view["size"] = 336
        view["append_from"] = 336
        view["fields"].append({"name": "future_value", "offset": 328, "size": 8})
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "candidate.json"
            path.write_text(json.dumps(candidate, sort_keys=True), encoding="utf-8")
            result = self.run_checker(path)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_new_symbol_cannot_enter_the_frozen_1_0_node(self) -> None:
        candidate = copy.deepcopy(json.loads(BASELINE.read_text(encoding="utf-8")))
        candidate["exports"]["tny_illegal_late_symbol"] = "LIBTNY_1.0"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "candidate.json"
            path.write_text(json.dumps(candidate, sort_keys=True), encoding="utf-8")
            result = self.run_checker(path)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must enter a later minor node", result.stderr)

    @unittest.skipUnless(
        struct.calcsize("P") == 8 and sys.byteorder == "little",
        "the proposed baseline is LP64 little-endian",
    )
    def test_proposed_prefix_matches_the_current_reviewed_header(self) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("C compiler unavailable")
        baseline = json.loads(BASELINE.read_text(encoding="utf-8"))
        header = (ROOT / "include" / "tny" / "tny.h").read_text(encoding="utf-8")
        header_exports = set(
            re.findall(r"TNY_API\s+[^;]*?\b(tny_[a-z0-9_]+)\s*\(", header, re.DOTALL)
        )
        self.assertEqual(header_exports, set(baseline["exports"]))
        lines = [
            "#include <stddef.h>",
            '#include "tny/tny.h"',
        ]
        for name, value in sorted(baseline["constants"].items()):
            if name in {"TNY_ABI_MAJOR", "TNY_ABI_MINOR", "TNY_ABI_VERSION"}:
                continue
            lines.append(f'_Static_assert({name} == {value}, "{name}");')
        for name, definition in sorted(baseline["structs"].items()):
            lines.append(
                f'_Static_assert(sizeof({name}) == {definition["size"]}, "sizeof {name}");'
            )
            lines.append(
                f'_Static_assert(_Alignof({name}) == {definition["alignment"]}, "alignof {name}");'
            )
            for field in definition["fields"]:
                lines.append(
                    f'_Static_assert(offsetof({name}, {field["name"]}) == {field["offset"]}, '
                    f'"offsetof {name}.{field["name"]}");'
                )
                lines.append(
                    f'_Static_assert(sizeof((({name} *)0)->{field["name"]}) == {field["size"]}, '
                    f'"sizeof {name}.{field["name"]}");'
                )
        lines.append("int main(void) { return 0; }")
        completed = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include"),
             "-x", "c", "-", "-fsyntax-only"],
            input="\n".join(lines), text=True, capture_output=True, cwd=ROOT, check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)


if __name__ == "__main__":
    unittest.main()
