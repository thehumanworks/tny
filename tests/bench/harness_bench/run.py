#!/usr/bin/env python3
"""Run isolated, repeatable harness tasks through the recording proxy."""

import argparse
import gzip
import json
import shutil
import statistics
import subprocess
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

from adapters import ADAPTERS, final_message, invocation
from cost import request_cost
from proxy import RecordingProxy, static_parts

HERE = Path(__file__).resolve().parent
DEFAULT_OUT = Path("/home/tomas/.cache/tny-opt/runs/bench")


def token_count(value):
    try:
        import tiktoken  # noqa: PLC0415
    except ImportError:
        return round(len(value) / 4), "chars/4"
    return len(tiktoken.get_encoding("o200k_base").encode(value)), "o200k_base"


def _git_init(workspace):
    for command in (
        ["git", "init", "-q"],
        ["git", "add", "-A"],
        [
            "git",
            "-c",
            "user.name=Benchmark",
            "-c",
            "user.email=benchmark@localhost",
            "commit",
            "-qm",
            "initial fixture",
            "--allow-empty",
        ],
    ):
        subprocess.run(command, cwd=workspace, check=True, capture_output=True)


def _summarize(rows, run_dir, model):
    usage_fields = (
        "input_tokens",
        "cached_input_tokens",
        "cache_write_tokens",
        "output_tokens",
        "reasoning_tokens",
    )
    totals = {
        key: sum(row[key] for row in rows)
        if rows and all(row.get(key) is not None for row in rows)
        else None
        for key in usage_fields
    }
    costs = [request_cost(row, model) for row in rows]
    totals["ite"] = (
        sum(cost[0] for cost in costs)
        if costs and all(cost[0] is not None for cost in costs)
        else None
    )
    totals["usd"] = (
        sum(cost[1] for cost in costs)
        if costs and all(cost[1] is not None for cost in costs)
        else None
    )
    totals["uncached_input_tokens"] = (
        sum(cost[2] for cost in costs)
        if costs and all(cost[2] is not None for cost in costs)
        else None
    )
    first = rows[0] if rows else None
    if first:
        sections = first["sections"]
        totals["static_prefix_chars"] = (
            sections["instructions_chars"] + sections["tools_chars"]
        )
        raw = gzip.decompress((run_dir / "proxy" / first["body_file"]).read_bytes())
        body = json.loads(raw)
        instruction_parts, tool_parts = static_parts(body)
        instructions, method = token_count(
            "".join(
                part if isinstance(part, str) else json.dumps(part, ensure_ascii=False)
                for part in instruction_parts
            )
        )
        tools, _ = token_count(
            json.dumps(tool_parts, ensure_ascii=False, separators=(",", ":"))
        )
        totals["static_prefix_tokens"] = instructions + tools
        totals["static_prefix_token_method"] = method
    else:
        totals["static_prefix_chars"] = None
        totals["static_prefix_tokens"] = None
        totals["static_prefix_token_method"] = None
    contexts = [
        row["input_tokens"] for row in rows if row.get("input_tokens") is not None
    ]
    totals["peak_input_tokens_per_request"] = max(contexts, default=None)
    totals["mean_input_tokens_per_request"] = (
        statistics.mean(contexts) if contexts else None
    )
    totals["total_tool_output_chars"] = sum(
        sum(row["sections"]["tool_output_chars"]) for row in rows
    )
    calls = Counter(
        item.get("name") or item["type"]
        for row in rows
        for item in row.get("output_items", [])
    )
    totals["tool_calls_by_name"] = dict(sorted(calls.items()))
    totals["tool_calls"] = sum(calls.values())
    totals["http_error_count"] = sum(
        (row.get("http_status") or 0) >= 400 for row in rows
    )
    return totals


def _wire_settings_valid(rows, run_dir, model, effort):
    if not rows:
        return False
    for row in rows:
        raw = gzip.decompress((run_dir / "proxy" / row["body_file"]).read_bytes())
        body = json.loads(raw)
        if (
            body.get("model") != model
            or (body.get("reasoning") or {}).get("effort") != effort
        ):
            return False
    return True


