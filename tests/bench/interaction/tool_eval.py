#!/usr/bin/env python3
"""Controlled model-generated tool workflow evaluation; explicit live opt-in.

Reuses six published task families, three hidden fixtures, the production
CPython cell and the exact same C virtual capability host across all four arms.
This is a representation/adapter study, not a new production tool registry.
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
import signal
import statistics
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent / "code_mode"))
import cases
from execute import score
from references import reference

ARMS = ("python", "shell", "javascript", "monty")
TASKS = (
    "join",
    "pagination",
    "batch_edit",
    "bounded_retry",
    "unicode",
    "dependency_order",
)
COMMON = """Return only a JSON object {"code":"..."}, with the complete program as a string.
An external harness will execute it; do not execute any tools while generating.
Input values are unknown until execution. Read virtual inputs through the provided
tools. Never hardcode answers. The five tool schemas below are complete and authoritative.
Every tool call takes a name string and a JSON OBJECT ENCODED AS A STRING; results
are strings. Parse JSON results when needed. fs.read returns raw text. Validate
fs.write returns {ok:true}. Write the requested answer to the exact relative path
output.json. Write only requested files, preserving JSON types.
After successful writes print exactly done and a newline, with no other stdout.
At most 64 tool calls, 64KiB output, 256KiB code and 2 seconds execution.
Ordinary host access/network is available in host modes; no network is needed
for these tasks and virtual input files exist only behind tools. Do not inspect
adapter/harness files or use other inputs. No state persists between executions.
"""
HELP = {
    "python": "Write Python 3.14 for the embedded CPython runtime. json, tools and print are prebound. tools.call(name, arguments_json), tools.list(), tools.describe(name) return strings (describe may return None). Standard Python and imports work.",
    "monty": "Write Python 3.14 for Monty 1.4.0. json, tools and print are prebound. tools.call(name, arguments_json), tools.list(), tools.describe(name) return strings (describe may return None). Host operations go through these functions; no ambient filesystem, environment or network exists in Monty. Use ordinary supported Python collection/string code.",
    "javascript": "Write JavaScript for host Node. tools and print are prebound; print and console.log append one newline, so use print('done'). tools.call(name, arguments_json), tools.list(), tools.describe(name) are synchronous and return strings (describe may return null). JSON.parse/JSON.stringify are available. Normal Node APIs work; this is CommonJS, no top-level await.",
    "shell": "Write a POSIX /bin/sh script. You may invoke host python3 or node and write temporary scripts. The command tny-tool call NAME JSON_ARGUMENT_STRING returns raw tool output on stdout; tny-tool list and tny-tool describe NAME are also available. In Python subprocesses, `from tool_eval_adapter import tools` provides the same synchronous tools.call(name, arguments_json), tools.list(), tools.describe(name) string API. Import json normally. Host node/python3 are on PATH; do not assume jq or third-party packages. Preserve shell quoting and propagate failures.",
}


def prompt(arm, task):
    return (
        COMMON
        + "\n"
        + HELP[arm]
        + "\nTool schemas:\n"
        + cases.dump(cases.CATALOG)
        + "\nTask:\n"
        + dict(cases.TASKS)[task]
    )


def wrapped_command(command):
    # Native nested calls stay on the owning cell's main thread, never a Python
    # thread. The short socket poll services child completion and the deadline.
    return """import os, socket, subprocess, json
server = socket.socket(socket.AF_UNIX)
server.bind(os.environ['TNY_EVAL_SOCKET'])
server.listen(8)
server.settimeout(0.01)
child = subprocess.Popen(COMMAND)
try:
    while True:
        try:
            connection, _ = server.accept()
        except TimeoutError:
            if child.poll() is not None: break
            continue
        with connection:
            stream = connection.makefile('rwb')
            request = json.loads(stream.readline(262144))
            try:
                operation = request[0]
                if operation not in ('call', 'list', 'describe'):
                    raise ValueError('unknown operation')
                value = getattr(tools, operation)(*request[1:])
                reply = {'result': value}
            except Exception as error:
                reply = {'error': str(error)}
            stream.write((json.dumps(reply)+'\\n').encode())
            stream.flush()
    if child.returncode != 0: raise RuntimeError('host program failed: '+str(child.returncode))
finally:
    server.close()
    if child.poll() is None:
        child.kill()
        child.wait()
""".replace("COMMAND", repr(command))


def check(arm, task, code, binary, monty_python):
    variants = []
    for variant in range(3):
        case = cases.fixture(task, variant)
        with tempfile.TemporaryDirectory(prefix="tny-tool-eval-", dir="/tmp") as temp:
            work = Path(temp)
            env = {
                "PATH": os.environ["PATH"],
                "HOME": temp,
                "TMPDIR": temp,
                "PYTHONPATH": str(HERE),
                "TNY_EVAL_SOCKET": str(work / "rpc"),
            }
            actual = code
            if arm != "python":
                (work / "source").write_text(code)
                if arm == "shell":
                    client = work / "tny-tool"
                    client.write_text(
                        "#!/bin/sh\nexec "
                        + __import__("shlex").quote(sys.executable)
                        + " "
                        + __import__("shlex").quote(str(HERE / "tool_eval_adapter.py"))
                        + ' "$@"\n'
                    )
                    client.chmod(0o700)
                    env["PATH"] = str(work) + ":" + env["PATH"]
                    command = ["/bin/sh", str(work / "source")]
                elif arm == "javascript":
                    prelude = """const {execFileSync}=require('node:child_process');
