#!/usr/bin/env python3
"""Run original shell fixtures from two Git revisions without building tny."""

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

REPORT_ROOTS = (
    Path.home() / "Library/Logs/DiagnosticReports",
    Path("/Library/Logs/DiagnosticReports"),
)


def shell_reports():
    reports = {}
    for root in REPORT_ROOTS:
        try:
            for path in root.iterdir():
                if (
                    path.name.lower().startswith("zsh")
                    and path.suffix in (".ips", ".crash")
                    and path.is_file()
                    and not path.is_symlink()
                ):
                    reports[path] = path.stat().st_mtime_ns
        except (FileNotFoundError, PermissionError):
            pass
    return reports


def collect_reports(output, initial, collected):
    for path, modified in shell_reports().items():
        if modified == initial.get(path) or modified == collected.get(path):
            continue
        target = (
            output
            / "crash-reports"
            / (str(REPORT_ROOTS.index(path.parent)) + "-" + path.name)
        )
        target.parent.mkdir(exist_ok=True)
        shutil.copy2(path, target)
        collected[path] = modified
        print(json.dumps({"crash_report": str(target)}), flush=True)
        text = target.read_text(errors="replace")
        objects = []
        while text.lstrip().startswith("{"):
            try:
                value, end = json.JSONDecoder().raw_decode(text.lstrip())
            except ValueError:
                break
            objects.append(value)
            text = text.lstrip()[end:]
        body = next((obj for obj in objects if "threads" in obj), None)
        if body is not None:
            thread = body.get("faultingThread", 0)
            threads = body.get("threads", [])
            frames = (
                threads[thread].get("frames", [])[:12]
                if 0 <= thread < len(threads)
                else []
            )
            print(
                json.dumps(
                    {
                        "exception": body.get("exception"),
                        "termination": body.get("termination"),
                        "frames": frames,
                        "images": body.get("usedImages", []),
                    }
                ),
                flush=True,
            )
        else:
            lines = target.read_text(errors="replace").splitlines()
            for index, line in enumerate(lines):
                if re.match(r"Thread \d+ Crashed", line):
                    print("\n".join(lines[index : index + 14]), flush=True)


def git_bytes(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args])


def focused_fixture(source):
    first = (
        b"# Parallel roots, deterministic dependency context, option fidelity, and a\n"
    )
    last = b"# Scheduler termination reaches every active worker and waits for its tny child.\n"
    for marker in (first, last):
        if source.count(marker) != 1:
            raise ValueError(f"fixture marker must occur exactly once: {marker!r}")
    begin, end = source.index(first), source.index(last)
    if begin >= end:
        raise ValueError("fixture marker order changed")
    # Both spans are copied byte-for-byte: setup, then BOTH cancellation cases.
    return source[:begin] + source[end:]


def run_fixture(shell, fixture, log, timeout, trace=False):
    env = os.environ.copy()
    env.update(BASH="/bin/bash", ZSH=shell)
    started = time.monotonic()
    timed_out = False
    with log.open("wb") as output:
        process = subprocess.Popen(
            [shell, *(["-x"] if trace else []), str(fixture)],
            stdout=output,
            stderr=subprocess.STDOUT,
            env=env,
            start_new_session=True,
        )
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            # Give the original fixture's cleanup traps a chance to run.
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
            code = 124
    return {
        "exit": code,
        "timeout": timed_out,
        "seconds": round(time.monotonic() - started, 3),
        "log": str(log),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument(
        "--baseline", default="032277e94f22666b7992abbb7f1771f4702cc69d"
    )
    parser.add_argument(
        "--candidate", default="e222c1eda3650a03d201c71cfc8b7c8a55bb0c02"
    )
    parser.add_argument(
        "--output", type=Path, default=os.environ.get("DIAGNOSTIC_OUTPUT")
    )
    parser.add_argument("--shell", default="/bin/zsh")
    parser.add_argument("--repeats", type=int, default=10)
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    if args.output is None:
        parser.error("--output or DIAGNOSTIC_OUTPUT is required")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    initial_reports = shell_reports()
    collected_reports = {}
    metadata = {
        "shell": subprocess.check_output([args.shell, "--version"], text=True).strip(),
        "platform": subprocess.check_output(["uname", "-a"], text=True).strip(),
        "runner_image": {
            key: os.environ.get(key) for key in ("ImageOS", "ImageVersion")
        },
        "revisions": {},
        "results": [],
        "planned_runs": 2 * args.repeats + 2,
        "complete": False,
    }
    fixtures = {}
    for label, ref in (("baseline", args.baseline), ("candidate", args.candidate)):
        commit = (
            git_bytes(args.repo, "rev-parse", "--verify", f"{ref}^{{commit}}")
            .decode()
            .strip()
        )
        library = git_bytes(args.repo, "show", f"{commit}:shell/tny-workflows.sh")
        full = git_bytes(args.repo, "show", f"{commit}:tests/shell/test_workflows.sh")
        focused = focused_fixture(full)
        metadata["revisions"][label] = {
            "commit": commit,
            "library_sha256": hashlib.sha256(library).hexdigest(),
            "full_fixture_sha256": hashlib.sha256(full).hexdigest(),
            "focused_fixture_sha256": hashlib.sha256(focused).hexdigest(),
        }
        for kind, body in (("focused", focused), ("full", full)):
            root = output / "fixtures" / f"{label}-{kind}"
            (root / "shell").mkdir(parents=True)
            (root / "shell" / "tny-workflows.sh").write_bytes(library)
            target = root / "tests" / "shell" / "test_workflows.sh"
            target.parent.mkdir(parents=True)
            target.write_bytes(body)
            fixtures[label, kind] = target
    focused_hashes = {
        revision["focused_fixture_sha256"]
        for revision in metadata["revisions"].values()
    }
    if len(focused_hashes) != 1:
        raise ValueError("baseline and candidate focused fixtures differ")
    metadata["identical_libraries"] = (
        len({revision["library_sha256"] for revision in metadata["revisions"].values()})
        == 1
    )
    print(
        json.dumps({key: value for key, value in metadata.items() if key != "results"}),
        flush=True,
    )

    def run(label, kind, iteration, timeout):
        result = {"revision": label, "kind": kind, "iteration": iteration}
        result.update(
            run_fixture(
                args.shell,
                fixtures[label, kind],
                output / f"{label}-{kind}-{iteration:02d}.log",
                timeout,
                trace=kind == "full",
            )
        )
        metadata["results"].append(result)
        (output / "results.json").write_text(json.dumps(metadata, indent=2) + "\n")
        print(json.dumps(result), flush=True)
        if result["exit"]:
            print(Path(result["log"]).read_text(errors="replace")[-8000:], flush=True)
        collect_reports(output, initial_reports, collected_reports)

    for iteration in range(1, args.repeats + 1):
        for label in ("baseline", "candidate"):
            run(label, "focused", iteration, 90)
    for label in ("baseline", "candidate"):
        run(label, "full", 1, 180)
    failures = sum(result["exit"] != 0 for result in metadata["results"])
    if failures:
        time.sleep(3)
    collect_reports(output, initial_reports, collected_reports)
    metadata["complete"] = True
    (output / "results.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(
        json.dumps({"completed": len(metadata["results"]), "failed": failures}),
        flush=True,
    )
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
