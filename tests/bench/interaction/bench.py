#!/usr/bin/env python3
"""Measure CLI startup and tool-continuation latency against loopback SSE.

Synthetic credentials, fresh home/workspace; no provider or network variability.
A large function-call event tests the transport's buffered-data scheduling.
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "integration"))
from test_execution_code_mode import events


class Provider(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.received.append(time.perf_counter())
        outputs = [
            v["output"]
            for v in body["input"]
            if v.get("type") == "function_call_output"
        ]
        if outputs:
            if self.server.expected not in outputs[-1] or "Traceback" in outputs[-1]:
                self.server.errors.append(outputs[-1])
            data = events("responses")
        else:
            data = events("responses", "run_code", {"code": self.server.code})
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        self.wfile.flush()
        self.server.sent.append(time.perf_counter())


def run(binary, padding, isolated, *, code=None, expected="42"):
    with tempfile.TemporaryDirectory(prefix="tny-latency-") as tmp:
        server = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
        server.received, server.sent, server.errors = [], [], []
        server.code = code if code is not None else "#" + "x" * padding + "\nprint(42)"
        server.expected = expected
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = {
            "PATH": os.environ["PATH"],
            "HOME": tmp,
            "TMPDIR": tmp,
            "OPENAI_API_KEY": "synthetic-benchmark",
            "OPENAI_BASE_URL": f"http://127.0.0.1:{server.server_port}/v1",
        }
        if not isolated:
            env["TNY_ISOLATE"] = "0"
        start = time.perf_counter()
        try:
            process = subprocess.Popen(
                [
                    binary,
                    "--cwd",
                    tmp,
                    "--provider",
                    "openai",
                    "--wire-api",
                    "responses",
                    "ask",
                    "--json",
                    "benchmark",
                ],
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            watchdog = threading.Timer(20, process.kill)
            watchdog.start()
            for line in process.stdout:
                json.loads(line)  # validate the final machine-readable result
            _, err = process.communicate(timeout=15)
            watchdog.cancel()
            elapsed = time.perf_counter() - start
            if process.returncode or server.errors or len(server.received) != 2:
                raise RuntimeError(
                    f"Failed run: exit={process.returncode}, requests={len(server.received)}, outputs={server.errors}, {err}"
                )
            return {
                "start_to_request_ms": (server.received[0] - start) * 1000,
                "tool_continuation_ms": (server.received[1] - server.sent[0]) * 1000,
                "total_ms": elapsed * 1000,
            }
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tny", required=True)
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = {"binary": str(Path(args.tny).resolve()), "cases": {}}
    for isolated in (False, True):
        for padding in (0, 5000):
            name = f"{'runner' if isolated else 'in_process'}_padding_{padding}"
            rows = [run(result["binary"], padding, isolated) for _ in range(args.runs)]
            medians = {
                k: statistics.median(r[k] for r in rows)
                for k in rows[0]
                if rows[0][k] is not None
            }
            result["cases"][name] = {"median": medians, "runs": rows}
            print(name, medians, flush=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
