#!/usr/bin/env python3
"""Opt-in matched live Codex/tny-swarm evaluation; no inference without --live.

Raw logs and isolated account/state directories stay in the private output root.
Only result.json is intended for review/publication after inspection. Existing
credentials are referenced, never embedded in commands, logs or result records.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from swarm_cases import CASES, COMMON_PROMPT, Case, prepare

TERMINAL = {"succeeded", "failed", "cancelled", "interrupted"}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def records(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        return []
    result = []
    for line in path.read_text(errors="replace").splitlines():
        try:
            value = json.loads(line)
        except ValueError:
            continue
        if isinstance(value, dict):
            result.append(value)
    return result


def atomic_json(path: Path, value: Any) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def tny_metrics(home: Path, lead_events: Path) -> dict[str, Any]:
    totals = {
        "input_tokens": 0,
        "output_tokens": 0,
        "cached_input_tokens": 0,
        "provider_requests": 0,
        "tool_calls": 0,
    }
    sessions = []
    for path in sorted((home / ".tny/sessions").rglob("session.json")):
        value = json.loads(path.read_text())
        usage = value.get("usage", {})
        sessions.append(
            {
                "input_tokens": usage.get("in"),
                "output_tokens": usage.get("out"),
                "cached_input_tokens": usage.get("cached_in"),
                "provider_requests": usage.get("requests"),
                "cache_coverage_complete": bool(usage.get("requests"))
                and usage.get("requests") == usage.get("cache_read_requests"),
            }
        )
        for source, target in (
            ("in", "input_tokens"),
            ("out", "output_tokens"),
            ("cached_in", "cached_input_tokens"),
            ("requests", "provider_requests"),
        ):
            if isinstance(usage.get(source), int):
                totals[target] += usage[source]
    event_files = [lead_events, *(home / ".tny/jobs").rglob("*.log")]
    totals["tool_calls"] = sum(
        event.get("type") == "tool_start"
        for path in event_files
        for event in records(path)
    )
    jobs = []
    for path in sorted((home / ".tny/jobs").rglob("job.json")):
        value = json.loads(path.read_text())
        jobs.append(
            {
                "state": value.get("state"),
                "items": [
                    {
                        "label": item.get("label"),
                        "state": item.get("state"),
                        "model": item.get("model"),
                        "attempt": item.get("attempt"),
                        "usage_known": item.get("usage_known"),
                        "launched": bool(item.get("session_id")),
                    }
                    for item in value.get("items", [])
                ],
            }
        )
    return {
        **totals,
        "sessions": sessions,
        "session_count": len(sessions),
        "jobs": jobs,
        "launched_collaborators": sum(
            item["launched"] for job in jobs for item in job["items"]
        ),
        "usage_complete": bool(sessions)
        and all(
            s["input_tokens"] is not None and s["output_tokens"] is not None
            for s in sessions
        ),
        "cache_coverage_complete": bool(sessions)
        and all(s["cache_coverage_complete"] for s in sessions),
    }


def codex_metrics(codex_home: Path, lead_events: Path) -> dict[str, Any]:
    sessions: dict[str, dict[str, Any]] = {}
    tool_calls = 0
    for path in sorted((codex_home / "sessions").rglob("*.jsonl")):
        identity = str(path)
        last_usage = None
        completed = False
        models: set[str] = set()
        for event in records(path):
            payload = event.get("payload", {})
            if not isinstance(payload, dict):
                continue
            if event.get("type") == "session_meta":
                identity = payload.get("id", identity)
            if event.get("type") == "turn_context" and isinstance(
                payload.get("model"), str
            ):
                models.add(payload["model"])
            if payload.get("type") == "token_count":
                last_usage = (payload.get("info") or {}).get(
                    "total_token_usage"
                ) or last_usage
            if payload.get("type") in (
                "task_complete",
                "turn_complete",
                "turn_completed",
            ):
                completed = True
            if event.get("type") == "response_item" and payload.get("type") in (
                "function_call",
                "custom_tool_call",
                "local_shell_call",
            ):
                tool_calls += 1
        if last_usage:
            sessions[identity] = {
                **last_usage,
                "completed": completed,
                "models": sorted(models),
            }
    events = records(lead_events)
    # Fallback is explicitly partial if the installed CLI does not emit rollout usage.
    fallback = False
    if not sessions:
        usage = next(
            (
                event.get("usage")
                for event in reversed(events)
                if event.get("type") == "turn.completed"
            ),
            None,
        )
        if usage:
            sessions["root-only"] = usage
            fallback = True
    totals = {
        field: sum(int(s.get(field, 0)) for s in sessions.values())
        for field in ("input_tokens", "output_tokens", "cached_input_tokens")
    }
    if not tool_calls:
        tool_calls = sum(
            event.get("type") == "item.completed"
            and event.get("item", {}).get("type")
            in ("command_execution", "mcp_tool_call")
            for event in events
        )
    return {
        **totals,
        "tool_calls": tool_calls,
        "provider_requests": None,
        "sessions": list(sessions.values()),
        "session_count": len(sessions),
        "launched_collaborators": max(0, len(sessions) - 1),
        "usage_complete": bool(sessions) and not fallback,
        "cache_coverage_complete": bool(sessions)
        and all("cached_input_tokens" in item for item in sessions.values()),
        "root_stream_turn_completed": any(
            e.get("type") == "turn.completed" for e in events
        ),
        "rollout_usage_fallback": fallback,
    }


def stop_owned_process(process: subprocess.Popen[Any]) -> int:
    """Only signal the new process group we created and still own as a child."""
    for sig, timeout in ((signal.SIGINT, 8), (signal.SIGTERM, 5), (signal.SIGKILL, 5)):
        if process.poll() is not None:
            return process.returncode
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            return process.wait(timeout=5)
        try:
            return process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            continue
    raise RuntimeError("owned process did not terminate")


def settle_jobs(tny: Path, home: Path, env: dict[str, str]) -> list[dict[str, Any]]:
    """Bound observation, then cancel only unfinished jobs in this trial's private HOME."""
    outcomes = []
    for path in sorted((home / ".tny/jobs").rglob("job.json")):
        job = json.loads(path.read_text())
        run = job.get("id") or path.parent.name
        if job.get("state") in TERMINAL:
            continue
        waited = subprocess.run(
            [str(tny), "jobs", "wait", run, "--timeout", "3", "--json"],
            env=env,
            text=True,
            capture_output=True,
            timeout=10,
        )
        fresh = json.loads(path.read_text())
        if fresh.get("state") in TERMINAL:
            continue
        cancelled = subprocess.run(
            [
                str(tny),
                "jobs",
                "cancel",
                run,
                "--expected-attempt",
                str(fresh.get("attempt", 1)),
                "--json",
            ],
            env=env,
            text=True,
            capture_output=True,
            timeout=15,
        )
        final = subprocess.run(
            [str(tny), "jobs", "wait", run, "--timeout", "10", "--json"],
            env=env,
            text=True,
            capture_output=True,
            timeout=15,
        )
        outcomes.append(
            {
                "unfinished_after_lead": True,
                "observation_exit": waited.returncode,
                "cancel_exit": cancelled.returncode,
                "settlement_exit": final.returncode,
                "terminal_state": json.loads(path.read_text()).get("state"),
            }
        )
    return outcomes


