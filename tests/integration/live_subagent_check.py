#!/usr/bin/env python3
"""Opt-in Codex gpt-6-luna subagent check; stores only allowlisted evidence.

Run with --live --tny build/tny --output build/subagents-live.json. Credentials
stay in the runtime environment/auth stores; sessions and markers use a private,
throwaway HOME. This is intentionally excluded from the default test_* runner.
"""

import argparse
import hashlib
import json
import os
import re
import secrets
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path


class Failed(Exception):
    pass


SUBAGENT_FAILURE_CODES = frozenset(
    (
        "SUBAGENT_INVALID_ARGUMENT",
        "SUBAGENT_UNSUPPORTED_ACTION",
        "SUBAGENT_UNSUPPORTED_CONTEXT",
        "SUBAGENT_SESSION_NOT_FOUND",
        "SUBAGENT_SESSION_BUSY",
        "SUBAGENT_CHILD_FAILED",
        "SUBAGENT_CANCELLED",
        "SUBAGENT_AUTH_UNAVAILABLE",
        "SUBAGENT_LAUNCH_FAILED",
        "SUBAGENT_INVALID_RESPONSE",
        "SUBAGENT_ADMISSION_NESTED",
    )
)


def require(condition, reason):
    if not condition:
        raise Failed(reason)


def code_calls(doc):
    return [
        call.get("function", call)
        for message in doc.get("messages", [])
        for call in message.get("tool_calls", [])
    ]


def documents(home):
    return {
        path.parent.name: json.loads(path.read_text())
        for path in (home / ".tny" / "sessions").glob("*/*/session.json")
    }


def safe_sessions(docs):
    return [
        {
            "id": sid,
            "turns": doc.get("turns"),
            "status": doc.get("status"),
            "exit_code": doc.get("exit_code"),
            "code_calls": sum(c.get("name") == "run_code" for c in code_calls(doc)),
        }
        for sid, doc in docs.items()
        if re.fullmatch(r"[0-9a-f]{16}", sid)
    ]


def verify_cell_boundaries(wrappers, nested, evidence):
    """Bind each actual nested action to its successful execution cell."""
    expected = (None, "create", "message", "inspect", "lifecycle")
    rows = []
    evidence["cell_nested_actions"] = rows
    require(len(wrappers) == len(expected), "five-successful-cells")
    for index, ((start, end, _code), action) in enumerate(zip(wrappers, expected)):
        contained = [
            (nested_start, nested_end)
            for nested_start, nested_end in nested
            if start["sequence"] < nested_start["sequence"] < end["sequence"]
        ]
        actions = [
            json.loads(nested_start["tool_detail"]).get("action")
            for nested_start, _nested_end in contained
        ]
        rows.append(
            {
                "cell": index,
                "nested_count": len(contained),
                "actions": [a if a in expected[1:] else "unexpected" for a in actions],
            }
        )
        require(len(contained) == (0 if action is None else 1), "per-cell-nested-count")
        require(
            all(
                nested_end["sequence"] < end["sequence"]
                for _nested_start, nested_end in contained
            ),
            "nested-call-outside-cell",
        )
        require(
            actions == ([] if action is None else [action]), "per-cell-action-order"
        )


def runtime_env(home):
    # Resolve the original Codex home before replacing HOME. Never persist auth
    # in output evidence or pass token values in command arguments.
    auth_home = Path.home()
    codex_home = Path(os.environ.get("CODEX_HOME", auth_home / ".codex")).resolve()
    env = {
        k: v
        for k, v in os.environ.items()
        if not (
            k.startswith(("TNY_", "OPENAI_", "CODEX_", "CHATGPT_"))
            or k.endswith(("_API_KEY", "_BASE_URL"))
        )
    }
    env.update(
        HOME=str(home),
        XDG_CONFIG_HOME=str(home / ".config"),
        CODEX_HOME=str(codex_home),
        TNY_TOOLS="all",
    )
    return env


def child_task(label):
    code = (
        "import json, os, time; from pathlib import Path; "
        "start = time.monotonic(); time.sleep(6); "
        "nonce = Path('challenge.txt').read_text(); "
        f"Path('child-{label}.json').write_text(json.dumps("
        "{'nonce': nonce, 'pid': os.getpid(), "
        "'elapsed': time.monotonic() - start})); "
        f"print('{label.upper()}:' + nonce)"
    )
    return (
        "Do not delegate. Execute exactly this Python through run_code with "
        "timeout_ms=20000, then reply with only its printed line. Python code: " + code
    )


