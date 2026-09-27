"""Opt-in live paired trials through the normal Codex ChatGPT login."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
from typing import Any

from cases import LANGUAGES, REPEATS, TASKS, prompt
from execute import BUILD, ROOT, evaluate, strict_json

MODEL = "gpt-6-luna"
SCHEMA = {"type": "object", "properties": {"code": {"type": "string"}},
          "required": ["code"], "additionalProperties": False}


def atomic_json(path: Path, value: Any) -> None:
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
    temporary.replace(path)


def generation_valid(events: list[dict[str, Any]], rc: int, answer: Any) -> bool:
    completed = [e for e in events if e.get("type") == "turn.completed"]
    messages_only = all(e.get("item", {}).get("type") in (None, "agent_message", "reasoning")
                        for e in events if e.get("type", "").startswith("item."))
    usage = completed[0].get("usage", {}) if len(completed) == 1 else {}
    usage_valid = all(type(usage.get(k)) is int and usage[k] >= 0
                      for k in ("input_tokens", "cached_input_tokens", "output_tokens"))
    return (rc == 0 and len(completed) == 1 and messages_only and usage_valid
            and not any(e.get("type") in ("error", "turn.failed") for e in events)
            and isinstance(answer, dict) and set(answer) == {"code"}
            and isinstance(answer["code"], str) and bool(answer["code"].strip()))


def generate(instructions: str, destination: Path) -> dict[str, Any]:
    destination.mkdir(parents=True)
    (destination / "prompt.txt").write_text(instructions)
    with tempfile.TemporaryDirectory(prefix="tny-language-generation-") as temporary:
        work = Path(temporary)
        schema = work / "schema.json"
        schema.write_text(json.dumps(SCHEMA))
        output = work / "answer.json"
        command = ["codex", "exec", "--ignore-user-config", "--ignore-rules",
                   "--skip-git-repo-check", "--ephemeral", "--model", MODEL,
                   "-c", 'model_reasoning_effort="low"', "--sandbox", "read-only",
                   "--json", "--color", "never", "--output-schema", str(schema),
                   "--output-last-message", str(output), "-"]
        start = time.monotonic()
        process = subprocess.Popen(command, cwd=work, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, start_new_session=True)
        timed_out = False
        try:
            stdout, stderr = process.communicate(instructions.encode(), timeout=100)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate(timeout=10)
        elapsed = time.monotonic() - start
        (destination / "events.jsonl").write_bytes(stdout)
        (destination / "stderr.txt").write_bytes(stderr)
        events = []
        try:
            events = [strict_json(line) for line in stdout.decode().splitlines() if line.strip()]
            answer = strict_json(output.read_text()) if output.exists() else None
        except (ValueError, UnicodeError):
            answer = None
        completed = [e for e in events if e.get("type") == "turn.completed"]
        receipt = {"requested_model": MODEL, "effort": "low", "command": command,
                   "generation_ok": generation_valid(events, process.returncode, answer),
                   "exit_code": process.returncode, "timed_out": timed_out,
                   "wall_seconds": elapsed,
                   "usage": completed[0].get("usage") if len(completed) == 1 else None,
                   "code": answer.get("code", "") if isinstance(answer, dict) else "",
                   "prompt_sha256": hashlib.sha256(instructions.encode()).hexdigest()}
        atomic_json(destination / "generation.json", receipt)
        return receipt


def sample(language: str, task: str, repetition: int, output: Path) -> dict[str, Any]:
    key = f"{task}-{repetition}-{language}"
    destination = output / key
    if destination.exists():
        raise RuntimeError(f"Refusing to overwrite or selectively rerun an existing sample: {key}")
    original = prompt(language, task)
    attempts = []
    request = original
    for attempt in range(2):
        result = generate(request, destination / f"attempt-{attempt}")
        evaluation = evaluate(language, task, result["code"]) if result["generation_ok"] else {
            "passed": False, "variants": [], "generation_failed": True}
        attempts.append({"generation": result, "evaluation": evaluation})
        if evaluation["passed"] or not result["generation_ok"]:
            break
        feedback = [{"variant": v["variant"], "execution_ok": v["execution_ok"],
                     "output_ok": v["output_ok"], "trace_ok": v["trace_ok"],
                     "observed": v["observed"]} for v in evaluation["variants"] if not v["passed"]]
        request = original + "\nRepair the following previous attempt. This is the only repair opportunity.\nPrevious code:\n" + result["code"] + "\nObserved failures (expected answers are not supplied):\n" + json.dumps(feedback, ensure_ascii=False)[:16000]
    row = {"id": key, "language": language, "task": task, "repetition": repetition, "attempts": attempts,
           "first_pass": attempts[0]["evaluation"]["passed"], "passed": attempts[-1]["evaluation"]["passed"]}
    atomic_json(destination / "sample.json", row)
    return row


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--live", action="store_true", help="Explicitly authorize this bounded Codex account benchmark")
    parser.add_argument("--output", type=Path, default=BUILD / "live")
    parser.add_argument("--workers", type=int, choices=range(1, 4), default=3)
    args = parser.parse_args()
    if not args.live:
        raise SystemExit("Live inference is opt-in: pass --live")
    controls = json.loads((BUILD / "controls.json").read_text())
    if len(controls) != 36 or not all(c["passed"] for c in controls):
        raise SystemExit("All reference controls must pass before live inference")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sorted(Path(__file__).parent.glob("*")) if p.is_file()}
    order = [(language, task, repetition) for repetition in range(REPEATS)
             for index, (task, _) in enumerate(TASKS)
             for language in (LANGUAGES[(index + repetition + offset) % 3] for offset in range(3))]
    atomic_json(args.output / "manifest.json", {
        "model": MODEL, "effort": "low", "source_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "codex_version": subprocess.check_output(["codex", "--version"], text=True).strip(),
        "source_sha256": manifest, "executor_build": json.loads((BUILD / "build.json").read_text()),
        "workers": args.workers, "order": order,
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "authentication": "normal Codex CLI ChatGPT login; credentials never read by benchmark"})
    rows = []
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(sample, *entry, args.output) for entry in order]
        for future in as_completed(futures):
            row = future.result()
            rows.append(row)
            print(f"{len(rows)}/{len(order)} {row['id']} first={row['first_pass']} final={row['passed']}", flush=True)
    atomic_json(args.output / "samples.json", sorted(rows, key=lambda r: r["id"]))
    print("COMPLETE", len(rows), flush=True)


if __name__ == "__main__":
    main()