def run_one(args, task_dir, harness, rep):
    task = json.loads((task_dir / "task.json").read_text())
    run_dir = args.out / args.label / harness / task["id"] / f"rep-{rep:02d}"
    result_file = run_dir / "result.json"
    if result_file.exists():
        return json.loads(result_file.read_text())
    run_dir.mkdir(parents=True, exist_ok=True)
    workspace = run_dir / "workspace"
    if workspace.exists():
        shutil.rmtree(workspace)
    shutil.copytree(task_dir / "repo", workspace)
    _git_init(workspace)
    setup = task_dir / "setup.sh"
    if setup.exists():
        subprocess.run(
            ["bash", str(setup)],
            cwd=workspace,
            check=True,
            stdout=(run_dir / "setup.stdout").open("w"),
            stderr=(run_dir / "setup.stderr").open("w"),
            timeout=task["timeout_s"],
        )
    started = time.monotonic()
    exit_code = None
    timed_out = False
    adapter_error = None
    stdout = ""
    stderr = ""
    with RecordingProxy(run_dir / "proxy", args.auth_file) as proxy:
        try:
            call = invocation(
                harness,
                run_dir,
                proxy.base_url,
                task["prompt"],
                args.model,
                args.effort,
                args.tny_bin,
            )
            completed = subprocess.run(
                call.command,
                cwd=workspace,
                env=call.env,
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=True,
                timeout=task["timeout_s"],
                errors="replace",
            )
            exit_code, stdout, stderr = (
                completed.returncode,
                completed.stdout,
                completed.stderr,
            )
        except subprocess.TimeoutExpired as error:
            timed_out = True
            stdout = (
                (error.stdout or b"").decode(errors="replace")
                if isinstance(error.stdout, bytes)
                else (error.stdout or "")
            )
            stderr = (
                (error.stderr or b"").decode(errors="replace")
                if isinstance(error.stderr, bytes)
                else (error.stderr or "")
            )
        except (OSError, RuntimeError, ValueError) as error:
            adapter_error = f"{type(error).__name__}: {error}"
        wall_s = round(time.monotonic() - started, 3)
        # A CLI can exit just before the handler writes its final accounting row.
        time.sleep(0.2)
        rows = proxy.rows
    (run_dir / "stdout.txt").write_text(stdout)
    (run_dir / "stderr.txt").write_text(stderr)
    message = final_message(harness, stdout)
    message_file = run_dir / "final_message.txt"
    message_file.write_text(message)
    verify = subprocess.run(
        ["bash", str(task_dir / "verify.sh"), str(workspace), str(message_file)],
        cwd=task_dir,
        capture_output=True,
        text=True,
        timeout=30,
    )
    reason = (
        verify.stdout.strip().splitlines() or verify.stderr.strip().splitlines() or [""]
    )[0]
    passed = verify.returncode == 0 and exit_code == 0 and not timed_out
    measurement_valid = bool(rows) and all(
        row.get(key) is not None
        for row in rows
        for key in (
            "input_tokens",
            "cached_input_tokens",
            "output_tokens",
            "reasoning_tokens",
        )
    )
    model_effort_valid = _wire_settings_valid(rows, run_dir, args.model, args.effort)
    if adapter_error:
        reason = adapter_error
    elif timed_out:
        reason = "task timed out"
    elif exit_code != 0:
        reason = f"harness exit {exit_code}: {reason}"
    elif not rows:
        passed = False
        reason = "no proxy requests"
    elif not measurement_valid:
        passed = False
        reason = "provider usage missing from a recorded request"
    elif not model_effort_valid:
        passed = False
        reason = "wire model or reasoning effort differs from requested setting"
    result = {
        "schema_version": 1,
        "label": args.label,
        "harness": harness,
        "task": task["id"],
        "category": task["category"],
        "tags": task.get("tags", []),
        "rep": rep,
        "model": args.model,
        "effort": args.effort,
        "pass": passed,
        "measurement_valid": measurement_valid,
        "model_effort_valid": model_effort_valid,
        "reason": reason,
        "wall_s": wall_s,
        "exit_code": exit_code,
        "timeout": timed_out,
        "requests": len(rows),
        "request_rows": rows,
        **_summarize(rows, run_dir, args.model),
    }
    temporary = result_file.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(result, indent=2) + "\n")
    temporary.replace(result_file)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", action="append", choices=ADAPTERS)
    parser.add_argument("--task", action="append", default=[])
    parser.add_argument("--reps", type=int, default=3)
    parser.add_argument("--model", default="gpt-5.6-luna")
    parser.add_argument("--effort", default="low")
    parser.add_argument("--concurrency", type=int, default=3)
    parser.add_argument("--tny-bin", default="build/tny")
    parser.add_argument("--label", default="baseline")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument(
        "--auth-file", type=Path, default=Path.home() / ".codex" / "auth.json"
    )
    args = parser.parse_args()
    if (
        args.reps < 1
        or args.concurrency < 1
        or "/" in args.label
        or args.label in {".", ".."}
    ):
        parser.error(
            "reps/concurrency must be positive; label must be one path component"
        )
    args.out = args.out.resolve()
    tasks = {
        path.name: path
        for path in (HERE / "tasks").iterdir()
        if path.is_dir() and (path / "task.json").exists()
    }
    selected = list(tasks) if not args.task or "all" in args.task else args.task
    for task in selected:
        if task not in tasks:
            parser.error(f"unknown task: {task}")
    jobs = [
        (tasks[task], harness, rep)
        for task in selected
        for harness in (args.harness or ["tny", "codex"])
        for rep in range(1, args.reps + 1)
    ]
    errors = 0
    with ThreadPoolExecutor(max_workers=args.concurrency) as executor:
        futures = {executor.submit(run_one, args, *job): job for job in jobs}
        for future in as_completed(futures):
            task, harness, rep = futures[future]
            try:
                row = future.result()
                print(
                    json.dumps(
                        {
                            "harness": harness,
                            "task": task.name,
                            "rep": rep,
                            "pass": row["pass"],
                            "reason": row["reason"],
                            "requests": row["requests"],
                        }
                    ),
                    flush=True,
                )
            except Exception as error:
                errors += 1
                print(
                    json.dumps(
                        {
                            "harness": harness,
                            "task": task.name,
                            "rep": rep,
                            "error": f"{type(error).__name__}: {error}",
                        }
                    ),
                    flush=True,
                )
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