def run_trial(
    args: argparse.Namespace,
    case: Case,
    arm: str,
    repetition: int,
    expected_binary_hash: str,
) -> dict[str, Any]:
    if digest(args.tny) != expected_binary_hash:
        raise RuntimeError("tny executable changed during evaluation; freeze it first")
    sample = args.output / f"{case.name}-r{repetition}-{arm}"
    sample.mkdir(mode=0o700)
    workspace = sample / "workspace"
    prepare(case, workspace)
    home = sample / "home"
    home.mkdir(mode=0o700)
    codex_home = sample / "codex"
    codex_home.mkdir(mode=0o700)
    # A private reference to the existing login; no token reads/copies or public storage.
    (codex_home / "auth.json").symlink_to(args.codex_home / "auth.json")
    cache = args.codex_home / "models_cache.json"
    if cache.is_file():
        shutil.copy2(cache, codex_home / "models_cache.json")
    env = {
        name: value
        for name, value in os.environ.items()
        if name
        in {
            "PATH",
            "TMPDIR",
            "LANG",
            "LC_ALL",
            "LC_CTYPE",
            "TERM",
            "USER",
            "LOGNAME",
            "SYSTEMROOT",
            "SSL_CERT_FILE",
            "SSL_CERT_DIR",
        }
    }
    env.update(HOME=str(home), CODEX_HOME=str(codex_home), NO_COLOR="1")
    # Keep a real interpreter first: mise shims resolve against the isolated HOME.
    env["PATH"] = os.pathsep.join(
        [
            str(Path(sys.executable).resolve().parent),
            str(args.codex.parent),
            "/usr/bin",
            "/bin",
            "/usr/sbin",
            "/sbin",
            env.get("PATH", ""),
        ]
    )
    prompt = (
        COMMON_PROMPT
        + f"\nUse {args.model} with {args.effort} effort for every agent; do not select a different model.\n"
    )
    if arm == "tny_swarm":
        command = [
            str(args.tny),
            "--provider",
            "codex",
            "--model",
            args.model,
            "--effort",
            args.effort,
            "--no-extensions",
            "--no-self-improve",
            "--max-steps",
            str(args.max_steps),
            f"--swarm={args.agents}",
        ]
        if args.swarm_file:
            command += ["--swarm-file", str(args.swarm_file)]
        if args.activation:
            prompt += "\n" + args.activation + "\n"
        command += ["ask", "--events=jsonl", "--progress=none", prompt]
    else:
        command = [
            str(args.codex),
            "exec",
            "--ignore-user-config",
            "--sandbox",
            "workspace-write",
            "-m",
            args.model,
            "-c",
            f'model_reasoning_effort="{args.effort}"',
            "-c",
            f'agents.default_subagent_model="{args.model}"',
            "-c",
            f'agents.default_subagent_reasoning_effort="{args.effort}"',
            "-c",
            f"agents.max_concurrent_threads_per_session={args.agents}",
            "--json",
            prompt,
        ]
    started = time.monotonic()
    timed_out = False
    with (
        (sample / "events.jsonl").open("w") as output,
        (sample / "stderr.log").open("w") as errors,
    ):
        process = subprocess.Popen(
            command,
            cwd=workspace,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=output,
            stderr=errors,
            start_new_session=True,
        )
        try:
            exit_code = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            exit_code = stop_owned_process(process)
    lead_seconds = time.monotonic() - started
    cleanup = settle_jobs(args.tny, home, env) if arm == "tny_swarm" else []
    elapsed_seconds = time.monotonic() - started
    try:
        graded = subprocess.run(
            [
                sys.executable,
                str(Path(__file__).with_name("swarm_cases.py")),
                "--grade",
                case.name,
                "--workspace",
                str(workspace),
            ],
            cwd=sample,
            text=True,
            capture_output=True,
            timeout=90,
        )
        try:
            correctness = json.loads(graded.stdout)
        except ValueError:
            correctness = {
                "passed": False,
                "error": "oracle_failed",
                "exit_code": graded.returncode,
            }
    except subprocess.TimeoutExpired:
        correctness = {"passed": False, "error": "oracle_timeout"}
    protected = {
        name: (workspace / name).is_file() and (workspace / name).read_text() == text
        for name, text in case.files.items()
        if name == "test_public.py"
    }
    metrics = (
        tny_metrics(home, sample / "events.jsonl")
        if arm == "tny_swarm"
        else codex_metrics(codex_home, sample / "events.jsonl")
    )
    completed = exit_code == 0 and not timed_out and not cleanup
    if arm == "tny_swarm":
        completed = completed and all(
            job["state"] == "succeeded" for job in metrics["jobs"]
        )
    return {
        "case": case.name,
        "complexity": case.complexity,
        "arm": arm,
        "repetition": repetition,
        "model": args.model,
        "effort": args.effort,
        "exit_code": exit_code,
        "timed_out": timed_out,
        "lead_wall_seconds": round(lead_seconds, 3),
        "end_to_end_seconds": round(elapsed_seconds, 3),
        "orchestration_completed": completed,
        "correctness": correctness,
        "protected_files_unchanged": all(protected.values()),
        "passed": completed
        and correctness.get("passed", False)
        and all(protected.values()),
        "metrics": metrics,
        "cleanup": cleanup,
        "artifacts": sample.name,
        "implementation_sha256": {
            name: digest(workspace / name)
            for name in case.files
            if (workspace / name).is_file()
        },
        "command": command[:-1] + ["<COMMON_PROMPT plus documented tny activation>"],
    }


