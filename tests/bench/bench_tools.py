#!/usr/bin/env python3
"""bench_tools.py — three-arm A/B of the native tool profiles (ADR 0062) on a
frozen task set, for the shell-first decision in ADR 0057 (issue #103).

Arms are the `tools` profiles, one per run via TNY_TOOLS:

    all             the complete structured schema (today's default)
    terminal+edit   terminal, edit_file, read_image (+ ask_user_question)
    terminal        terminal, read_image only

Every run copies a fixture repo from tests/bench/fixtures/tools/<task>/ into a
fresh scratch directory, feeds `task.md` to `tny ask -B --json --stdin`, waits
with `tny session ID --wait --timeout --json`, and scores the scratch with the
fixture's `check.sh` (exit 0 = pass). The session document carries the whole
transcript, so the harness measures steps, tool calls, token usage, repair
loops (a tool call issued after a nonzero-exit tool result) and edit-method
drift (`sed -i`/heredoc writes versus `tny edit`/`edit_file`) without a second
provider call.

Usage:
  bench_tools.py --dry-run
  bench_tools.py --mock --tasks fix-py-sum-range
  bench_tools.py --provider aiproxy --effort high --runs 1
  bench_tools.py --verify-fixtures        # no provider: check.sh red -> green

Output: a markdown report on stdout and one JSON document under --out.

`--mock` needs no key: it starts tests/integration/mock_openai.py with a
scripted single-`terminal` trajectory that runs the fixture's own
solution.sh, so CI can smoke the whole pipeline (see docs/ci.md).
"""

import argparse
import json
import os
import re
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIXTURES = os.path.join(ROOT, "tests", "bench", "fixtures", "tools")
MOCK = os.path.join(ROOT, "tests", "integration", "mock_openai.py")
ARMS = ("all", "terminal+edit", "terminal")
# fixture bookkeeping never copied into the workspace the model sees
FIXTURE_META = ("task.md", "check.sh", "check.py", "family", "solution.sh")

# a terminal command that mutates a file without an exact-match editor
SHELL_WRITE = re.compile(
    r"""(?x)
    \bsed\s+-i\b
  | \bperl\s+-[a-zA-Z]*i
  | \bex\s+-s\b
  | <<-?\s*['"]?\w*EOF
  | \b(?:cat|tee|printf|echo)\b[^|;&]*>{1,2}\s*\S
  | \bpython3?\s+-c\b[^;|&]*\bopen\(
  | \bawk\b[^|;&]*>{1,2}\s*\S
""",
)
# `tny edit FILE`, but not the `tny edit --help` probe a model may make first
TNY_EDIT = re.compile(r"\btny\b[^|;&]*\bedit\b(?![^|;&]*(?:--help|\s-h\b))")
STRUCTURED_EDIT = {"edit_file", "write_file", "apply_patch", "create_file"}
# the shell-profile terminal result opens `exit: N`; `all` opens `exit code: N`
EXIT_LINE = re.compile(r"^exit(?: code)?:\s*(-?\d+)", re.M)


# ------------------------------------------------------------------- fixtures
def discover(names=None, families=None):
    tasks = []
    for name in sorted(os.listdir(FIXTURES)):
        d = os.path.join(FIXTURES, name)
        if not os.path.isdir(d) or not os.path.exists(os.path.join(d, "task.md")):
            continue
        check = None
        for cand in ("check.sh", "check.py"):
            if os.path.exists(os.path.join(d, cand)):
                check = cand
                break
        if check is None:
            raise SystemExit(f"fixture {name} has no check.sh or check.py")
        fam = "unknown"
        fp = os.path.join(d, "family")
        if os.path.exists(fp):
            fam = open(fp).read().strip()
        if names and name not in names:
            continue
        if families and fam not in families:
            continue
        tasks.append({"name": name, "dir": d, "check": check, "family": fam})
    if names:
        missing = sorted(set(names) - {t["name"] for t in tasks})
        if missing:
            raise SystemExit("unknown task(s): " + ", ".join(missing))
    return tasks