def run_live_parent(tny, evidence, home, workspace, env, prompt, deadline, events=True):
    require(time.monotonic() < deadline, "live-watchdog")
    args = [
        str(tny),
        "--cwd",
        str(workspace),
        "--provider",
        "codex",
        "--model",
        "gpt-6-luna",
        "--effort",
        "low",
        "--max-steps",
        "16",
        "ask",
        "--events=jsonl" if events else "--json",
        "--stdin",
    ]
    evidence["stage"] = "parent"
    started = time.monotonic()
    process = subprocess.Popen(
        args,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )
    try:
        stdout, _stderr = process.communicate(
            prompt, timeout=max(1, deadline - time.monotonic())
        )
    except subprocess.TimeoutExpired:
        process.send_signal(signal.SIGINT)
        try:
            process.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=15)
        raise Failed("parent-timeout") from None
    finally:
        evidence["elapsed_seconds"] = round(time.monotonic() - started, 3)
        evidence["sessions"] = safe_sessions(documents(home))
    evidence["parent_exit"] = process.returncode
    require(process.returncode == 0, "parent-exit")
    return stdout


def check(tny, evidence, home, workspace, env, deadline):
    nonce = secrets.token_hex(16)
    (workspace / "challenge.txt").write_text(nonce)

    create_task, follow_task = child_task("create"), child_task("follow")
    prompt = (
        "Regression verification. Complete five successful separate run_code calls. "
        "Each must omit timeout_ms completely. First run only "
        "print(tools.describe('subagent')) to discover the actual schema. "
        "Then use four separate Python cells. Avoid deeply nested parentheses: "
        "assign a named arguments dictionary, encode it in a second assignment, "
        "assign the tools.call result, then print that result. Valid short example:\n"
        "arguments = {'action': 'inspect', 'id': '0123456789abcdef'}\n"
        "arguments_json = json.dumps(arguments)\n"
        "result = tools.call('subagent', arguments_json)\n"
        "print(result)\n"
        "That id is only an example: replace it with the actual returned child id. "
        "Each cell is fresh; define every variable it needs inside that cell. "
        "Each delegated cell must perform only one subagent call. "
        "Do not read files or execute the child's code yourself. "
        "The create arguments must include action='create', provider='codex', model='gpt-6-luna', "
        "effort='low', omitting id, and prompt=" + json.dumps(create_task) + ". "
        "The message arguments must include action='message', id equal to the returned child id, "
        "and prompt=" + json.dumps(follow_task) + ". "
        "The inspect arguments must include action='inspect' and that child id. "
        "The lifecycle arguments must include action='lifecycle' and that child id. Omit prompt from the "
        "last two arguments. You may correct and retry ONLY a SyntaxError, which "
        "occurs before any Python effect. Stop on every other error; never replay "
        "a call whose execution outcome is uncertain. After all four succeed reply "
        "SUBAGENT_LIVE_OK followed by the child id."
    )
    evidence["parent_mode"] = "in-process-events"
    stdout = run_live_parent(tny, evidence, home, workspace, env, prompt, deadline)
    try:
        events = [json.loads(line) for line in stdout.splitlines() if line.strip()]
    except ValueError:
        raise Failed("parent-event-json") from None
    require(
        events
        and all(
            e.get("schema_version") == 1
            and e.get("provider") == "codex"
            and isinstance(e.get("timestamp_ms"), int)
            for e in events
        ),
        "canonical-event-envelope",
    )
    require(
        all(a["sequence"] < b["sequence"] for a, b in zip(events, events[1:])),
        "canonical-event-sequence",
    )
    terminals = [e for e in events if e.get("type") == "turn_end"]
    require(len(terminals) == 1 and events[-1] == terminals[0], "one-terminal-event")
    require(terminals[0].get("stop_reason") == 0, "parent-terminal-success")
    evidence["parent_stop_reason"] = terminals[0]["stop_reason"]
    parent_id = terminals[0].get("session_id")
    docs = documents(home)
    require(
        parent_id in docs and all(e.get("session_id") == parent_id for e in events),
        "parent-session",
    )
    parent = docs[parent_id]
    require(
        parent.get("backend") == "codex"
        and parent.get("model") == "gpt-6-luna"
        and parent.get("turns") == 1,
        "parent-provider-model-state",
    )
    evidence["parent_id"] = parent_id
    results = [
        m.get("content", "")
        for m in docs[parent_id]["messages"]
        if m.get("role") == "tool"
    ]
    evidence["subagent_failure_codes"] = sorted(
        {
            code
            for text in results
            if isinstance(text, str)
            for code in re.findall(r"\bSUBAGENT_[A-Z_]+\b", text)
            if code in SUBAGENT_FAILURE_CODES
        }
    )
    starts, completed = {}, []
    for event in events:
        if event.get("type") not in ("tool_start", "tool_end"):
            continue
        key = (event.get("tool_name"), event.get("tool_id"))
        if event["type"] == "tool_start":
            require(key not in starts, "duplicate-tool-start")
            starts[key] = event
        else:
            require(key in starts, "uncorrelated-tool-end")
            completed.append((starts.pop(key), event))
    require(not starts, "unfinished-tools")
    require(
        all(
            end.get("tool_name") in ("run_code", "subagent")
            for _start, end in completed
        ),
        "unexpected-parent-tool",
    )
    evidence["tools"] = [
        {
            "name": end.get("tool_name"),
            "status": "success" if end.get("tool_ok") is True else "error",
        }
        for _start, end in completed
        if end.get("tool_name") in ("run_code", "subagent")
    ]
    nested = [
        (start, end) for start, end in completed if end.get("tool_name") == "subagent"
    ]
    require(
        len(nested) == 4 and all(end.get("tool_ok") is True for _start, end in nested),
        "four-subagent-successes",
    )
    evidence["subagent_durations"] = []
    for (start, end), action in zip(
        nested, ("create", "message", "inspect", "lifecycle")
    ):
        require(
            json.loads(start["tool_detail"]).get("action") == action,
            "subagent-action-order",
        )
        duration = end["timestamp_ms"] - start["timestamp_ms"]
        if action in ("create", "message"):
            require(duration > 5000, "subagent-duration-" + action)
        evidence["subagent_durations"].append(
            {"action": action, "duration_ms": duration}
        )
    wrappers = [
        (start, end) for start, end in completed if end.get("tool_name") == "run_code"
    ]
    require(len(code_calls(parent)) == len(wrappers), "persisted-cell-count")
    successful, syntax_errors = [], 0
    for start, end in wrappers:
        cell = json.loads(start["tool_detail"])
        require("timeout_ms" not in cell, "omitted-cell-budgets")
        code = cell.get("code", "")
        if end.get("tool_ok") is True:
            successful.append((start, end, code))
            continue
        require("SyntaxError" in end.get("tool_detail", ""), "non-syntax-cell-failure")
        try:
            compile(code, "<live-check-cell>", "exec")
        except SyntaxError:
            syntax_errors += 1
        else:
            raise Failed("failed-cell-not-syntax")
        require(
            not any(
                start["sequence"] < nested_start["sequence"] < end["sequence"]
                for nested_start, _nested_end in nested
            ),
            "failed-cell-had-effects",
        )
    evidence["syntax_error_recoveries"] = syntax_errors
    require(len(successful) == 5, "five-successful-cells")
    verify_cell_boundaries(successful, nested, evidence)
    require(
        "tools.describe" in successful[0][2]
        and "subagent" in successful[0][2]
        and "tools.call" not in successful[0][2],
        "schema-discovery",
    )
    require(
        all(
            "tools.call" in code and "subagent" in code
            for _start, _end, code in successful[1:]
        ),
        "four-delegated-cells",
    )
    evidence.update(schema_discovery=True, omitted_cell_budgets=True)
    ids = set()
    for text in results:
        ids.update(re.findall(r"subagent ([0-9a-f]{16}) finished", text))
    require(len(ids) == 1, "one-child-id")
    child_id = ids.pop()
    require(child_id in docs and child_id != parent_id, "stored-child")
    child = docs[child_id]
    require(
        child.get("turns") == 2
        and child.get("status") == "done"
        and child.get("exit_code") == 0
        and child.get("backend") == "codex"
        and child.get("model") == "gpt-6-luna",
        "child-final-state",
    )
    require(
        sum(c.get("name") == "run_code" for c in code_calls(child)) >= 2,
        "child-actual-tool-work",
    )
    evidence["child_id"] = child_id
    evidence["markers"] = []
    for label in ("create", "follow"):
        path = workspace / f"child-{label}.json"
        require(path.is_file(), "child-marker-" + label)
        marker = json.loads(path.read_text())
        require(
            marker.get("nonce") == nonce
            and marker.get("elapsed", 0) > 5
            and isinstance(marker.get("pid"), int)
            and marker["pid"] > 1,
            "child-marker-content-" + label,
        )
        require(
            any(label.upper() + ":" + nonce in r for r in results),
            "child-result-" + label,
        )
        evidence["markers"].append(
            {
                "task": label,
                "elapsed_seconds": round(marker["elapsed"], 3),
                "pid": marker["pid"],
                "nonce_matches": True,
            }
        )
    require(
        any("turns: 2" in r and "resumable: true" in r for r in results),
        "child-inspect",
    )
    require(
        any(
            "status: done\nexit_code: 0\nrunning: false\nresumable: true" in r
            for r in results
        ),
        "child-lifecycle",
    )
    require(
        "SUBAGENT_LIVE_OK"
        in "".join(e.get("text", "") for e in events if e.get("type") == "text_delta"),
        "parent-final",
    )