def summarize(rows: list[dict[str, Any]]) -> dict[str, Any]:
    import statistics

    out = {}
    for case in CASES:
        out[case.name] = {}
        for arm in ("tny_swarm", "codex"):
            selected = [
                row for row in rows if row["case"] == case.name and row["arm"] == arm
            ]
            if not selected:
                continue
            out[case.name][arm] = {
                "runs": len(selected),
                "passed": sum(row["passed"] for row in selected),
                "median_seconds": statistics.median(
                    row["end_to_end_seconds"] for row in selected
                ),
                "median_input_tokens": statistics.median(
                    row["metrics"]["input_tokens"] for row in selected
                ),
                "median_output_tokens": statistics.median(
                    row["metrics"]["output_tokens"] for row in selected
                ),
                "median_cached_input_tokens": statistics.median(
                    row["metrics"]["cached_input_tokens"] for row in selected
                ),
                "usage_complete": all(
                    row["metrics"]["usage_complete"] for row in selected
                ),
                "collaborators_per_run": [
                    row["metrics"]["launched_collaborators"] for row in selected
                ],
            }
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--live", action="store_true", help="explicitly authorize live account usage"
    )
    parser.add_argument("--tny", type=Path, required=True)
    parser.add_argument(
        "--codex", type=Path, default=Path(shutil.which("codex") or "codex")
    )
    parser.add_argument(
        "--codex-home",
        type=Path,
        default=Path(os.environ.get("CODEX_HOME", str(Path.home() / ".codex"))),
    )
    parser.add_argument("--model", default="gpt-5.6-sol")
    parser.add_argument("--effort", default="medium", choices=("medium", "high"))
    parser.add_argument("--agents", type=int, default=3)
    parser.add_argument("--max-steps", type=int, default=60)
    parser.add_argument("--swarm-file", type=Path)
    parser.add_argument(
        "--activation", help="documented definition-activation instruction for tny"
    )
    parser.add_argument("--repetitions", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=360)
    parser.add_argument(
        "--cases",
        nargs="+",
        choices=[case.name for case in CASES],
        default=[case.name for case in CASES],
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.live:
        parser.error(
            "live account usage requires --live; offline tests use the oracle functions directly"
        )
    if not (
        1 <= args.repetitions <= 5
        and 30 <= args.timeout <= 900
        and 1 <= args.agents <= 16
    ):
        parser.error("repetitions 1..5, timeout 30..900 and agents 1..16 required")
    args.tny, args.codex, args.codex_home = (
        args.tny.resolve(),
        args.codex.resolve(),
        args.codex_home.resolve(),
    )
    args.output = args.output.resolve()
    if args.swarm_file:
        args.swarm_file = args.swarm_file.resolve()
    if not (args.codex_home / "auth.json").is_file():
        parser.error(
            "an existing Codex auth.json is required; this program does not log in"
        )
    args.output.mkdir(mode=0o700, parents=True, exist_ok=False)
    expected_hash = digest(args.tny)
    result = {
        "schema_version": 1,
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "model": args.model,
        "effort": args.effort,
        "timeout_seconds": args.timeout,
        "environment": {
            "platform": platform.platform(),
            "python": sys.version,
            "codex_version": subprocess.check_output(
                [str(args.codex), "--version"], text=True
            ).strip(),
            "tny_version": subprocess.check_output(
                [str(args.tny), "--version"], text=True
            ).strip(),
            "tny_sha256": expected_hash,
        },
        "definition_sha256": digest(args.swarm_file) if args.swarm_file else None,
        "activation": args.activation,
        "common_prompt": COMMON_PROMPT,
        "cases_sha256": digest(Path(__file__).with_name("swarm_cases.py")),
        "runs": [],
        "limitations": [
            "Small synthetic coding sample, not a general coding benchmark or statistical superiority claim.",
            "Identical external task specifications and model/effort; harness system prompts and tools differ.",
            "tny uses default native permission policy; Codex uses workspace-write sandbox. Tasks need no network.",
            "Trials alternate harness order, have fresh workspaces/state, and do not deliberately warm provider caches.",
            "Provider-side cache reuse is uncontrolled and reported only where usage telemetry exists.",
            "Live failures/timeouts are retained. Incomplete usage must not be treated as zero cost.",
            "Only sanitized result records are publishable; auth references and raw sessions remain private.",
        ],
    }
    atomic_json(args.output / "result.json", result)
    for case in CASES:
        if case.name not in args.cases:
            continue
        for repetition in range(1, args.repetitions + 1):
            order = ("tny_swarm", "codex") if repetition % 2 else ("codex", "tny_swarm")
            for arm in order:
                row = run_trial(args, case, arm, repetition, expected_hash)
                result["runs"].append(row)
                result["summary"] = summarize(result["runs"])
                atomic_json(args.output / "result.json", result)
                print(
                    json.dumps(
                        {
                            key: row[key]
                            for key in (
                                "case",
                                "arm",
                                "repetition",
                                "passed",
                                "end_to_end_seconds",
                            )
                        }
                    ),
                    flush=True,
                )
    result["completed_utc"] = datetime.now(timezone.utc).isoformat()
    atomic_json(args.output / "result.json", result)


if __name__ == "__main__":
    main()
