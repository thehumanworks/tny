#!/usr/bin/env python3
"""Compare model-visible bytes and fetch count, not tokens or task quality."""

import argparse
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "integration"))
import test_execution_code_mode as fixture  # noqa: E402


def measure(binary):
    fixture.TNY = str(Path(binary).resolve())
    test = fixture.ExecutionCodeMode()
    test.setUp()
    data = b"target line\n" + b"irrelevant line\n" * 20000
    (test.workspace / "bulk.txt").write_bytes(data)

    class Page(BaseHTTPRequestHandler):
        calls = 0

        def log_message(self, *_):
            pass

        def do_GET(self):
            Page.calls += 1
            self.send_response(200)
            self.send_header("Content-Type", "text/markdown")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    web = ThreadingHTTPServer(("127.0.0.1", 0), Page)
    thread = threading.Thread(target=web.serve_forever, daemon=True)
    thread.start()
    try:
        result = {"binary": binary, "source_bytes": len(data), "wires": {}}
        for wire in ("responses", "chat"):
            row = {}
            for name, args in (
                ("read_file", {"path": "bulk.txt"}),
                ("web_fetch", {"url": f"http://127.0.0.1:{web.server_port}/page"}),
            ):
                before = Page.calls
                output = test.run_code(
                    f"print(tools.call({name!r}, {json.dumps(args)!r}))",
                    wire=wire,
                    no_save=False,
                )
                row[name] = {
                    "model_visible_bytes": len(output.encode()),
                    "fetch_count": Page.calls - before,
                }
            result["wires"][wire] = row
        return result
    finally:
        web.shutdown()
        thread.join()
        web.server_close()
        test.doCleanups()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    args = parser.parse_args()
    print(json.dumps([measure(args.baseline), measure(args.candidate)], indent=2))


if __name__ == "__main__":
    main()
