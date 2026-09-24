#!/usr/bin/env python3
"""Local 60-step Responses turn for the opt-in context-edit experiment."""

import argparse
import json
import os
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TNY = Path(os.environ.get("TNY", ROOT / "build/tny"))
STEPS = 60


def frame(event):
    return (
        f"event: {event['type']}\ndata: {json.dumps(event, separators=(',', ':'))}\n\n"
    ).encode()


class Mock(ThreadingHTTPServer):
    def __init__(self, wire):
        super().__init__(("127.0.0.1", 0), Handler)
        self.requests = []
        self.wire = wire


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_POST(self):
        endpoint = (
            "/v1/responses"
            if self.server.wire == "responses"
            else "/v1/chat/completions"
        )
        if self.path != endpoint:
            self.send_error(404)
            return
        raw = self.rfile.read(int(self.headers["Content-Length"]))
        req = json.loads(raw)
        index = len(self.server.requests)
        self.server.requests.append(req)
        items = req["input"] if self.server.wire == "responses" else req["messages"]
        results = [
            x
            for x in items
            if x.get("type") == "function_call_output" or x.get("role") == "tool"
        ]
        if len(results) != index:
            self.send_error(400, "tool call/output pairing changed")
            return
        usage = {"input_tokens": len(raw) // 4, "output_tokens": 20}
        if self.server.wire == "chat":
            chat_usage = {"prompt_tokens": len(raw) // 4, "completion_tokens": 20}
            if index < STEPS:
                events = [
                    {
                        "choices": [
                            {
                                "index": 0,
                                "delta": {
                                    "role": "assistant",
                                    "tool_calls": [
                                        {
                                            "index": 0,
                                            "id": f"c_{index}",
                                            "type": "function",
                                            "function": {
                                                "name": "read_file",
                                                "arguments": json.dumps(
                                                    {"path": f"output-{index:02d}.txt"}
                                                ),
                                            },
                                        }
                                    ],
                                },
                            }
                        ]
                    },
                    {
                        "choices": [
                            {"index": 0, "delta": {}, "finish_reason": "tool_calls"}
                        ],
                        "usage": chat_usage,
                    },
                ]
            else:
                events = [
                    {"choices": [{"index": 0, "delta": {"content": "MOCK-OK"}}]},
                    {
                        "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                        "usage": chat_usage,
                    },
                ]
            body = (
                b"".join(
                    f"data: {json.dumps(event, separators=(',', ':'))}\n\n".encode()
                    for event in events
                )
                + b"data: [DONE]\n\n"
            )
        elif index < STEPS:
            call = {
                "type": "function_call",
                "id": f"fc_{index}",
                "call_id": f"c_{index}",
                "name": "read_file",
                "arguments": json.dumps({"path": f"output-{index:02d}.txt"}),
                "status": "completed",
            }
            events = [
                {"type": "response.output_item.added", "output_index": 0, "item": call},
                {"type": "response.output_item.done", "output_index": 0, "item": call},
            ]
        else:
            events = [
                {
                    "type": "response.output_text.delta",
                    "output_index": 0,
                    "item_id": "answer",
                    "delta": "MOCK-OK",
                }
            ]
        if self.server.wire == "responses":
            events.append(
                {
                    "type": "response.completed",
                    "response": {"status": "completed", "usage": usage},
                }
            )
            body = b"".join(map(frame, events))
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True


def measure(requests, wire):
    key = "input" if wire == "responses" else "messages"
    payloads = [json.dumps(r[key], separators=(",", ":")).encode() for r in requests]
    changes = 1
    for prior, current in zip(requests, requests[1:]):
        if current[key][: len(prior[key])] != prior[key]:
            changes += 1
    return {
        "requests": len(requests),
        "input_bytes_per_request": [len(p) for p in payloads],
        "distinct_prefixes": changes,
        "total_input_bytes": sum(map(len, payloads)),
    }


def run(flag, workspace, root, wire):
    server = Mock(wire)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    home = root / ("on" if flag else "off")
    home.mkdir()
    env = dict(
        os.environ,
        HOME=str(home),
        OPENAI_BASE_URL=f"http://127.0.0.1:{server.server_port}/v1",
        OPENAI_API_KEY="synthetic",
        OPENAI_WIRE_API=wire,
        TNY_SELF_IMPROVE="0",
        TNY_ISOLATE="0",
        TNY_EXP_CTX_EDIT="1" if flag else "0",
        TNY_TOOLS="all",
    )
    try:
        result = subprocess.run(
            [str(TNY), "--cwd", str(workspace), "ask", "--json", "run mock turn"],
            env=env,
            capture_output=True,
            timeout=180,
        )
        if result.returncode:
            raise RuntimeError(
                result.stderr.decode()[-3000:] + result.stdout.decode()[-1000:]
            )
        output = json.loads(result.stdout)
        assert "MOCK-OK" in output["output"], output
        assert len(server.requests) == STEPS + 1, len(server.requests)
        report = measure(server.requests, wire)
        report["context_edits"] = 0
        sessions = list((home / ".tny" / "sessions").glob("*/*/session.json"))
        if sessions:
            saved = json.loads(sessions[0].read_text())
            edits = saved.get("context_edits", [])
            report["context_edits"] = len(edits)
            report["payback_requests_per_batch"] = [
                round(event["payback_requests"], 2) for event in edits
            ]
            report["removed_bytes_per_batch"] = [
                event["removed_bytes"] for event in edits
            ]
            report["affected_bytes_per_batch"] = [
                event["affected_bytes"] for event in edits
            ]
        assert report["distinct_prefixes"] == report["context_edits"] + 1, report
        return report
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wire", choices=("responses", "chat"), default="responses")
    args = parser.parse_args()
    tmp = os.environ.get("TMPDIR")
    with tempfile.TemporaryDirectory(prefix="tny-ctx-edit-", dir=tmp) as directory:
        root = Path(directory)
        workspace = root / "workspace"
        workspace.mkdir()
        for i in range(STEPS):
            size = (2 + i * 7 % 29) * 1024
            (workspace / f"output-{i:02d}.txt").write_text(chr(65 + i % 26) * size)
        result = {
            "off": run(False, workspace, root, args.wire),
            "on": run(True, workspace, root, args.wire),
        }
        print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
