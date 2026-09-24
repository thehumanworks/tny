#!/usr/bin/env python3
"""Recheck saved benchmark workspaces with the current task verifiers."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import stat
import subprocess
from datetime import datetime, timezone
from pathlib import Path

from run import HERE, verification_outcome, verify_task


def git_output(workspace: Path, *args: str) -> bytes:
    return subprocess.run(
        ["git", "-C", str(workspace), *args],
        check=True,
        capture_output=True,
    ).stdout


def saved_repo_manifest(workspace: Path) -> dict[str, tuple[str, str]]:
    """Read the fixture from the workspace's initial Git commit, before setup."""
    top = Path(
        os.fsdecode(git_output(workspace, "rev-parse", "--show-toplevel")).strip()
    )
    if top.resolve() != workspace.resolve():
        raise ValueError(f"{workspace}: no workspace Git repository")
    roots = git_output(workspace, "rev-list", "--max-parents=0", "HEAD").splitlines()
    if len(roots) != 1:
        raise ValueError(f"{workspace}: initial fixture commit is unavailable")
    message = git_output(workspace, "log", "-1", "--format=%s", roots[0].decode())
    if message.strip() != b"initial fixture":
        raise ValueError(f"{workspace}: initial fixture commit is unavailable")
    entries = git_output(workspace, "ls-tree", "-rz", roots[0].decode())
    manifest = {}
    for entry in entries.split(b"\0"):
        if not entry:
            continue
        metadata, name = entry.split(b"\t", 1)
        mode, kind, object_id = metadata.decode().split()
        if kind != "blob":
            raise ValueError(f"{workspace}: unsupported fixture entry")
        manifest[os.fsdecode(name)] = (mode, object_id)
    return manifest


def current_repo_manifest(repo: Path) -> dict[str, tuple[str, str]]:
    manifest = {}
    for path in repo.rglob("*"):
        if path.is_symlink():
            content = os.fsencode(os.readlink(path))
            mode = "120000"
        elif path.is_file():
            content = path.read_bytes()
            mode = "100755" if path.stat().st_mode & stat.S_IXUSR else "100644"
        else:
            continue
        digest = hashlib.sha1(
            b"blob " + str(len(content)).encode() + b"\0" + content
        ).hexdigest()
        manifest[path.relative_to(repo).as_posix()] = (mode, digest)
    return manifest


def contains_exact_prompt(value: object, prompt: str) -> bool:
    if isinstance(value, str):
        return value == prompt
    if isinstance(value, dict):
        return any(contains_exact_prompt(item, prompt) for item in value.values())
    if isinstance(value, list):
        return any(contains_exact_prompt(item, prompt) for item in value)
    return False


def check_prompt(run_dir: Path, result: dict, prompt: str) -> None:
    rows = result.get("request_rows") or []
    if not rows:
        raise ValueError(f"{run_dir}: no recorded request proves the original prompt")
    body_file = rows[0].get("body_file", "")
    proxy_dir = (run_dir / "proxy").resolve()
    body_path = (proxy_dir / body_file).resolve()
    if not body_path.is_relative_to(proxy_dir) or not body_path.is_file():
        raise ValueError(f"{run_dir}: first recorded request is unavailable")
    body = json.loads(gzip.decompress(body_path.read_bytes()))
    messages = body.get("input") if isinstance(body, dict) else None
    if not isinstance(messages, list) or not any(
        isinstance(message, dict)
        and message.get("role") == "user"
        and contains_exact_prompt(message.get("content"), prompt)
        for message in messages
    ):
        raise ValueError(f"{run_dir}: task prompt changed or cannot be proven")