def isolated_probe(tny, evidence, home, workspace, env, deadline):
    """Separately prove the normal CLI's detached parent and child runners."""
    workspace.mkdir()
    nonce = secrets.token_hex(16)
    (workspace / "challenge.txt").write_text(nonce)
    task = child_task("isolated")
    arguments = {
        "action": "create",
        "provider": "codex",
        "model": "gpt-6-luna",
        "effort": "low",
        "prompt": task,
    }
    code = (
        "arguments = " + repr(arguments) + "\narguments_json = json.dumps(arguments)\n"
    )
    code += "result = tools.call('subagent', arguments_json)\nprint(result)"
    prompt = (
        "Isolated process regression. First use one run_code cell to execute "
        "print(tools.describe('subagent')). Then use one fresh run_code cell to execute "
        "the following Python exactly, omitting timeout_ms from both cells. "
        "Do not execute the child's code yourself or call other tools. Stop on an error. "
        "After the child succeeds reply ISOLATED_PROBE_OK. Python:\n" + code
    )
    evidence["parent_mode"] = "isolated"
    stdout = run_live_parent(
        tny, evidence, home, workspace, env, prompt, deadline, events=False
    )
    payload = json.loads(stdout)
    require(
        payload.get("provider") == "codex" and payload.get("model") == "gpt-6-luna",
        "isolated-parent-provider-model",
    )
    docs = documents(home)
    parent_id = payload.get("session_id")
    require(parent_id in docs, "isolated-parent-session")
    parent = docs[parent_id]
    results = [
        m.get("content", "")
        for m in parent.get("messages", [])
        if m.get("role") == "tool"
    ]
    evidence["subagent_failure_codes"] = sorted(
        {
            code
            for text in results
            for code in re.findall(r"\bSUBAGENT_[A-Z_]+\b", text)
            if code in SUBAGENT_FAILURE_CODES
        }
    )
    records = payload.get("tool_calls", [])
    evidence["tools"] = [
        {"name": r.get("name"), "status": r.get("status")}
        for r in records
        if r.get("name") in ("run_code", "subagent")
    ]
    require(
        [(r["name"], r["status"]) for r in records if r["name"] == "subagent"]
        == [("subagent", "success")],
        "isolated-one-subagent-success",
    )
    require(
        [(r["name"], r["status"]) for r in records if r["name"] == "run_code"]
        == [("run_code", "success")] * 2,
        "isolated-two-successful-cells",
    )
    require(
        parent.get("backend") == "codex"
        and parent.get("model") == "gpt-6-luna"
        and parent.get("status") == "done"
        and parent.get("exit_code") == 0
        and parent.get("turns") == 1,
        "isolated-parent-done",
    )
    calls = code_calls(parent)
    require(
        len(calls) == 2
        and all("timeout_ms" not in json.loads(c["arguments"]) for c in calls),
        "isolated-omitted-budgets",
    )
    ids = {
        sid
        for text in results
        for sid in re.findall(r"subagent ([0-9a-f]{16}) finished", text)
    }
    require(len(ids) == 1, "isolated-one-child")
    child_id = ids.pop()
    require(child_id in docs, "isolated-child-session")
    child = docs[child_id]
    require(
        child.get("backend") == "codex"
        and child.get("model") == "gpt-6-luna"
        and child.get("status") == "done"
        and child.get("exit_code") == 0
        and child.get("turns") == 1
        and len(code_calls(child)) >= 1,
        "isolated-child-done",
    )
    marker_path = workspace / "child-isolated.json"
    require(marker_path.is_file(), "isolated-child-marker")
    marker = json.loads(marker_path.read_text())
    require(
        marker.get("nonce") == nonce
        and marker.get("elapsed", 0) > 5
        and isinstance(marker.get("pid"), int)
        and marker["pid"] > 1,
        "isolated-marker-content",
    )
    require(
        any("ISOLATED:" + nonce in text for text in results), "isolated-child-result"
    )
    require("ISOLATED_PROBE_OK" in payload.get("output", ""), "isolated-parent-final")
    evidence.update(
        success=True,
        parent_id=parent_id,
        child_id=child_id,
        omitted_cell_budgets=True,
        parent_status="done",
        child_status="done",
        marker={
            "nonce_matches": True,
            "elapsed_seconds": round(marker["elapsed"], 3),
            "pid": marker["pid"],
        },
    )


