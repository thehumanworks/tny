"""End-to-end agent-task latency: baseline (Lua cells) versus Python cells.

A local Responses-wire provider replays a fixed sequence of `run_code` calls
and then a final message; `tny ask --json --no-save` runs it end to end
(process start, provider stream, execution server, cell, nested tools). The
baseline binary gets Lua cells and the candidate the equivalent Python cells;
runs are interleaved per round. Synthetic credentials, private HOME and
workspace, loopback only; model latency is excluded by construction.

  bench_agent_cells.py --baseline BIN --candidate BIN [--rounds N] --output FILE
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import statistics
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

# (task, baseline Lua cells, candidate Python cells). Both do the same work.
TASKS = {
    "empty_cell": (["print(1)"], ["print(1)"]),
    "inspect": (
        [
            'local files = tools.call("list_files", json.encode({path = "."}))\n'
            'local readme = tools.call("read_file", json.encode({path = "README.md"}))\n'
            'local hits = tools.call("grep_files", json.encode({pattern = "TODO", path = "."}))\n'
            "print(#files, #readme, #hits)"
        ],
        [
            'files = tools.call("list_files", json.dumps({"path": "."}))\n'
            'readme = tools.call("read_file", json.dumps({"path": "README.md"}))\n'
            'hits = tools.call("grep_files", json.dumps({"pattern": "TODO", "path": "."}))\n'
            "print(len(files), len(readme), len(hits))"
        ],
    ),
    "edit": (
        [
            'local text = tools.call("read_file", json.encode({path = "notes.txt"}))\n'
            'text = string.gsub(text, "draft", "final")\n'
            'tools.call("write_file", json.encode({path = "notes.txt", content = text}))\n'
            'print(tools.call("read_file", json.encode({path = "notes.txt"})))'
        ],
        [
            'text = tools.call("read_file", json.dumps({"path": "notes.txt"}))\n'
            'text = text.replace("draft", "final")\n'
            'tools.call("write_file", json.dumps({"path": "notes.txt", "content": text}))\n'
            'print(tools.call("read_file", json.dumps({"path": "notes.txt"})))'
        ],
    ),
    "compute": (
        [
            "local t = {}\nfor i = 1, 20000 do t[#t + 1] = i * i end\nlocal s = 0\nfor _, v in ipairs(t) do s = s + v end\nprint(s)"
        ],
        ["t = [i * i for i in range(1, 20001)]\nprint(sum(t))"],
    ),
    "three_cells": (
        [
            "print(1)",
            'print(tools.call("read_file", json.encode({path = "README.md"})))',
            "print(3)",
        ],
        [
            "print(1)",
            'print(tools.call("read_file", json.dumps({"path": "README.md"})))',
            "print(3)",
        ],
    ),
}


def sse(frames: list[dict]) -> bytes:
    return b"".join(
        ("event: " + f["type"] + "\ndata: " + json.dumps(f) + "\n\n").encode()
        for f in frames
    )


def response(call: str | None, index: int) -> bytes:
    frames = [{"type": "response.created", "response": {"status": "in_progress"}}]
    if call is None:
        frames.append(
            {
                "type": "response.output_text.delta",
                "output_index": 0,
                "item_id": "m",
                "delta": "DONE",
            }
        )
    else:
        item = {
            "type": "function_call",
            "id": f"fc_{index}",
            "call_id": f"call_{index}",
            "name": "run_code",
            "arguments": json.dumps({"code": call}),
        }
        frames += [
            {"type": "response.output_item.added", "output_index": 0, "item": item},
            {
                "type": "response.output_item.done",
                "output_index": 0,
                "item": dict(item, status="completed"),
            },
        ]
    frames.append({"type": "response.completed", "response": {"status": "completed"}})
    return sse(frames)


class Provider(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        outputs = [
            m["output"]
            for m in body.get("input", [])
            if m.get("type") == "function_call_output"
        ]
        self.server.outputs = outputs
        cells = self.server.cells
        data = response(
            cells[len(outputs)] if len(outputs) < len(cells) else None, len(outputs)
        )
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def run_once(
    binary: str, cells: list[str], server: ThreadingHTTPServer
) -> tuple[float, list[str]]:
    server.cells = cells
    server.outputs = []
    with tempfile.TemporaryDirectory(
        prefix="tny-agent-cells-", dir=os.environ.get("TMPDIR")
    ) as tmp:
        home = Path(tmp)
        workspace = home / "workspace"
        workspace.mkdir()
        (workspace / "README.md").write_text("# demo\n" + "TODO: item\n" * 20)
        (workspace / "notes.txt").write_text("draft plan\n" * 50)
        for i in range(30):
            (workspace / f"f{i}.txt").write_text(f"line {i}\n")
        env = {
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "HOME": str(home),
            "TMPDIR": str(home),
            "TNY_ISOLATE": "0",
            "TNY_TOOLS": "all",
            "TNY_SELF_IMPROVE": "0",
            "OPENAI_API_KEY": "bench-not-real",
            "OPENAI_BASE_URL": f"http://127.0.0.1:{server.server_port}/v1",
        }
        start = time.perf_counter()
        done = subprocess.run(
            [
                binary,
                "--cwd",
                str(workspace),
                "--provider",
                "openai",
                "--wire-api",
                "responses",
                "ask",
                "--json",
                "--no-save",
                "bench",
            ],
            env=env,
            capture_output=True,
            timeout=120,
            check=False,
        )
        elapsed = (time.perf_counter() - start) * 1000
        if done.returncode:
            raise SystemExit(f"{binary} failed: {done.stderr.decode()[-2000:]}")
        result = json.loads(done.stdout)
        calls = result.get("tool_calls", [])
        if result.get("exit_code") != 0 or len(server.outputs) != len(cells):
            raise SystemExit(
                "incomplete tool-cell execution cannot be a latency sample"
            )
        if not calls or any(call.get("status") != "success" for call in calls):
            raise SystemExit(
                "failed nested calls cannot be a successful latency sample"
            )
        if sum(call.get("name") == "run_code" for call in calls) != len(cells):
            raise SystemExit("wrong number of completed code cells")
        return elapsed, list(server.outputs)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--rounds", type=int, default=15)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    report: dict = {
        "rounds": args.rounds,
        "tasks": {},
        "scope": "Serial rotated native loopback-provider tasks; all cell and nested-tool statuses must succeed. Model inference excluded; warm page cache; shared host.",
        "binaries": {
            arm: {
                "path": str(Path(binary).resolve()),
                "bytes": Path(binary).stat().st_size,
                "sha256": hashlib.sha256(Path(binary).read_bytes()).hexdigest(),
            }
            for arm, binary in (
                ("baseline", args.baseline),
                ("candidate", args.candidate),
            )
        },
        "load_average_at_start": list(os.getloadavg())
        if hasattr(os, "getloadavg")
        else None,
    }
    for task, (lua, python) in TASKS.items():
        arms = {"baseline": (args.baseline, lua), "candidate": (args.candidate, python)}
        samples: dict[str, list[float]] = {"baseline": [], "candidate": []}
        outputs = {}
        for arm, (binary, cells) in arms.items():  # warmup, and keep one output
            _, outputs[arm] = run_once(binary, cells, server)
        for index in range(args.rounds):
            order = (
                ["baseline", "candidate"]
                if index % 2 == 0
                else ["candidate", "baseline"]
            )
            for arm in order:
                binary, cells = arms[arm]
                samples[arm].append(run_once(binary, cells, server)[0])
        row = {
            arm: {"median_ms": statistics.median(v), "min_ms": min(v), "samples_ms": v}
            for arm, v in samples.items()
        }
        row["delta_median_ms"] = (
            row["candidate"]["median_ms"] - row["baseline"]["median_ms"]
        )
        row["outputs"] = outputs
        report["tasks"][task] = row
        print(
            f"{task}: baseline {row['baseline']['median_ms']:.1f} ms, candidate "
            f"{row['candidate']['median_ms']:.1f} ms (delta {row['delta_median_ms']:+.1f})",
            flush=True,
        )
    server.shutdown()
    for arm, binary in (("baseline", args.baseline), ("candidate", args.candidate)):
        if (
            hashlib.sha256(Path(binary).read_bytes()).hexdigest()
            != report["binaries"][arm]["sha256"]
        ):
            raise SystemExit("binary changed during the paired benchmark")
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=1) + "\n")


if __name__ == "__main__":
    main()
