#!/usr/bin/env python3
"""Real browser WASM JSONL delivery under an initially unavailable host sink.

Explicit acceptance invocation (missing tool/artifact is then a failure):
  TNY_TEST_WASM_BACKPRESSURE_REQUIRED=1 python3 tests/integration/test_wasm_event_backpressure.py

The ordinary native fixture sweep does not provision browsers. It does not
count a not-run browser case as passing the separate WASM acceptance gate.
"""

import http.server
import json
import os
import shutil
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class Fixture(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Access-Control-Allow-Methods", "POST, OPTIONS")
        self.end_headers()

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length))
        self.server.requests.append(body)
        chunks = [
            {
                "id": "fixture",
                "choices": [{"index": 0, "delta": {"content": "WASM ready"}}],
            },
            {
                "id": "fixture",
                "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                "usage": {
                    "prompt_tokens": 3,
                    "completion_tokens": 2,
                    "total_tokens": 5,
                },
            },
        ]
        data = (
            "".join("data: " + json.dumps(row) + "\n\n" for row in chunks)
            + "data: [DONE]\n\n"
        )
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(data.encode())))
        self.end_headers()
        self.wfile.write(data.encode())


def main():
    if os.environ.get("TNY_TEST_WASM_BACKPRESSURE_REQUIRED") != "1":
        print(
            "WASM browser backpressure: NOT RUN (separate explicit browser acceptance target)"
        )
        return
    from playwright.sync_api import sync_playwright

    module = ROOT / "build/wasm/tny-web.mjs"
    if not module.is_file() or not module.with_suffix(".wasm").is_file():
        raise RuntimeError(
            "WASM acceptance requires make wasm-web and its actual artifact"
        )
    api = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
    api.requests = []
    thread = threading.Thread(target=api.serve_forever, daemon=True)
    thread.start()
    with tempfile.TemporaryDirectory(prefix="tny-wasm-backpressure-") as temporary:
        root = Path(temporary)
        shutil.copy2(module, root / module.name)
        shutil.copy2(module.with_suffix(".wasm"), root / "tny-web.wasm")
        (root / "index.html").write_text(
            "<!doctype html><title>tny WASM verification</title>"
        )

        class Assets(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, directory=str(root), **kwargs)

            def log_message(self, *_args):
                pass

        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Assets)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        try:
            with sync_playwright() as playwright:
                browser = playwright.chromium.launch()
                try:
                    page = browser.new_page()
                    page.set_default_timeout(30000)
                    errors = []
                    page.on("pageerror", lambda exc: errors.append(str(exc)))
                    page.goto(f"http://127.0.0.1:{httpd.server_port}/index.html")
                    result = page.evaluate(
                        """async base => {
                          const create = (await import('/tny-web.mjs')).default;
                          const lines = [], diagnostics = [];
                          let raw = ""; const decoder = new TextDecoder();
                          let ready = false, falsePolls = 0, firstWriteAt = null;
                          const began = performance.now();
                          let exitResolve;
                          const exited = new Promise(resolve => { exitResolve = resolve; });
                          const instance = await create({
                            noInitialRun: true,
                            tnyEnv: {HOME:'/tmp',TNY_TOOLS:'all',OPENAI_API_KEY:'fixture-key'},
                            tnyOut: bytes => { if(firstWriteAt === null) firstWriteAt=performance.now()-began; raw += decoder.decode(bytes,{stream:true}); },
                            print: text => { if(firstWriteAt === null) firstWriteAt = performance.now()-began; lines.push(text); },
                            printErr: text => diagnostics.push(text),
                            onExit: code => exitResolve(code),
                            __tnyPollStdout: fd => { if(!ready) falsePolls++; return ready; }
                          });
                          instance.ENV.HOME = '/tmp';
                          instance.ENV.TNY_TOOLS = 'all';
                          setTimeout(() => { ready = true; }, 1250);
                          instance.callMain(['--provider','openai','--base-url',base+'/v1',
                            '--wire-api','chat','ask','--ephemeral',
                            '--events=jsonl','--progress=none','say hello']);
                          const code = await Promise.race([exited,new Promise((_, reject) =>
                            setTimeout(() => reject(new Error('real WASM turn did not exit')),20000))]);
                          await new Promise(resolve => setTimeout(resolve,0));
                          return {code,lines:raw.split("\\n"),diagnostics,falsePolls,firstWriteAt,elapsed:performance.now()-began};
                        }""",
                        f"http://127.0.0.1:{api.server_port}",
                    )
                    if errors:
                        raise AssertionError(f"browser execution error: {errors}")
                    assert result["code"] == 0, result
                    assert result["falsePolls"] >= 8, result
                    assert result["firstWriteAt"] >= 1200, result
                    events = [
                        json.loads(line) for line in result["lines"] if line.strip()
                    ]
                    assert events, result
                    assert sum(row["type"] == "turn_end" for row in events) == 1, events
                    assert events[-1]["type"] == "turn_end", events
                    assert len({row["sequence"] for row in events}) == len(events), (
                        events
                    )
                    assert [row["sequence"] for row in events] == sorted(
                        row["sequence"] for row in events
                    ), events
                    assert "WASM ready" in "".join(
                        row.get("text", "") for row in events
                    ), events
                    assert not result["diagnostics"], result
                    assert len(api.requests) == 1, api.requests
                    print(
                        json.dumps(
                            {
                                "browser": browser.version,
                                "false_polls": result["falsePolls"],
                                "first_write_ms": result["firstWriteAt"],
                                "events": len(events),
                                "provider_requests": len(api.requests),
                                "exit_code": result["code"],
                            }
                        )
                    )
                finally:
                    browser.close()
        finally:
            httpd.shutdown()
            httpd.server_close()
            api.shutdown()
            api.server_close()
            thread.join(timeout=2)
    print(
        "PASS real WASM initially-unavailable stdout preserves canonical event delivery"
    )


if __name__ == "__main__":
    started = time.monotonic()
    main()
    print(f"browser backpressure fixture duration: {time.monotonic() - started:.3f}s")
