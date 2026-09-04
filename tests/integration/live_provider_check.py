#!/usr/bin/env python3
"""Live check of the native loop against a real provider profile.

Not part of `make test` (it needs a key and network). Runs, for each wire
requested, the sequence users hit when "stream errors after a tool call"
poison a session (docs/adr/0069):

  1. a prompt that forces a tool call (read a file), so the turn has a
     tool-result POST;
  2. a follow-up turn on the same saved session (the transcript now holds
     assistant tool_calls + tool results + reasoning passthrough members);
  3. a resumed turn after simulating a runner death mid-batch (the last
     tool result is removed from session.json) — the provider must accept
     the repaired view.

Every run prints the exit code, the diagnostics tny wrote to stderr, and
the transcript shape; a non-zero exit or a missing tool call fails the
check. Provider text stays redacted unless TNY_DEBUG_PROVIDER_ERRORS=1 is
exported before running this script.

Usage:
  tests/integration/live_provider_check.py --provider aiproxy --model grok-4.6
  tests/integration/live_provider_check.py --provider openrouter \\
      --model anthropic/claude-sonnet-4.6 --wire chat --effort medium
  tests/integration/live_provider_check.py --provider codex --model gpt-5.5

A private HOME is used for the sessions so the user's settings.json and
last_provider are untouched; provider env vars (NAME_BASE_URL, NAME_API_KEY)
and auth stores are read from the real environment/HOME through --auth-home.
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TNY = os.environ.get("TNY", os.path.join(ROOT, "build", "tny"))


def run(env, ws, args, timeout):
    r = subprocess.run(
        [TNY, "--cwd", ws, *args],
        env=env,
        capture_output=True,
        timeout=timeout,
    )
    return r


def shape(session_file):
    doc = json.load(open(session_file))
    out = []
    for m in doc["messages"]:
        role = m.get("role")
        if role == "assistant" and m.get("tool_calls"):
            extras = [
                k
                for k in ("reasoning_details", "reasoning_content", "reasoning_items")
                if k in m
            ]
            out.append(
                f"assistant[{','.join(t['id'] for t in m['tool_calls'])}]"
                + (f"+{'+'.join(extras)}" if extras else "")
            )
        elif role == "tool":
            out.append(f"tool[{m.get('tool_call_id')}]")
        else:
            c = m.get("content")
            out.append(f"{role}({len(c) if isinstance(c, str) else 'parts'})")
    return " ".join(out)


def check_wire(args, wire, home):
    ws = os.path.join(home, f"ws-{wire}")
    os.makedirs(ws)
    open(os.path.join(ws, "notes.txt"), "w").write("the secret word is pelican\n")
    env = dict(os.environ, HOME=home)
    if args.auth_home:
        # credential stores live in the real HOME; copy what the profiles read
        for rel in (
            ".tny/codex-auth.json",
            ".codex/auth.json",
            ".grok/auth.json",
            ".claude/.credentials.json",
        ):
            src = os.path.join(args.auth_home, rel)
            if os.path.exists(src):
                dst = os.path.join(home, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy(src, dst)
    base = ["--provider", args.provider, "--model", args.model, "--wire-api", wire]
    if args.effort:
        base += ["--effort", args.effort]
    ok = True

    def report(label, r):
        nonlocal ok
        print(f"--- {wire}: {label}: exit {r.returncode}")
        err = r.stderr.decode(errors="replace").strip()
        if err:
            print("    stderr: " + err.replace("\n", "\n            "))
        try:
            out = json.loads(r.stdout)
            print(
                f"    steps={out.get('steps')} tools={[t['name'] + ':' + t['status'] for t in out.get('tool_calls', [])]}"
            )
            print(f"    output: {out.get('output', '')[:200]!r}")
            return out
        except ValueError:
            print(f"    stdout: {r.stdout[:300]!r}")
            ok = False
            return None

    r = run(
        env,
        ws,
        [
            *base,
            "ask",
            "--json",
            "Use a tool to read notes.txt and tell me the secret word. Do not guess.",
        ],
        args.timeout,
    )
    out = report("tool-call turn", r)
    if r.returncode != 0 or not out or not out.get("tool_calls"):
        print("    FAIL: the first turn must succeed and call a tool")
        ok = False
    files = glob.glob(os.path.join(home, ".tny", "sessions", "*", "*", "session.json"))
    files = [
        f
        for f in files
        if json.load(open(f)).get("workspace", "").endswith(f"ws-{wire}") or True
    ]
    latest = max(files, key=os.path.getmtime) if files else None
    if latest:
        print(f"    transcript: {shape(latest)}")

    r = run(
        env,
        ws,
        [
            *base,
            "ask",
            "--json",
            "--resume",
            "last",
            "Now say the word again, uppercase, and nothing else.",
        ],
        args.timeout,
    )
    out = report("follow-up turn", r)
    if r.returncode != 0:
        print("    FAIL: the follow-up after a tool call must succeed")
        ok = False
    if latest:
        print(f"    transcript: {shape(latest)}")

    if latest and args.simulate_crash:
        doc = json.load(open(latest))
        msgs = doc["messages"]
        victims = [i for i, m in enumerate(msgs) if m.get("role") == "tool"]
        if victims:
            del msgs[victims[-1]]
            json.dump(doc, open(latest, "w"))
            print(
                f"    simulated runner death: removed one tool result -> {shape(latest)}"
            )
            r = run(
                env,
                ws,
                [
                    *base,
                    "ask",
                    "--json",
                    "--resume",
                    "last",
                    "One more time: the word, lowercase.",
                ],
                args.timeout,
            )
            report("resume after unpaired batch", r)
            if r.returncode != 0:
                print("    FAIL: the repaired transcript must be accepted")
                ok = False
            if b"repaired the transcript" not in r.stderr:
                print(
                    "    note: no repair status line (nothing to repair, or stderr suppressed)"
                )
    return ok


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--provider", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--wire", choices=["responses", "chat", "both"], default="both")
    ap.add_argument("--effort", default=None)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument(
        "--auth-home",
        default=os.path.expanduser("~"),
        help="HOME holding codex/grok/claude credential stores (default: real HOME)",
    )
    ap.add_argument("--no-crash", dest="simulate_crash", action="store_false")
    args = ap.parse_args()
    wires = ["responses", "chat"] if args.wire == "both" else [args.wire]
    ok = True
    with tempfile.TemporaryDirectory(prefix="tny-live-") as home:
        os.makedirs(os.path.join(home, ".tny"))
        for wire in wires:
            try:
                ok = check_wire(args, wire, home) and ok
            except subprocess.TimeoutExpired as exc:
                print(f"--- {wire}: TIMEOUT: {exc}")
                ok = False
    print("live_provider_check:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
