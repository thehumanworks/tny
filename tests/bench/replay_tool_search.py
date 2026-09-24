#!/usr/bin/env python3
"""Replay saved file-search argument shapes on disposable synthetic trees.

Both arms compile the real C file tools through bench_edit_feedback.c. Session
JSON is read only; every path is remapped under TMPDIR. This measures whether
the requested shape can find a seeded match, not whether the original project
contained that match. No provider is used.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import platform
import subprocess
import tempfile
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = (
    "src/core/tools.c",
    "src/core/edit.c",
    "src/core/image.c",
    "src/util/util.c",
    "src/util/alloc.c",
    "src/util/parallel.c",
    "src/json/json.c",
    "third_party/yyjson/yyjson.c",
)


def run(cmd: list[str], **kw: object) -> str:
    return subprocess.run(cmd, check=True, text=True, capture_output=True, **kw).stdout


def compile_arm(source: str, output: Path, selected: Path) -> None:
    selected.write_text(source)
    cmd = [
        "cc",
        "-O2",
        "-std=c11",
        "-D_DEFAULT_SOURCE",
        "-D_DARWIN_C_SOURCE",
        "-Dyyjson_api=",
        "-ffunction-sections",
        "-fdata-sections",
        "-Isrc",
        "-Iinclude",
        "-Ithird_party/yyjson",
        f'-DTNY_BENCH_TOOLS_FS="{selected}"',
        "tests/bench/bench_edit_feedback.c",
        *SOURCES,
        "-pthread",
        "-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections",
        "-o",
        str(output),
    ]
    run(cmd, cwd=ROOT)


def calls(session_root: Path) -> list[tuple[str, dict]]:
    result = []
    for name in glob.glob(str(session_root / "*" / "*" / "session.json")):
        try:
            session = json.loads(Path(name).read_text())
        except (OSError, ValueError):
            continue
        for message in session.get("messages", []):
            for call in message.get("tool_calls", []):
                fn = call.get("function") or {}
                if fn.get("name") not in ("grep_files", "glob_files"):
                    continue
                try:
                    args = json.loads(fn.get("arguments", "{}"))
                except ValueError:
                    continue
                if isinstance(args, dict) and isinstance(args.get("pattern"), str):
                    result.append((fn["name"], args))
    return result


def mapped_path(work: Path, original: str) -> Path:
    parts = Path(original).parts
    if "node_modules" in parts:
        tail = parts[parts.index("node_modules") :]
        return work.joinpath(
            *(part if part not in (".", "..") else "_parent_" for part in tail)
        )
    if original.endswith((".md", ".js", ".ts", ".c", ".h", ".json", ".py")):
        return work / Path(original).name
    return work / "named"


def seed(work: Path, tool: str, args: dict) -> dict:
    original = args.get("path")
    target = (
        mapped_path(work, original) if isinstance(original, str) and original else work
    )
    payload = dict(args)
    if original:
        payload["path"] = str(target)
    pattern = args["pattern"]
    if tool == "grep_files":
        is_file = target.suffix in (".md", ".js", ".ts", ".c", ".h", ".json", ".py")
        file = (
            target
            if is_file
            else target
            / (
                "dist/core/agent-session.js"
                if "node_modules" in target.parts
                else "fixture.txt"
            )
        )
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(pattern + "\n")
    else:
        if pattern.startswith(str(original) + "/") and original:
            pattern = pattern[len(str(original)) + 1 :]
            payload["pattern"] = str(target) + "/" + pattern
        elif pattern.startswith("/"):
            pattern = Path(pattern).name
            payload["pattern"] = str(target) + "/" + pattern
        filename = pattern.replace("**/", "").replace("*", "fixture").replace("?", "x")
        filename = filename.lstrip("/") or "fixture.txt"
        filename = "/".join(
            part if part not in (".", "..") else "_parent_"
            for part in Path(filename).parts
        )
        if target.suffix:
            file = target
        else:
            file = target / filename
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text("fixture\n")
    return payload


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True, help="pre-change git revision")
    parser.add_argument("--sessions", type=Path, default=Path.home() / ".tny/sessions")
    parser.add_argument("--out", type=Path)
    opt = parser.parse_args()
    baseline = run(["git", "show", f"{opt.baseline}:src/core/tools_fs.c"], cwd=ROOT)
    current = (ROOT / "src/core/tools_fs.c").read_text()
    cases = calls(opt.sessions)
    totals = {arm: Counter() for arm in ("before", "after")}
    with tempfile.TemporaryDirectory(
        prefix="tny-tool-search-", dir=os.environ.get("TMPDIR")
    ) as td:
        scratch = Path(td)
        binaries = {}
        for arm, source in (("before", baseline), ("after", current)):
            binary = scratch / arm
            compile_arm(source, binary, scratch / f"{arm}.c")
            binaries[arm] = binary
        for index, (tool, args) in enumerate(cases):
            work = scratch / "cases" / str(index)
            work.mkdir(parents=True)
            payload = seed(work, tool, args)
            request = work / "request.json"
            request.write_text(json.dumps(payload))
            for arm, binary in binaries.items():
                proc = subprocess.run(
                    [str(binary), str(work), tool, str(request)],
                    text=True,
                    capture_output=True,
                    timeout=30,
                )
                key = (
                    "no_match"
                    if proc.stdout.startswith("(no matches")
                    else (
                        "matched"
                        if proc.returncode == 0 and not proc.stdout.startswith("error:")
                        else "error"
                    )
                )
                totals[arm][f"{tool}_{key}"] += 1
    report = {
        "source": "saved argument shapes, synthetic local fixture trees",
        "baseline": opt.baseline,
        "recorded_calls": len(cases),
        "fixture_contents": "exact grep pattern as literal text, including special characters",
        "before": dict(totals["before"]),
        "after": dict(totals["after"]),
    }
    data = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if opt.out:
        opt.out.write_text(data)
    print(data, end="")


if __name__ == "__main__":
    main()