def scratch_copy(task, dest):
    for entry in sorted(os.listdir(task["dir"])):
        if entry in FIXTURE_META:
            continue
        src = os.path.join(task["dir"], entry)
        dst = os.path.join(dest, entry)
        if os.path.isdir(src):
            shutil.copytree(src, dst)
        else:
            shutil.copy2(src, dst)


def run_check(task, scratch, timeout=180):
    check = os.path.join(task["dir"], task["check"])
    argv = [sys.executable, check] if task["check"].endswith(".py") else ["sh", check]
    env = {k: v for k, v in os.environ.items() if not k.startswith(("TNY_", "MOCK_"))}
    try:
        p = subprocess.run(
            argv,
            cwd=scratch,
            env=env,
            capture_output=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return False, "check timed out"
    out = (p.stdout + p.stderr).decode("utf-8", "replace").strip()
    return p.returncode == 0, out[-400:]


# ---------------------------------------------------------------- mock server
def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def start_mock(task, effort):
    """One scripted terminal call that runs the fixture's reference solution."""
    port = free_port()
    solution = os.path.join(task["dir"], "solution.sh")
    if not os.path.exists(solution):
        raise SystemExit(f"--mock needs {solution}")
    env = dict(
        os.environ,
        MOCK_EXPECT_WIRE="responses",
        MOCK_CUSTOM_TOOL="terminal",
        MOCK_CUSTOM_ARGUMENTS=json.dumps({"command": "sh " + solution}),
    )
    if effort:
        env["MOCK_EXPECT_EFFORT"] = effort
    else:
        env.pop("MOCK_EXPECT_EFFORT", None)
    for k in ("MOCK_EXPECT_TOOL_NAMES", "MOCK_EXPECT_INSTRUCTIONS"):
        env.pop(k, None)
    proc = subprocess.Popen(
        [sys.executable, MOCK, str(port)],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    line = proc.stdout.readline().decode()
    if "ready" not in line:
        proc.kill()
        raise SystemExit(f"mock did not start: {line!r}")
    return proc, port


# ------------------------------------------------------------------- measuring
def classify(messages):
    """Walk the transcript once for tool names, repair loops and edit method.

    A repair loop is a tool call issued straight after a nonzero-exit result.
    In the shell arms a plain `grep` miss or a missing `rg` also exits nonzero,
    so `failures` keeps the command and the head of its output: the ADR reads
    the two apart rather than treating every nonzero exit as a mistake."""
    m = {
        "tool_calls": 0,
        "terminal_calls": 0,
        "repair_loops": 0,
        "edit_tny_edit": 0,
        "edit_structured": 0,
        "edit_shell_write": 0,
        "tool_names": {},
        "failed_results": 0,
        "failures": [],
    }
    commands = {}
    prev_failed = False
    for msg in messages:
        role = msg.get("role")
        if role == "assistant" and msg.get("tool_calls"):
            for call in msg["tool_calls"]:
                fn = call.get("function") or {}
                name = fn.get("name") or call.get("name") or "?"
                args = fn.get("arguments") or ""
                m["tool_calls"] += 1
                m["tool_names"][name] = m["tool_names"].get(name, 0) + 1
                if prev_failed:
                    m["repair_loops"] += 1
                try:
                    command = json.loads(args).get("command", "")
                except (ValueError, AttributeError):
                    command = args
                commands[call.get("id")] = command or name
                if name in STRUCTURED_EDIT:
                    m["edit_structured"] += 1
                elif name == "terminal":
                    m["terminal_calls"] += 1
                    if TNY_EDIT.search(command):
                        m["edit_tny_edit"] += 1
                    elif SHELL_WRITE.search(command):
                        m["edit_shell_write"] += 1
            prev_failed = False
        elif role == "tool":
            content = msg.get("content") or ""
            hit = EXIT_LINE.search(content)
            prev_failed = bool(
                (hit and hit.group(1) != "0") or content.startswith("error:")
            )
            if prev_failed:
                m["failed_results"] += 1
                if len(m["failures"]) < 8:
                    m["failures"].append(
                        {
                            "exit": hit.group(1) if hit else "error",
                            "command": (commands.get(msg.get("tool_call_id")) or "?")[
                                :160
                            ],
                            "output": " ".join(content.split())[:160],
                        }
                    )
    return m


def one_run(a, task, arm, home):
    scratch = tempfile.mkdtemp(prefix="tnybench.ws.")
    mock = None
    rec = {
        "task": task["name"],
        "family": task["family"],
        "arm": arm,
        "pass": False,
        "error": None,
    }
    try:
        scratch_copy(task, scratch)
        env = dict(os.environ, HOME=home, TNY_TOOLS=arm)
        env["PATH"] = os.path.join(home, "bin") + os.pathsep + env.get("PATH", "")
        env.pop("TNY_ISOLATE", None)
        provider = a.provider
        if a.mock:
            mock, port = start_mock(task, a.effort)
            provider = "openai"
            env["OPENAI_BASE_URL"] = f"http://127.0.0.1:{port}/v1"
            env["OPENAI_API_KEY"] = "mock-key"
        argv = [a.tny, "--cwd", scratch, "--provider", provider]
        if a.model:
            argv += ["--model", a.model]
        if a.effort:
            argv += ["--effort", a.effort]
        argv += ["--max-steps", str(a.max_steps), "ask", "-B", "--json", "--stdin"]
        prompt = open(os.path.join(task["dir"], "task.md")).read()
        t0 = time.time()
        launch = subprocess.run(
            argv, input=prompt.encode(), capture_output=True, env=env, timeout=120
        )
        if launch.returncode != 0:
            rec["error"] = "launch: " + launch.stderr.decode("utf-8", "replace")[-300:]
            return rec
        sid = json.loads(launch.stdout)["session_id"]
        rec["session_id"] = sid
        wait = subprocess.run(
            [
                a.tny,
                "--cwd",
                scratch,
                "session",
                sid,
                "--wait",
                "--timeout",
                str(a.timeout),
                "--json",
            ],
            capture_output=True,
            env=env,
            timeout=a.timeout + 60,
        )
        rec["wall_s"] = round(time.time() - t0, 2)
        rec["wait_exit"] = wait.returncode
        try:
            doc = json.loads(wait.stdout)
        except ValueError:
            rec["error"] = (
                "session --wait produced no JSON: "
                + wait.stderr.decode("utf-8", "replace")[-300:]
            )
            return rec
        result = doc.get("result") or {}
        usage = doc.get("usage") or {}
        rec.update(
            status=doc.get("status"),
            model=doc.get("model"),
            steps=result.get("steps", 0),
            tokens_in=usage.get("in", 0),
            tokens_out=usage.get("out", 0),
            output=(result.get("output") or "")[:400],
        )
        rec.update(classify(doc.get("messages") or []))
        if wait.returncode == 124:
            # the detached runner outlives the wait; stop it before the scratch
            # it is working in disappears under it
            subprocess.run(
                [a.tny, "--cwd", scratch, "session", "stop", sid],
                capture_output=True,
                env=env,
                timeout=60,
            )
            rec["error"] = "timeout after %ds" % a.timeout
        elif doc.get("status") != "done":
            # a failed turn has no comparable step/token profile; keep it in the
            # pass-rate denominator but out of the per-arm averages
            rec["error"] = "status %s: %s" % (
                doc.get("status"),
                (result.get("error") or "")[:200],
            )
        ok, detail = run_check(task, scratch)
        rec["pass"] = ok
        if not ok and not rec["error"]:
            rec["check"] = detail
        return rec
    except subprocess.TimeoutExpired as e:
        rec["error"] = "subprocess timeout: %s" % (e.cmd[1] if e.cmd else "?")
        return rec
    finally:
        if mock:
            mock.terminate()
            try:
                mock.wait(timeout=5)
            except subprocess.TimeoutExpired:
                mock.kill()
        shutil.rmtree(scratch, ignore_errors=True)


# ------------------------------------------------------------------ reporting
def mean(xs):
    return round(statistics.fmean(xs), 1) if xs else 0.0


def tool_histogram(rows):
    hist = {}
    for r in rows:
        for name, n in (r.get("tool_names") or {}).items():
            hist[name] = hist.get(name, 0) + n
    return dict(sorted(hist.items(), key=lambda kv: (-kv[1], kv[0])))


def comparable(rec):
    """A run whose step/token profile is meaningful: it reached a finished turn
    without a harness or provider error."""
    return rec.get("error") is None and rec.get("status") == "done"


def summarize(runs, arm):
    rows = [r for r in runs if r["arm"] == arm]
    done = [r for r in rows if comparable(r)]
    return {
        "arm": arm,
        "runs": len(rows),
        "errors": len(rows) - len(done),
        "passed": sum(1 for r in rows if r["pass"]),
        "pass_rate": round(100.0 * sum(1 for r in rows if r["pass"]) / len(rows), 1)
        if rows
        else 0.0,
        "steps": mean([r.get("steps", 0) for r in done]),
        "tool_calls": mean([r.get("tool_calls", 0) for r in done]),
        "tokens_in": mean([r.get("tokens_in", 0) for r in done]),
        "tokens_out": mean([r.get("tokens_out", 0) for r in done]),
        "wall_s": mean([r.get("wall_s", 0) for r in done]),
        "repair_loops": sum(r.get("repair_loops", 0) for r in done),
        "failed_results": sum(r.get("failed_results", 0) for r in done),
        "edit_tny_edit": sum(r.get("edit_tny_edit", 0) for r in done),
        "edit_structured": sum(r.get("edit_structured", 0) for r in done),
        "edit_shell_write": sum(r.get("edit_shell_write", 0) for r in done),
        "tool_names": tool_histogram(done),
    }


def markdown(meta, tasks, runs, summaries):
    out = []
    out.append("## Tool-profile A/B — %s" % meta["date"])
    out.append("")
    out.append(
        "provider `%s`, model `%s`, effort `%s`, N=%d per task per arm, "
        "--max-steps %d, timeout %ds, tny `%s`"
        % (
            meta["provider"],
            meta["model"],
            meta["effort"],
            meta["runs"],
            meta["max_steps"],
            meta["timeout"],
            meta["tny_version"],
        )
    )
    out.append("")
    out.append(
        "| Arm | Pass | Steps | Tool calls | Tok in | Tok out | Wall s | "
        "Repairs | tny edit | edit_file | shell write |"
    )
    out.append(
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
    )
    for s in summaries:
        out.append(
            "| `%s` | %d/%d (%.0f%%) | %.1f | %.1f | %.0f | %.0f | %.1f | %d | %d | %d | %d |"
            % (
                s["arm"],
                s["passed"],
                s["runs"],
                s["pass_rate"],
                s["steps"],
                s["tool_calls"],
                s["tokens_in"],
                s["tokens_out"],
                s["wall_s"],
                s["repair_loops"],
                s["edit_tny_edit"],
                s["edit_structured"],
                s["edit_shell_write"],
            )
        )
    out.append("")
    out.append("### Per-task pass matrix")
    out.append("")
    arms = [s["arm"] for s in summaries]
    out.append("| Task | Family | " + " | ".join("`%s`" % a for a in arms) + " |")
    out.append("| --- | --- | " + " | ".join("---" for _ in arms) + " |")
    for t in tasks:
        cells = []
        for arm in arms:
            rows = [r for r in runs if r["task"] == t["name"] and r["arm"] == arm]
            if not rows:
                cells.append("-")
            else:
                npass = sum(1 for r in rows if r["pass"])
                cells.append(
                    ("%d/%d" % (npass, len(rows)))
                    if len(rows) > 1
                    else ("pass" if npass else "fail")
                )
        out.append("| %s | %s | %s |" % (t["name"], t["family"], " | ".join(cells)))
    out.append("")
    out.append("### By family")
    out.append("")
    fams = sorted({t["family"] for t in tasks})
    out.append("| Family | " + " | ".join("`%s`" % a for a in arms) + " |")
    out.append("| --- | " + " | ".join("---:" for _ in arms) + " |")
    for fam in fams:
        cells = []
        for arm in arms:
            rows = [r for r in runs if r["family"] == fam and r["arm"] == arm]
            npass = sum(1 for r in rows if r["pass"])
            cells.append("%d/%d" % (npass, len(rows)) if rows else "-")
        out.append("| %s | %s |" % (fam, " | ".join(cells)))
    out.append("")
    return "\n".join(out)


# ----------------------------------------------------------------- fixture QA
def verify_fixtures(tasks):
    """Every check must be red on the untouched fixture and green after its
    reference solution. Keeps a task from silently scoring a free pass."""
    bad = 0
    for t in tasks:
        scratch = tempfile.mkdtemp(prefix="tnybench.fx.")
        try:
            scratch_copy(t, scratch)
            ok, _ = run_check(t, scratch)
            if ok:
                print("FAIL %s: check passes before any work" % t["name"])
                bad += 1
                continue
            solution = os.path.join(t["dir"], "solution.sh")
            if not os.path.exists(solution):
                print("FAIL %s: no solution.sh" % t["name"])
                bad += 1
                continue
            p = subprocess.run(
                ["sh", solution], cwd=scratch, capture_output=True, timeout=120
            )
            if p.returncode != 0:
                print(
                    "FAIL %s: solution.sh exit %d: %s"
                    % (t["name"], p.returncode, p.stderr.decode()[-200:])
                )
                bad += 1
                continue
            ok, detail = run_check(t, scratch)
            if not ok:
                print("FAIL %s: check red after solution.sh: %s" % (t["name"], detail))
                bad += 1
            else:
                print("ok   %s (%s)" % (t["name"], t["family"]))
        finally:
            shutil.rmtree(scratch, ignore_errors=True)
    print("%d/%d fixtures verified" % (len(tasks) - bad, len(tasks)))
    return 1 if bad else 0


def bench_home(tny):
    """A private HOME so the bench never writes sessions into the user's, while
    still resolving the provider entries from the real settings file. `bin/tny`
    shadows any installed build: the shell profiles tell the model to reach for
    `tny edit`, and it must be the binary under test."""
    home = tempfile.mkdtemp(prefix="tnybench.home.")
    os.makedirs(os.path.join(home, ".tny"), exist_ok=True)
    real = os.path.join(os.path.expanduser("~"), ".tny", "settings.json")
    if os.path.exists(real):
        shutil.copy2(real, os.path.join(home, ".tny", "settings.json"))
    bindir = os.path.join(home, "bin")
    os.makedirs(bindir, exist_ok=True)
    os.symlink(os.path.abspath(tny), os.path.join(bindir, "tny"))
    return home


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument(
        "--tny", default=os.environ.get("TNY", os.path.join(ROOT, "build", "tny"))
    )
    ap.add_argument("--arms", default=",".join(ARMS))
    ap.add_argument("--tasks", default="", help="comma-separated task names")
    ap.add_argument("--family", default="", help="comma-separated families")
    ap.add_argument("--runs", type=int, default=1, help="N runs per task per arm")
    ap.add_argument("--provider", default="aiproxy")
    ap.add_argument("--model", default="")
    ap.add_argument("--effort", default="high")
    ap.add_argument("--max-steps", type=int, default=40)
    ap.add_argument("--timeout", type=int, default=600, help="per-run seconds")
    ap.add_argument("--out", default=os.path.join(ROOT, "tests", "bench", "out"))
    ap.add_argument("--label", default="")
    ap.add_argument("--dry-run", action="store_true", help="list tasks and exit")
    ap.add_argument("--mock", action="store_true", help="scripted mock, no key")
    ap.add_argument("--verify-fixtures", action="store_true")
    ap.add_argument("--report", default="", help="re-render a stored JSON document")
    a = ap.parse_args()

    if a.report:
        doc = json.load(open(a.report))
        arms = [s["arm"] for s in doc["summaries"]]
        tasks = [
            {"name": n, "family": f}
            for n, f in sorted({(r["task"], r["family"]) for r in doc["runs"]})
        ]
        print(
            markdown(
                doc["meta"],
                tasks,
                doc["runs"],
                [summarize(doc["runs"], x) for x in arms],
            )
        )
        return 0

    names = [s for s in a.tasks.split(",") if s]
    families = [s for s in a.family.split(",") if s]
    tasks = discover(names or None, families or None)
    arms = [s for s in a.arms.split(",") if s]
    for arm in arms:
        if arm not in ARMS:
            raise SystemExit("unknown arm %r (want %s)" % (arm, ", ".join(ARMS)))

    if a.dry_run:
        for t in tasks:
            print("%-22s %s" % (t["family"], t["name"]))
        print(
            "%d tasks x %d arms x %d runs = %d runs"
            % (len(tasks), len(arms), a.runs, len(tasks) * len(arms) * a.runs)
        )
        return 0
    if a.verify_fixtures:
        return verify_fixtures(tasks)

    if not os.path.exists(a.tny):
        raise SystemExit("no tny binary at %s (make release)" % a.tny)
    version = (
        subprocess.run([a.tny, "--version"], capture_output=True)
        .stdout.decode()
        .strip()
    )

    home = bench_home(a.tny)
    runs = []
    total = len(tasks) * len(arms) * a.runs
    i = 0
    try:
        for arm in arms:
            for t in tasks:
                for k in range(a.runs):
                    i += 1
                    print(
                        "[%d/%d] %-14s %s run %d" % (i, total, arm, t["name"], k + 1),
                        file=sys.stderr,
                        flush=True,
                    )
                    rec = one_run(a, t, arm, home)
                    rec["run"] = k + 1
                    runs.append(rec)
                    print(
                        "    %s%s"
                        % (
                            "pass" if rec["pass"] else "FAIL",
                            " (%s)" % rec["error"] if rec.get("error") else "",
                        ),
                        file=sys.stderr,
                        flush=True,
                    )
    finally:
        shutil.rmtree(home, ignore_errors=True)

    summaries = [summarize(runs, arm) for arm in arms]
    meta = {
        "date": time.strftime("%Y-%m-%d"),
        "provider": "mock" if a.mock else a.provider,
        "model": a.model
        or next((r["model"] for r in runs if r.get("model")), "(settings default)"),
        "effort": a.effort,
        "runs": a.runs,
        "max_steps": a.max_steps,
        "timeout": a.timeout,
        "tny_version": version,
        "label": a.label,
    }
    report = markdown(meta, tasks, runs, summaries)
    print(report)
    os.makedirs(a.out, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    base = os.path.join(a.out, "bench-tools-%s%s" % (stamp, "-mock" if a.mock else ""))
    with open(base + ".json", "w") as f:
        json.dump(
            {"meta": meta, "summaries": summaries, "runs": runs},
            f,
            indent=2,
            sort_keys=True,
        )
        f.write("\n")
    with open(base + ".md", "w") as f:
        f.write(report + "\n")
    print("\nwrote %s.json and %s.md" % (base, base), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