const rpc=(...args)=>execFileSync(PYTHON,[ADAPTER,...args],{encoding:'utf8'});
const tools={call:(n,a)=>rpc('call',n,a),list:()=>rpc('list'),describe:n=>rpc('describe',n)||null};
const print=(...v)=>console.log(...v);
""".replace("PYTHON", json.dumps(sys.executable)).replace(
                        "ADAPTER", json.dumps(str(HERE / "tool_eval_adapter.py"))
                    )
                    (work / "source.js").write_text(prelude + code)
                    command = ["node", str(work / "source.js")]
                else:
                    command = [
                        monty_python,
                        str(HERE / "tool_eval_adapter.py"),
                        "monty",
                        str(work / "source"),
                    ]
                actual = wrapped_command(command)
            payload = cases.dump(
                {
                    "code": actual,
                    "catalog": cases.dump(cases.CATALOG),
                    "fixture": case["runtime"],
                }
            )
            started = time.perf_counter()
            process = subprocess.Popen(
                [binary],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                cwd=temp,
                env=env,
                start_new_session=True,
            )
            try:
                stdout, stderr = process.communicate(payload.encode(), timeout=5)
                observed = (
                    json.loads(stdout)
                    if process.returncode == 0
                    else {
                        "runtime_ok": False,
                        "stdout": "executor failed",
                        "calls": [],
                        "writes": {},
                        "invalid_call": False,
                    }
                )
            except (subprocess.TimeoutExpired, ValueError):
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.communicate()
                observed = {
                    "runtime_ok": False,
                    "stdout": "timeout or invalid result",
                    "calls": [],
                    "writes": {},
                    "invalid_call": False,
                }
            verdict = score(observed, case)
            if task == "bounded_retry":
                checks = case["runtime"]["checks"]
                counts = {key: 0 for key in checks}
                for call in observed.get("calls", []):
                    if call["name"] == "api.check":
                        key = call["args"].get("id")
                        if key in counts:
                            counts[key] += 1
                verdict["retry_bound_ok"] = all(
                    counts[key] == len(responses) for key, responses in checks.items()
                )
                verdict["passed"] = verdict["passed"] and verdict["retry_bound_ok"]
            variants.append(
                {
                    "variant": variant,
                    **verdict,
                    "observed": observed,
                    "wall_ms": (time.perf_counter() - started) * 1000,
                }
            )
    return {"passed": all(v["passed"] for v in variants), "variants": variants}


def controls(binary, monty_python):
    rows = []
    for task in TASKS:
        for arm in ARMS:
            code = reference("javascript" if arm == "javascript" else "python", task)
            if arm == "shell":
                code = (
                    "python3 - <<'PYTHON'\nimport json\nfrom tool_eval_adapter import tools\n"
                    + code
                    + "\nPYTHON\n"
                )
            result = check(arm, task, code, binary, monty_python)
            rows.append({"arm": arm, "task": task, **result})
            print("control", arm, task, result["passed"], flush=True)
    # Negative controls must fail even when the process exits successfully.
    for arm, task, code in [
        ("python", "join", 'print("done")'),
        ("python", "join", 'tools.call("fs.read", "[]")\nprint("done")'),
    ]:
        assert not check(arm, task, code, binary, monty_python)["passed"]
    return rows


def generate(token, request, model):
    conn = http.client.HTTPSConnection("api.openai.com", timeout=90)
    payload = {
        "model": model,
        "store": False,
        "stream": True,
        "reasoning": {"effort": "low"},
        "input": [{"role": "user", "content": request}],
    }
    start = time.perf_counter()
    try:
        conn.request(
            "POST",
            "/v1/responses",
            json.dumps(payload),
            {"Authorization": "Bearer " + token, "Content-Type": "application/json"},
        )
        response = conn.getresponse()
        if response.status != 200:
            return {"ok": False, "http_status": response.status}
        text = []
        usage = None
        for line in response:
            if not line.startswith(b"data: ") or line.strip() == b"data: [DONE]":
                continue
            event = json.loads(line[6:])
            kind = event.get("type")
            if kind == "response.output_text.delta":
                text.append(event["delta"])
            if kind == "response.completed":
                usage = event["response"].get("usage")
                break
            if kind in ("error", "response.failed", "response.incomplete"):
                return {"ok": False, "error_type": kind}
        raw = "".join(text).strip()
        if raw.startswith("```"):
            raw = "\n".join(raw.splitlines()[1:-1])
        try:
            code = json.loads(raw)["code"]
        except (ValueError, KeyError, TypeError):
            return {"ok": False, "format_error": True, "usage": usage, "raw": raw}
        return {
            "ok": isinstance(code, str),
            "code": code,
            "usage": usage,
            "wall_ms": (time.perf_counter() - start) * 1000,
        }
    except Exception as error:
        return {"ok": False, "error_class": type(error).__name__}
    finally:
        conn.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--binary", type=Path, default=ROOT / "build/python-runtime-bench/production"
    )
    p.add_argument("--monty-python", default="/tmp/tny-experiments/bin/python")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--live", action="store_true")
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--model", default="gpt-6.1-sol")
    args = p.parse_args()
    binary = str(args.binary.resolve())
    args.output.mkdir(parents=True, exist_ok=False)
    reference_rows = controls(binary, args.monty_python)
    (args.output / "controls.json").write_text(
        json.dumps(reference_rows, indent=2) + "\n"
    )
    if not all(row["passed"] for row in reference_rows):
        raise SystemExit("Reference controls failed; no inference performed")
    if not args.live:
        return
    auth = json.loads((Path.home() / ".tny/openai-auth.json").read_text())
    if not auth.get("plan_usage"):
        raise SystemExit("SIWC plan consent required")
    order = [
        (ARMS[(i + rep + off) % 4], task, rep)
        for rep in range(args.repeats)
        for i, task in enumerate(TASKS)
        for off in range(4)
    ]
    manifest = {
        "model": args.model,
        "effort": "low",
        "repeats": args.repeats,
        "variants": 3,
        "arms": ARMS,
        "tasks": TASKS,
        "order": order,
        "source_revision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "source_sha256": {
            f.name: hashlib.sha256(f.read_bytes()).hexdigest()
            for f in [Path(__file__), HERE / "tool_eval_adapter.py"]
        },
        "auth": "SIWC saved login; public Responses HTTP; no credential logging",
        "max_repairs": 1,
        "comparison": "production CPython cell and same C virtual tools; experimental host adapters; synthetic published task suite",
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    rows = []
    # Only independent inference calls overlap; execution/state remains serial.
    with ThreadPoolExecutor(max_workers=2) as pool:
        pending = [
            pool.submit(generate, auth["access_token"], prompt(arm, task), args.model)
            for arm, task, _ in order
        ]
        for (arm, task, rep), future in zip(order, pending, strict=True):
            generation = future.result()
            attempts = []
            for attempt in range(2):
                evaluation = (
                    check(arm, task, generation["code"], binary, args.monty_python)
                    if generation.get("ok")
                    else {"passed": False, "variants": []}
                )
                attempts.append({"generation": generation, "evaluation": evaluation})
                if evaluation["passed"] or not generation.get("ok") or attempt == 1:
                    break
                failures = [
                    {
                        "variant": v["variant"],
                        "execution_ok": v["execution_ok"],
                        "output_ok": v["output_ok"],
                        "trace_ok": v["trace_ok"],
                        "observed": v["observed"],
                    }
                    for v in evaluation["variants"]
                    if not v["passed"]
                ]
                repair = (
                    prompt(arm, task)
                    + "\nRepair this program once using observed failures; expected answers are not supplied.\nProgram:\n"
                    + generation["code"]
                    + "\nObserved:\n"
                    + json.dumps(failures)[:6000]
                )
                generation = generate(auth["access_token"], repair, args.model)
            row = {
                "arm": arm,
                "task": task,
                "repetition": rep,
                "attempts": attempts,
                "first_pass": attempts[0]["evaluation"]["passed"],
                "passed": attempts[-1]["evaluation"]["passed"],
            }
            rows.append(row)
            (args.output / "samples.json").write_text(json.dumps(rows, indent=2) + "\n")
            print(
                len(rows),
                len(order),
                arm,
                task,
                rep,
                "first",
                row["first_pass"],
                "final",
                row["passed"],
                flush=True,
            )
    summary = {}
    for arm in ARMS:
        samples = [r for r in rows if r["arm"] == arm]
        generations = [a["generation"] for r in samples for a in r["attempts"]]
        input_tokens = sum(
            (g.get("usage") or {}).get("input_tokens", 0) for g in generations
        )
        output_tokens = sum(
            (g.get("usage") or {}).get("output_tokens", 0) for g in generations
        )
        successes = sum(r["passed"] for r in samples)
        summary[arm] = {
            "samples": len(samples),
            "first_pass": sum(r["first_pass"] for r in samples),
            "final_pass": successes,
            "requests": len(generations),
            "usage_missing": sum(not g.get("usage") for g in generations),
            "input_tokens": input_tokens,
            "output_tokens": output_tokens,
            "total_tokens_per_success": (input_tokens + output_tokens) / successes
            if successes
            else None,
            "median_first_output_tokens": statistics.median(
                (r["attempts"][0]["generation"].get("usage") or {}).get(
                    "output_tokens", 0
                )
                for r in samples
            ),
            "invalid_call_variants": sum(
                v["observed"].get("invalid_call", False)
                for r in samples
                for a in r["attempts"]
                for v in a["evaluation"]["variants"]
            ),
        }
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
