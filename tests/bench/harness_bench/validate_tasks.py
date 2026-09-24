#!/usr/bin/env python3
"""Check every coding-agent task against untouched and reference workspaces."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).parent / "tasks"
IDS = (
    "log-triage",
    "verbose-test-failure",
    "bigfile-fix",
    "bugfix-multimodule",
    "rename-refactor",
    "c-segfault",
    "codebase-qa",
    "perf-fix",
    "cli-feature",
    "feature-ledger",
    "workflow-dag",
    "build-fix",
)


def run(*args: str, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=cwd, text=True, capture_output=True, check=False)


def apply_solution(task: Path, workspace: Path) -> None:
    overlay = task / "solution"
    patch = task / "solution.patch"
    if overlay.is_dir():
        for source in overlay.rglob("*"):
            if source.is_file():
                target = workspace / source.relative_to(overlay)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
                target.touch()
        for cache in workspace.rglob("__pycache__"):
            shutil.rmtree(cache)
    elif patch.is_file():
        result = run("git", "apply", str(patch), cwd=workspace)
        if result.returncode:
            raise AssertionError(f"patch failed: {result.stderr}")
    else:
        raise AssertionError("missing reference solution")


def check_answers(task: Path) -> None:
    repo = task / "repo"
    solution = task / "solution"
    for name in ("ANSWER.txt", "ANSWERS.json"):
        answer = solution / name
        if not answer.is_file():
            continue
        expected = answer.read_text().strip()
        for source in repo.rglob("*"):
            if source.is_file() and expected in source.read_text(errors="replace"):
                raise AssertionError(f"{source} embeds complete answer")
        if (repo / name).exists():
            raise AssertionError(f"{name} present in initial workspace")


def check_task(task: Path, tmp_root: Path) -> tuple[bool, bool]:
    info = json.loads((task / "task.json").read_text())
    if info["id"] != task.name:
        raise AssertionError("task id mismatch")
    repo = task / "repo"
    if sum(p.stat().st_size for p in repo.rglob("*") if p.is_file()) >= 300_000:
        raise AssertionError("initial repo exceeds 300 KB")
    check_answers(task)
    with tempfile.TemporaryDirectory(prefix=f"task-{task.name}-", dir=tmp_root) as tmp:
        workspace = Path(tmp) / "workspace"
        shutil.copytree(repo, workspace)
        for args in (
            ("git", "init", "-q"),
            ("git", "add", "."),
            (
                "git",
                "-c",
                "user.name=Harness Benchmark",
                "-c",
                "user.email=benchmark@example.invalid",
                "commit",
                "-qm",
                "initial",
            ),
        ):
            result = run(*args, cwd=workspace)
            if result.returncode:
                raise AssertionError(f"git setup: {result.stderr}")
        setup = task / "setup.sh"
        if setup.exists():
            result = run(str(setup), str(workspace), cwd=workspace)
            if result.returncode:
                raise AssertionError(f"setup failed: {result.stderr}")
        final = Path(tmp) / "final.txt"
        final.write_text("")
        verify = task / "verify.sh"
        before = run(str(verify), str(workspace), str(final), cwd=workspace)
        apply_solution(task, workspace)
        after = run(str(verify), str(workspace), str(final), cwd=workspace)
        if after.returncode:
            print(f"  solved stderr: {after.stderr.strip()[:1200]}")
            print(f"  solved stdout: {after.stdout.strip()[:500]}")
        return before.returncode != 0, after.returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tasks_dir", nargs="?", type=Path)
    parser.add_argument("--tasks-dir", dest="tasks_dir_option", type=Path)
    args = parser.parse_args()
    tasks_dir = (args.tasks_dir_option or args.tasks_dir or ROOT).resolve()
    if not tasks_dir.is_dir():
        parser.error(f"tasks directory does not exist: {tasks_dir}")
    names = (
        IDS
        if tasks_dir.resolve() == ROOT.resolve()
        else tuple(sorted(path.parent.name for path in tasks_dir.glob("*/task.json")))
    )
    if not names:
        parser.error(f"no tasks found in {tasks_dir}")
    tmp_root = Path(os.environ.get("TMPDIR", tempfile.gettempdir()))
    tmp_root.mkdir(parents=True, exist_ok=True)
    results = []
    print(f"{'task':24} {'untouched fails':16} reference passes")
    for name in names:
        task = tasks_dir / name
        try:
            before, after = check_task(task, tmp_root)
            results.append(before and after)
            print(f"{name:24} {str(before):16} {after}")
        except Exception as exc:
            results.append(False)
            print(f"{name:24} ERROR {type(exc).__name__}: {exc}")
    count = sum(results)
    print(f"{count}/{len(names)} tasks fail before and pass after reference solution")
    return 0 if count == len(names) else 1


if __name__ == "__main__":
    raise SystemExit(main())
