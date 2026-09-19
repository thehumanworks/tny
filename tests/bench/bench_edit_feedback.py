#!/usr/bin/env python3
"""Reproduce edit-feedback baseline/A/B with actual compiled file-tool outputs.

Run: python3 tests/bench/bench_edit_feedback.py --out /tmp/edit-feedback.json
No provider calls or latency/token claims. The frozen 18 one-character typo
cases come from --baseline, plus four diagnostic boundary cases. Every arm
must preserve failed-edit bytes and make exact corrected retries. Baseline
uses an oracle-quality one-hit grep for recovery; A uses its returned snippet;
B uses its returned line number and a one-line read. Requests and common
retry responses are excluded from recovery-result byte counts. Scratch roots
are normalized to /scratch so reruns have identical path-dependent sizes.

A compiles the production tools_fs.c unchanged. Baseline substitutes that
file from the pinned revision, with the shared editor checked unchanged. B
substitutes only A's diagnostic formatting in a temporary source copy. All
three invoke tool_fs_execute, including real grep_files/read_file followups.
The tiny no-session driver aborts if result storage is unexpectedly called.

Original scout (18 typo cases, different path rendering, simulated A/B):
baseline+ideal grep 3208 bytes; A 3028; B+read 3946; evidence calls 18/0/18.
Those historical numbers are not assertions about the production wording.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASELINE = "72bafe6e6c3e863f0e3f15cfa2626d7bc140b7c8"
CORPUS = (
    "src/core/tools_fs.c",
    "src/core/tools.c",
    "src/core/tools_ext.c",
    "src/core/edit.c",
    "tests/test_edit.c",
    "docs/features/mcp-and-skills.md",
)
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
HINT = "\nAdvisory (first nonempty search line only), line "


def checked(command: list[str], **kwargs) -> str:
    return subprocess.run(
        command, check=True, capture_output=True, text=True, timeout=120, **kwargs
    ).stdout


def historical(revision: str, path: str) -> str:
    return checked(["git", "show", f"{revision}:{path}"], cwd=ROOT)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def line_only(source: str) -> str:
    changes = (
        (
            r"\nAdvisory (first nonempty search line only), line %zu: ",
            r"\nAdvisory (first nonempty search line only), line %zu; "
            "read current context before retrying.",
        ),
        (
            "buf_append(&msg, result.nearest_context, cut);",
            "/* Line-only arm: no snippet. */",
        ),
        ('if (cut < len) buf_appends(&msg, " [truncated]");', ""),
    )
    for before, after in changes:
        if source.count(before) != 1:
            raise ValueError(
                "production formatting changed; update the B-only substitution"
            )
        source = source.replace(before, after)
    return source


def corpus(revision: str) -> tuple[list[dict], dict]:
    cases = []
    hashes = {}
    for index, name in enumerate(CORPUS):
        text = historical(revision, name)
        hashes[name] = digest(text.encode())
        eligible = [
            (i + 1, line)
            for i, line in enumerate(text.splitlines())
            if 40 <= len(line) <= 140
            and len(re.findall("[a-zA-Z]", line)) > 25
            and text.count(line) == 1
        ]
        for k in range(3):
            number, line = eligible[(len(eligible) - 1) * (k + 1) // 4]
            pos = max(m.start() for m in re.finditer("[a-zA-Z]", line))
            typo = line[:pos] + ("Q" if line[pos] != "Q" else "Z") + line[pos + 1 :]
            assert typo not in text
            cases.append(
                dict(
                    name=f"real-{index}-{k}",
                    text=text,
                    old=typo,
                    line=number,
                    context=line,
                )
            )
    cases.extend(
        [
            dict(
                name="tie",
                text="alpha\ncorrect target\ncorrect target\nomega\n",
                old="correct targat",
                line=0,
                context=None,
            ),
            dict(
                name="multiline",
                text="header\nfirst line\nsecond changed\nfooter\n",
                old="first line\nsecond old",
                line=2,
                context="first line",
            ),
            dict(
                name="long",
                text="x" * 600 + "target\n",
                old="x" * 600 + "targat",
                line=1,
                context="x" * 600 + "target",
            ),
            dict(name="ambiguous", text="old\nold\n", old="old", line=0, context=None),
        ]
    )
    return cases, hashes


def run(args: argparse.Namespace) -> dict:
    baseline = historical(args.baseline, "src/core/tools_fs.c")
    production = (ROOT / "src/core/tools_fs.c").read_text()
    if (
        historical(args.baseline, "src/core/edit.c")
        != (ROOT / "src/core/edit.c").read_text()
    ):
        raise ValueError(
            "shared editor changed since baseline; use separately matched builds"
        )
    variants = {"baseline": baseline, "A": production, "B": line_only(production)}
    cases, hashes = corpus(args.baseline)
    report = dict(
        baseline_revision=args.baseline,
        compiler=checked([*shlex.split(args.cc), "--version"]).splitlines()[0],
        platform=platform.platform(),
        measurement="scripted recovery result bytes and evidence calls, not tokens or latency",
        corpus_sha256=hashes,
        sources_sha256={name: digest((ROOT / name).read_bytes()) for name in SOURCES},
        harness_sha256={
            name: digest((ROOT / name).read_bytes())
            for name in (
                "tests/bench/bench_edit_feedback.py",
                "tests/bench/bench_edit_feedback.c",
            )
        },
        variant_sha256={
            arm: digest(source.encode()) for arm, source in variants.items()
        },
        builds=[],
        cases=[],
    )
    totals = {
        arm: dict(
            recovery_bytes=0,
            evidence_calls=0,
            failed_files_preserved=0,
            exact_retries_verified=0,
        )
        for arm in variants
    }
    with tempfile.TemporaryDirectory(prefix="tny-edit-feedback-") as temporary:
        scratch = Path(temporary)
        binaries = {}
        for arm, source in variants.items():
            selected = scratch / f"{arm}_tools_fs.c"
            selected.write_text(source)
            binary = scratch / arm
            command = [
                *shlex.split(args.cc),
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
                "-Wl,-dead_strip"
                if platform.system() == "Darwin"
                else "-Wl,--gc-sections",
                "-o",
                str(binary),
            ]
            checked(command, cwd=ROOT)
            report["builds"].append(
                dict(
                    arm=arm, command=command, binary_sha256=digest(binary.read_bytes())
                )
            )
            binaries[arm] = binary

        def call(arm: str, tool: str, payload: dict) -> str:
            request = scratch / "request.json"
            request.write_text(json.dumps(payload))
            result = checked([str(binaries[arm]), str(scratch), tool, str(request)])
            return result.replace(str(scratch), "/scratch")

        for case in cases:
            name = case["name"] + ".txt"
            path = scratch / name
            original = case["text"].encode()
            outputs = {}
            for arm in variants:
                path.write_bytes(original)
                response = call(
                    arm,
                    "edit_file",
                    dict(path=name, old_string=case["old"], new_string="replacement"),
                )
                assert response.startswith("error: "), (arm, name, response)
                assert path.read_bytes() == original, (arm, name)
                totals[arm]["failed_files_preserved"] += 1
                assert (HINT in response) == (
                    arm != "baseline" and case["context"] is not None
                )
                if arm == "A" and case["context"] is not None:
                    snippet = (
                        case["context"].encode()[:300].decode("utf-8", errors="ignore")
                    )
                    suffix = " [truncated]" if snippet != case["context"] else ""
                    assert response.endswith(f"{HINT}{case['line']}: {snippet}{suffix}")
                outputs[arm] = dict(failure=response)
                if not case["name"].startswith("real-"):
                    continue
                recovery = ""
                if arm == "baseline":
                    # Optimistic oracle knows a pattern yielding exactly the right line.
                    recovery = call(
                        arm, "grep_files", dict(path=name, pattern=case["context"])
                    )
                    assert recovery == f"{name}:{case['line']}:{case['context']}\n"
                    retry_text = recovery.split(":", 2)[2].removesuffix("\n")
                elif arm == "B":
                    hint = response.split(HINT, 1)[1]
                    offset = int(hint.split(";", 1)[0])
                    recovery = call(
                        arm, "read_file", dict(path=name, offset=offset, limit=1)
                    )
                    assert recovery == case["context"] + "\n"
                    retry_text = recovery.removesuffix("\n")
                else:
                    retry_text = response.split(HINT, 1)[1].split(": ", 1)[1]
                assert retry_text == case["context"]
                totals[arm]["recovery_bytes"] += len((response + recovery).encode())
                totals[arm]["evidence_calls"] += int(bool(recovery))
                outputs[arm]["recovery"] = recovery
                corrected = call(
                    arm,
                    "edit_file",
                    dict(path=name, old_string=retry_text, new_string="replacement"),
                )
                assert corrected.startswith("replaced 1 occurrence"), (
                    arm,
                    name,
                    corrected,
                )
                expected = original.replace(case["context"].encode(), b"replacement", 1)
                assert path.read_bytes() == expected, (arm, name)
                totals[arm]["exact_retries_verified"] += 1
            report["cases"].append(
                dict(name=case["name"], source_sha256=digest(original), outputs=outputs)
            )
    report["summary"] = dict(cases=len(cases), recovery_cases=18, arms=totals)
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", default=BASELINE)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    report = run(args)
    if args.out:
        args.out.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["summary"], indent=2))


if __name__ == "__main__":
    main()
