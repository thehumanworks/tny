"""Five-turn repeated-prefix evaluations using unchanged harness cache settings."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import secrets
import subprocess
import time
from pathlib import Path

from adapters import final_message, invocation, resume_id, session_invocation
from cache_proxy import RecordingProxy
from experiment import atomic
from run import _git_init

HERE = Path(__file__).resolve().parent
COLORS = ["amber", "cyan", "violet", "green", "silver"]
INDICES = [19, 74, 143, 199, 5]


def run_round(state, scenario, harness, rep, nonce):
    root = Path(state["temporary_root"])
    folder = root / "output/cache" / scenario / harness / f"rep-{rep:02d}"
    if folder.exists():
        raise RuntimeError(f"Refusing to overwrite cache round {folder}")
    workspace = folder / "workspace"
    workspace.mkdir(parents=True)
    context = (
        "Synthetic catalog "
        + nonce
        + "\nAnswer catalog questions using only the requested color. No tool use is needed.\n"
    )
    context += (
        "\n".join(
            f"Item {i:03d}: product SKU{i:03d}; color {COLORS[i % 5]}; shelf {i % 17}; quantity {7 * i + 3}; batch R{i % 31:02d}."
            for i in range(200)
        )
        + "\n"
    )
    (workspace / "AGENTS.md").write_text(context)
    _git_init(workspace)
    session = None
    turns = []
    with RecordingProxy(folder / "proxy", Path.home() / ".codex/auth.json") as proxy:
        for turn, index in enumerate(INDICES, 1):
            prompt = f"What is the color of catalog item {index:03d}? Answer only the color without using tools."
            proxy.begin_turn(turn)
            if scenario == "conversation":
                call = session_invocation(
                    harness,
                    folder,
                    proxy.base_url,
                    prompt,
                    "gpt-6-luna",
                    "low",
                    str(root / "bin/tny"),
                    session,
                )
            else:
                call = invocation(
                    harness,
                    folder,
                    proxy.base_url,
                    prompt,
                    "gpt-6-luna",
                    "low",
                    str(root / "bin/tny"),
                )
            start = time.monotonic()
            completed = subprocess.run(
                call.command,
                cwd=workspace,
                env=call.env,
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=True,
                timeout=120,
            )
            elapsed = time.monotonic() - start
            time.sleep(0.25)
            output = final_message(harness, completed.stdout)
            observed_session = resume_id(harness, completed.stdout)
            if scenario == "conversation" and session and observed_session != session:
                raise RuntimeError("Conversation session identity changed")
            session = observed_session
            (folder / f"turn-{turn:02d}.stdout.txt").write_text(completed.stdout)
            (folder / f"turn-{turn:02d}.stderr.txt").write_text(completed.stderr)
            rows = [r for r in proxy.rows if r.get("turn") == turn]
            valid = bool(rows) and all(
                r.get("completion") == "response.completed"
                and r.get("requested_model") == "gpt-6-luna"
                and r.get("requested_effort") == "low"
                and r.get("reported_model") == "gpt-6-luna"
                and all(
                    type(r.get(k)) is int
                    for k in ["input_tokens", "cached_input_tokens", "output_tokens"]
                )
                for r in rows
            )
            turns.append(
                {
                    "turn": turn,
                    "question_index": index,
                    "expected": COLORS[index % 5],
                    "output": output,
                    "correct": output.strip() == COLORS[index % 5],
                    "exit_code": completed.returncode,
                    "wall_s": elapsed,
                    "session_present": bool(observed_session),
                    "requests": len(rows),
                    "measurement_valid": valid,
                    "command": call.command,
                }
            )
            atomic(folder / "turns.json", turns)
        rows = proxy.rows
    result = {
        "scenario": scenario,
        "harness": harness,
        "rep": rep,
        "context_sha256": hashlib.sha256(context.encode()).hexdigest(),
        "context_bytes": len(context.encode()),
        "turns": turns,
        "requests": rows,
    }
    atomic(folder / "result.json", result)
    print(
        json.dumps(
            {
                "scenario": scenario,
                "harness": harness,
                "rep": rep,
                "correct": sum(t["correct"] for t in turns),
                "turns": len(turns),
                "input": sum(r["input_tokens"] for r in rows),
                "cached": sum(r["cached_input_tokens"] for r in rows),
                "output": sum(r["output_tokens"] for r in rows),
            }
        ),
        flush=True,
    )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--live", action="store_true")
    args = parser.parse_args()
    if not args.live:
        raise SystemExit("Live inference requires --live")
    state = json.loads((HERE / "STATE.json").read_text())
    root = Path(state["temporary_root"])
    os.environ["TMPDIR"] = str(root / "tmp")
    os.environ["PATH"] = (
        str(Path.home() / ".local/share/mise/installs/python/3.14.7/bin")
        + os.pathsep
        + os.environ["PATH"]
    )
    if (
        not (root / "primary.exit").exists()
        or (root / "primary.exit").read_text().strip() != "0"
    ):
        raise SystemExit(
            "Complete primary run first; cache trials must not overlap latency measurements"
        )
    results = []
    for scenario in ("conversation", "fresh"):
        for rep in (1, 2):
            nonce = secrets.token_hex(16)
            for harness in ("tny", "codex") if rep == 1 else ("codex", "tny"):
                results.append(run_round(state, scenario, harness, rep, nonce))
    atomic(root / "cache-results.json", results)
    assert len(results) == 8
    print(
        "COMPLETE",
        len(results),
        "rounds",
        sum(len(r["turns"]) for r in results),
        "turns",
        flush=True,
    )


if __name__ == "__main__":
    main()
