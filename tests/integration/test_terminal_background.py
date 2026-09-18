#!/usr/bin/env python3
"""Collect detached terminal commands across real turns and runner teardown.

Reuses the terminal cancellation suite's stdlib loopback provider. No keys or
remote services. Native, every tool profile, isolated and in-process turns.
"""

from __future__ import annotations

import json
import os
import shlex
import subprocess
import tempfile
import threading
from pathlib import Path

from test_terminal_cancel import TNY, Handler, QuietServer, check, workspace_env


class BackgroundHandler(Handler):
    arguments = {}
    results = []

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        results = [m["content"] for m in body["messages"] if m["role"] == "tool"]
        if results:
            type(self).results.append(results[-1])
            self._stream(
                [
                    {"choices": [{"index": 0, "delta": {"content": "done"}}]},
                    {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]},
                ]
            )
            return
        self._stream(
            [
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {
                                "role": "assistant",
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "id": "terminal-call",
                                        "type": "function",
                                        "function": {
                                            "name": "terminal",
                                            "arguments": json.dumps(
                                                type(self).arguments
                                            ),
                                        },
                                    }
                                ],
                            },
                        }
                    ]
                },
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]},
            ]
        )


def turn(env, workspace, arguments):
    BackgroundHandler.arguments = arguments
    BackgroundHandler.results = []
    result = subprocess.run(
        [TNY, "--provider", "openai", "--yolo", "ask", "run the terminal tool"],
        cwd=workspace,
        env=env,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=20,
        check=False,
    )
    check(result.returncode == 0, (result.returncode, result.stdout, result.stderr))
    check(len(BackgroundHandler.results) == 1, BackgroundHandler.results)
    return json.loads(BackgroundHandler.results[0])


def main():
    if os.environ.get("TNY_TEST_EXPECT_WASM") == "1":
        print(
            "skip (wasm): background terminal returns unsupported; no native processes"
        )
        return
    server = QuietServer(("127.0.0.1", 0), BackgroundHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        for profile in ("all", "terminal", "terminal+edit"):
            for isolated in ("0", "1"):
                with tempfile.TemporaryDirectory(prefix="tny-terminal-bg-") as tmp:
                    env, workspace = workspace_env(tmp, server.server_address[1])
                    env.update(TNY_TOOLS=profile, TNY_ISOLATE=isolated)
                    pidfile = Path(workspace) / "command.pid"
                    launched = turn(
                        env,
                        workspace,
                        {
                            "command": f"echo $$ > {shlex.quote(str(pidfile))}; sleep 1; exit 7",
                            "background": True,
                        },
                    )
                    check(launched["task_id"], launched)
                    check("pid" not in launched, launched)
                    # The first caller/runner has now exited. A new session must
                    # recover the real result without inheriting child ownership.
                    done = turn(
                        env, workspace, {"task_id": launched["task_id"], "wait_s": 5}
                    )
                    check(done["state"] == "failed" and done["exit_code"] == 7, done)
                    check(
                        done["signal"] is None and done["status_source"] == "waitpid",
                        done,
                    )
                    check(Path(done["log"]).read_bytes() == b"", done)
                    # One postcondition probe, NOT polling for task completion.
                    pid = int(pidfile.read_text())
                    try:
                        os.kill(pid, 0)
                    except ProcessLookupError:
                        pass
                    else:
                        raise AssertionError(
                            f"collected command {pid} remains (possibly a zombie)"
                        )
                    again = turn(env, workspace, {"task_id": launched["task_id"]})
                    check(again["state"] == "failed" and again["exit_code"] == 7, again)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    print(
        "ok  background terminal: all profiles, cross-session collection, no zombie command"
    )


if __name__ == "__main__":
    main()
