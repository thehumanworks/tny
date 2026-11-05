#!/usr/bin/env python3
"""Same four 100ms local web reads, serial versus existing native parallel API."""

import argparse
import json
import statistics
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from bench import run


class Slow(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_GET(self):
        time.sleep(0.1)
        data = self.path.encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--tny", required=True)
    p.add_argument("--runs", type=int, default=10)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", 0), Slow)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    results = {
        "workload": "four independent 100ms loopback web_fetch calls",
        "arms": {},
    }
    calls = [
        {
            "name": "web_fetch",
            "arguments": {"url": f"http://127.0.0.1:{server.server_port}/{i}"},
        }
        for i in range(4)
    ]
    rows = {"serial": [], "parallel": []}
    try:
        for _ in range(args.runs):
            for arm in rows:
                code = (
                    'tools.describe("web_fetch")\ncalls = json.loads('
                    + repr(json.dumps(calls))
                    + ")\n"
                )
                code += (
                    "results = json.loads(tools.parallel(json.dumps(calls)))\n"
                    if arm == "parallel"
                    else 'results = [tools.call(c["name"], json.dumps(c["arguments"])) for c in calls]\n'
                )
                code += 'assert len(results) == 4\nfor i, result in enumerate(results):\n    assert result.startswith("HTTP 200") and result.endswith("/"+str(i)), result\nprint(42)'
                rows[arm].append(
                    run(str(Path(args.tny).resolve()), 0, False, code=code)
                )
        for arm, samples in rows.items():
            results["arms"][arm] = {
                "median_tool_continuation_ms": statistics.median(
                    r["tool_continuation_ms"] for r in samples
                ),
                "runs": samples,
            }
        args.output.write_text(json.dumps(results, indent=2) + "\n")
        print({k: v["median_tool_continuation_ms"] for k, v in results["arms"].items()})
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