def cleanup_sessions(tny, home, workspace, env):
    """Stop only identities allocated under this check's private HOME."""
    rows = []
    for path in (home / ".tny" / "sessions").glob("*/*"):
        sid = path.name
        if not path.is_dir() or not re.fullmatch(r"[0-9a-f]{16}", sid):
            continue
        try:
            stored = json.loads((path / "session.json").read_text())
            session_workspace = Path(stored["workspace"]).resolve()
            require(
                session_workspace.is_relative_to(workspace.resolve()),
                "cleanup-workspace-scope",
            )
            result = subprocess.run(
                [
                    str(tny),
                    "--cwd",
                    str(session_workspace),
                    "session",
                    "stop",
                    sid,
                    "--kill",
                ],
                env=env,
                capture_output=True,
                timeout=15,
                check=False,
            )
            rows.append({"id": sid, "exit_code": result.returncode})
        except Exception as exc:
            rows.append({"id": sid, "failure": type(exc).__name__})
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--live", action="store_true", help="explicitly authorize live inference"
    )
    parser.add_argument("--tny", type=Path, default=Path("build/tny"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not args.live:
        parser.error("--live is required; this check uses the configured Codex account")
    evidence = {
        "success": False,
        "provider": "codex",
        "model": "gpt-6-luna",
        "effort": "low",
    }
    try:
        tny = args.tny.resolve()
        require(tny.is_file(), "binary-missing")
        evidence["binary_sha256"] = hashlib.sha256(tny.read_bytes()).hexdigest()
        tmp = Path(tempfile.mkdtemp(prefix="tny-live-subagent-"))
        home, workspace = tmp / "home", tmp / "workspace"
        home.mkdir(mode=0o700)
        workspace.mkdir()
        env = runtime_env(home)
        check_passed = False
        try:
            deadline = time.monotonic() + 240
            evidence["isolated_probe"] = {"success": False}
            isolated_probe(
                tny,
                evidence["isolated_probe"],
                home,
                workspace / "isolated-probe",
                env,
                deadline,
            )
            check(tny, evidence, home, workspace, env, deadline)
            check_passed = True
        finally:
            # Evidence is captured before cleanup changes persisted status.
            try:
                evidence["sessions"] = safe_sessions(documents(home))
            finally:
                rows = cleanup_sessions(tny, home, workspace, env)
                evidence["cleanup"] = rows
                clean = all(row.get("exit_code") == 0 for row in rows)
                evidence["cleanup_success"] = clean
                if clean and check_passed:
                    shutil.rmtree(tmp)
                else:
                    # Preserve failed-check diagnostics after scoped cleanup;
                    # active state also remains if cleanup could not finish.
                    evidence["scratch_retained"] = str(tmp)
                if not clean:
                    raise Failed("session-cleanup")
        evidence.update(success=True, stage="complete")
    except Failed as exc:
        evidence["failure"] = str(exc)  # Only static check names, never provider data.
    except Exception as exc:
        evidence["failure"] = type(exc).__name__
    text = json.dumps(evidence, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    print(text, end="")
    return 0 if evidence["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