def inspect_run(result_file: Path, tasks_dir: Path) -> tuple[Path, Path, dict, dict]:
    run_dir = result_file.parent
    result = json.loads(result_file.read_text())
    task_id = result.get("task")
    if not isinstance(task_id, str) or task_id != run_dir.parent.name:
        raise ValueError(f"{run_dir}: task identity is inconsistent")
    task_dir = tasks_dir / task_id
    if not task_dir.is_dir():
        raise ValueError(f"{run_dir}: current task is unavailable")
    task = json.loads((task_dir / "task.json").read_text())
    if task.get("id") != task_id:
        raise ValueError(f"{run_dir}: current task ID changed")
    workspace = run_dir / "workspace"
    message_file = run_dir / "final_message.txt"
    if not workspace.is_dir() or not message_file.is_file():
        raise ValueError(f"{run_dir}: saved workspace or final message is unavailable")
    if saved_repo_manifest(workspace) != current_repo_manifest(task_dir / "repo"):
        raise ValueError(f"{run_dir}: task repo/ changed; refusing to rescore")
    check_prompt(run_dir, result, task["prompt"])
    return task_dir, workspace, result, task


def rescored_result(
    result: dict, task: dict, verify_status: str, reason: str, verify_rc: int
) -> dict:
    updated = result.copy()
    updated.setdefault("pass_original", result["pass"])
    updated.setdefault("status_original", result.get("status"))
    updated.setdefault("timeout_original", result.get("timeout"))
    updated.setdefault("category_original", result.get("category"))
    updated.setdefault("tags_original", result.get("tags", []))
    harness_timed_out = bool(
        result.get("timeout_original") and result.get("exit_code") is None
    )
    adapter_error = str(result.get("reason", "")).startswith("error: adapter")
    passed = (
        verify_status == "pass"
        and result.get("exit_code") == 0
        and not harness_timed_out
    )
    if adapter_error:
        passed = False
        reason = result["reason"]
    elif verify_status == "error":
        passed = False
    elif harness_timed_out:
        reason = "task timed out"
    elif result.get("exit_code") != 0:
        reason = f"harness exit {result.get('exit_code')}: {reason}"
    elif not result.get("request_rows"):
        passed = False
        reason = "no proxy requests"
    elif not result.get("measurement_valid"):
        passed = False
        reason = "provider usage missing from a recorded request"
    elif not result.get("model_effort_valid"):
        passed = False
        reason = "wire model or reasoning effort differs from requested setting"
    updated["pass"] = passed
    updated["status"] = (
        "error"
        if adapter_error or verify_status == "error"
        else ("pass" if passed else "fail")
    )
    updated["reason"] = reason
    updated["timeout"] = harness_timed_out or verify_rc == 124
    updated["category"] = task["category"]
    updated["tags"] = task.get("tags", [])
    updated["verify_rescored_at"] = datetime.now(timezone.utc).isoformat()
    return updated


def rescore(runs_dir: Path, tasks_dir: Path) -> int:
    result_files = sorted(runs_dir.glob("*/*/rep-*/result.json"))
    if not result_files:
        raise ValueError(f"{runs_dir}: no saved run results found")
    # Check every saved fixture before running any verifier or rewriting a result.
    inspected = [(path, *inspect_run(path, tasks_dir)) for path in result_files]
    rewritten = []
    for result_file, task_dir, workspace, result, task in inspected:
        message_file = result_file.parent / "final_message.txt"
        verify = verify_task(
            task_dir, workspace, message_file, task.get("verify_timeout_s", 120)
        )
        verify_status, reason = verification_outcome(verify, message_file)
        if verify_status == "error":
            raise RuntimeError(f"{result_file.parent}: {reason}")
        rewritten.append(
            (
                result_file,
                rescored_result(result, task, verify_status, reason, verify.returncode),
            )
        )
    for result_file, result in rewritten:
        temporary = result_file.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(result, indent=2) + "\n")
        temporary.replace(result_file)
    return len(rewritten)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs_dir", type=Path)
    parser.add_argument("--tasks-dir", type=Path, default=HERE / "tasks")
    args = parser.parse_args()
    try:
        count = rescore(args.runs_dir.resolve(), args.tasks_dir.resolve())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"rescore: {error}\n")
    print(f"rescored {count} saved runs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
