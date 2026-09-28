"""Inspectable tool-call structure and retry friction, from retained provider input."""

from __future__ import annotations

import argparse
import ast
import gzip
import json
from collections import Counter
from pathlib import Path

from analyze_eval import output_text


def call_name(node):
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        parent = call_name(node.value)
        return parent + "." + node.attr if parent else node.attr
    return ""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = args.root or Path(
        json.loads((here / "STATE.json").read_text())["temporary_root"]
    )
    observations = []
    for p in sorted((root / "output/primary").glob("*/*/rep-*/result.json")):
        result = json.loads(p.read_text())
        calls = {}
        outputs = {}
        for row in result["request_rows"]:
            request = json.loads(
                gzip.decompress((p.parent / "proxy" / row["body_file"]).read_bytes())
            )
            for item in request.get("input", []):
                if not isinstance(item, dict):
                    continue
                key = item.get("call_id", item.get("id"))
                if item.get("type") in ("function_call", "custom_tool_call"):
                    calls[key] = item
                elif item.get("type") in (
                    "function_call_output",
                    "custom_tool_call_output",
                ):
                    outputs[key] = output_text(item.get("output", ""))
        rows = []
        for key, call in calls.items():
            code = call.get("input", "")
            if call.get("name") == "run_code":
                try:
                    code = json.loads(call["arguments"]).get("code", "")
                except (ValueError, TypeError):
                    code = call.get("arguments", "")
            output = outputs.get(key, "")
            kinds = Counter()
            if call.get("name") == "run_code":
                try:
                    tree = ast.parse(code)
                    for node in ast.walk(tree):
                        if isinstance(node, ast.Call):
                            kinds[call_name(node.func)] += 1
                except SyntaxError:
                    pass
            runtime_failure = output.startswith("error: code:") or output.startswith(
                "error: run_code"
            )
            empty_executable = (
                runtime_failure
                and "PermissionError" in output
                and "Permission denied: ''" in output
                and "sys.executable" in code
            )
            missing_state = runtime_failure and "NameError:" in output
            rows.append(
                {
                    "call_id": key,
                    "tool": call.get("name"),
                    "code_bytes": len(code.encode()),
                    "output_bytes": len(output.encode()),
                    "runtime_failure": runtime_failure,
                    "empty_sys_executable_failure": empty_executable,
                    "missing_cell_state_failure": missing_state,
                    "catalog_discovery": "tools.list(" in code
                    or "tools.describe(" in code,
                    "python_call_sites": dict(kinds),
                    "code": code,
                    "output": output,
                }
            )
        observations.append(
            {
                "harness": result["harness"],
                "task": result["task"],
                "rep": result["rep"],
                "calls": rows,
            }
        )
    totals = {}
    for harness in ("tny", "codex"):
        runs = [r for r in observations if r["harness"] == harness]
        calls = [c for r in runs for c in r["calls"]]
        totals[harness] = {"runs": len(runs), "unique_visible_model_calls": len(calls)}
        for metric in (
            "runtime_failure",
            "empty_sys_executable_failure",
            "missing_cell_state_failure",
            "catalog_discovery",
        ):
            totals[harness][metric + "_calls"] = sum(c[metric] for c in calls)
            totals[harness][metric + "_runs"] = sum(
                any(c[metric] for c in r["calls"]) for r in runs
            )
        totals[harness]["code_bytes"] = sum(c["code_bytes"] for c in calls)
        totals[harness]["largest_tool_output_bytes"] = max(
            (c["output_bytes"] for c in calls), default=0
        )
    report = {
        "totals": totals,
        "runs": observations,
        "limits": "Call-site counts are syntactic, not execution counts. Codex internal native actions and Tny Python operations are not equivalent. Error flags identify only named wrapper failures, not every failed shell command or intentional pre-fix test.",
    }
    (root / "call-diagnostics.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(totals, indent=2))


if __name__ == "__main__":
    main()
