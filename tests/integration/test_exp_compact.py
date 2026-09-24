#!/usr/bin/env python3
"""Local provider checks for token-triggered compaction; --measure runs long cases."""

import hashlib
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

IMAGE_URL = re.compile(rb"data:image/[a-z]+;base64,[A-Za-z0-9+/=]+")

TNY = str(
    Path(
        sys.argv[1]
        if len(sys.argv) > 1 and not sys.argv[1].startswith("-")
        else os.environ.get("TNY", "build/tny")
    ).resolve()
)


def write_screenshot(path):
    """Write a valid, mostly incompressible PNG for image-billing checks."""
    width, height = 400, 300
    rng = random.Random(17)
    pixels = b"".join(b"\0" + rng.randbytes(width * 3) for _ in range(height))

    def chunk(kind, data):
        payload = kind + data
        return (
            struct.pack(">I", len(data))
            + payload
            + struct.pack(">I", zlib.crc32(payload) & 0xFFFFFFFF)
        )

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(pixels, 9)) + chunk(b"IEND", b"")
    path.write_bytes(png)
    return len(png)


class Provider(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_POST(self):
        server = self.server
        raw = self.rfile.read(int(self.headers["Content-Length"]))
        body = json.loads(raw)
        chat = "messages" in body
        summary = body.get("tool_choice") == "none"
        image_urls = IMAGE_URL.findall(raw) if server.scenario == "image" else []
        input_tokens = max(
            1, (len(raw) - sum(map(len, image_urls))) // 4 + 1500 * len(image_urls)
        )
        request = {
            "bytes": len(raw),
            "input_tokens": input_tokens,
            "images": len(image_urls),
            "body": body,
            "summary": summary,
        }
        if getattr(server, "capture_raw", False):
            request["raw"] = raw
        server.requests.append(request)
        if (
            not summary
            and getattr(server, "overflow_once", False)
            and not server.overflow_done
            and server.normal_count >= 2
        ):
            server.overflow_done = True
            payload = (
                b'{"error":{"type":"invalid_request_error",'
                b'"code":"context_length_exceeded","message":"too long"}}'
            )
            self.send_response(400)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return
        if summary:
            server.compactions += 1
            if (
                getattr(server, "partial_summary_retry", False)
                and server.compactions == 1
            ):
                wire = (
                    b'data: {"type":"response.output_text.delta",'
                    b'"delta":"PARTIALSTART"}\n\n'
                )
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(wire)))
                self.end_headers()
                self.wfile.write(wire)
                return
            if server.fail_summary and server.compactions == 1:
                payload = (
                    b'{"error":{"type":"invalid_request_error","message":"fixture"}}'
                )
                self.send_response(400)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                return
            answer = "GOAL=continue; constraints=preserve user text; completed=prior steps; remaining=continue"
            call = None
        elif server.scenario == "turns":
            server.phase ^= 1
            call = server.phase == 1
            answer = "turn complete"
        else:
            call = server.normal_count < server.steps
            answer = "long turn complete"
        if not summary:
            server.normal_count += 1
        usage = {
            "prompt_tokens" if chat else "input_tokens": input_tokens,
            "completion_tokens" if chat else "output_tokens": 12,
        }
        if call:
            index = server.normal_count
            if server.scenario == "image" and index % 5 == 1:
                name = "read_image"
                arguments = json.dumps({"path": str(server.image_path)})
            else:
                name = "terminal"
                size = (
                    6000
                    if server.scenario == "image"
                    else 4096 + (index % 5) * 4096
                    if server.scenario == "long"
                    else 32
                )
                arguments = json.dumps({"command": f"printf '%{size}s' x"})
            call_id = f"call_{index}"
        if chat:
            delta = (
                {
                    "tool_calls": [
                        {
                            "index": 0,
                            "id": call_id,
                            "type": "function",
                            "function": {"name": name, "arguments": arguments},
                        }
                    ]
                }
                if call
                else {"content": answer}
            )
            event = {
                "choices": [
                    {
                        "index": 0,
                        "delta": delta,
                        "finish_reason": "tool_calls" if call else "stop",
                    }
                ]
            }
            if body.get("stream_options", {}).get("include_usage"):
                event["usage"] = usage
            events = [event]
            wire = b"".join(
                b"data: " + json.dumps(e).encode() + b"\n\n" for e in events
            )
            wire += b"data: [DONE]\n\n"
        else:
            events = []
            if call:
                events.append(
                    {
                        "type": "response.output_item.done",
                        "output_index": 0,
                        "item": {
                            "type": "function_call",
                            "call_id": call_id,
                            "name": name,
                            "arguments": arguments,
                        },
                    }
                )
            else:
                events.append({"type": "response.output_text.delta", "delta": answer})
            events.append(
                {
                    "type": "response.completed",
                    "response": {"status": "completed", "usage": usage},
                }
            )
            wire = b"".join(
                b"data: " + json.dumps(e).encode() + b"\n\n" for e in events
            )
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(wire)))
        self.end_headers()
        self.wfile.write(wire)


def archive_valid(entry):
    path = Path(entry["transcript"])
    if not path.is_file():
        return False
    content = path.read_text()
    messages = (
        [json.loads(line) for line in content.splitlines()]
        if path.suffix == ".jsonl"
        else json.loads(content)
    )
    return len(messages) == int(path.stem.rsplit("-", 1)[-1])


def run_case(
    scenario,
    enabled,
    wire="responses",
    steps=4,
    turns=2,
    fail_summary=False,
    tokens=128000,
    isolate=False,
    ctx_edit=False,
    ctx_trigger=1000,
    ctx_step=1000,
    partial_summary_retry=False,
    overflow_once=False,
):
    with tempfile.TemporaryDirectory() as home:
        ws = Path(home) / "workspace"
        ws.mkdir()
        image_bytes = write_screenshot(ws / "shot.png") if scenario == "image" else 0
        server = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
        server.requests = []
        server.compactions = 0
        server.normal_count = 0
        server.phase = 0
        server.scenario = scenario
        server.image_path = ws / "shot.png"
        server.steps = steps
        server.fail_summary = fail_summary
        server.partial_summary_retry = partial_summary_retry
        server.overflow_once = overflow_once
        server.overflow_done = False
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = dict(os.environ)
        for name in list(env):
            if name.endswith("_API_KEY") or name.endswith("_BASE_URL"):
                env.pop(name)
        env.pop("TNY_ISOLATE", None)
        env.update(
            HOME=home,
            OPENAI_API_KEY="fixture",
            OPENAI_BASE_URL=f"http://127.0.0.1:{server.server_port}/v1",
            OPENAI_WIRE_API=wire,
            TNY_TOOLS="terminal",
            TNY_SELF_IMPROVE="0",
            TNY_EXP_COMPACT="1" if enabled else "0",
            TNY_EXP_COMPACT_TOKENS=str(tokens),
            TNY_EXP_CTX_EDIT="1" if ctx_edit else "0",
            TNY_EXP_CTX_EDIT_TRIGGER=str(ctx_trigger),
            TNY_EXP_CTX_EDIT_STEP=str(ctx_step),
            TNY_EXP_CTX_EDIT_KEEP="1",
        )
        if not isolate:
            env["TNY_ISOLATE"] = "0"
        try:
            for turn in range(turns if scenario == "turns" else 1):
                command = [TNY, "--cwd", str(ws), "--yolo", "ask", "--json"]
                if turn:
                    command += ["--resume", "last"]
                command += [f"user turn {turn}: preserve this exact request"]
                result = subprocess.run(
                    command, env=env, capture_output=True, timeout=240
                )
                assert result.returncode == 0, (
                    result.returncode,
                    result.stdout[-1000:],
                    result.stderr[-1000:],
                )
            sessions = list((Path(home) / ".tny" / "sessions").glob("*/*/session.json"))
            assert len(sessions) == 1, sessions
            session = json.loads(sessions[0].read_text())
            archives_valid = all(
                archive_valid(entry) for entry in session.get("compactions", [])
            )
            requests = server.requests
            sizes = [r["bytes"] for r in requests]
            first = [r for r in requests if not r["summary"]]
            if scenario == "turns":
                first = first[::2]
            roots = []
            for r in first:
                body = r["body"]
                items = body.get("input", body.get("messages", []))
                roots.append(json.dumps(items[0] if items else {}, sort_keys=True))
            # Every archived call that remains in the provider view has its output.
            for r in requests:
                items = r["body"].get("input", r["body"].get("messages", []))
                if "messages" in r["body"]:
                    calls = [
                        call["id"]
                        for item in items
                        for call in item.get("tool_calls", [])
                    ]
                    outputs = [
                        item.get("tool_call_id")
                        for item in items
                        if item.get("role") == "tool"
                    ]
                else:
                    calls = [
                        item.get("call_id")
                        for item in items
                        if item.get("type") == "function_call"
                    ]
                    outputs = [
                        item.get("call_id")
                        for item in items
                        if item.get("type") == "function_call_output"
                    ]
                assert set(calls) == set(outputs), (calls, outputs)
            return {
                "scenario": scenario,
                "enabled": enabled,
                "default_isolation": isolate,
                "wire": wire,
                "request_input_bytes": sizes,
                "normal_input_tokens": [
                    r["input_tokens"] for r in requests if not r["summary"]
                ],
                "image_bytes": image_bytes,
                "max_real_input_tokens": max(
                    (r["input_tokens"] for r in requests if not r["summary"]),
                    default=0,
                ),
                "normal_requests_over_limit": sum(
                    r["input_tokens"] >= tokens for r in requests if not r["summary"]
                ),
                "image_requests": sum(r["images"] > 0 for r in requests),
                "last_compact_after_tokens": session.get("last_compact_after_tokens"),
                "last_input_tokens": session.get("last_input_tokens"),
                "total_bytes": sum(sizes),
                "distinct_first_request_prefixes": len(set(roots)),
                "compactions": (
                    server.compactions
                    if enabled
                    else sum("Earlier in this session" in root for root in roots)
                ),
                "summary_requests": server.compactions,
                "summary_positions": [
                    index
                    for index, request in enumerate(requests)
                    if request["summary"]
                ],
                "recorded_compactions": len(session.get("compactions", [])),
                "latest_prompt_on_final_wire": f"user turn {turns - 1}"
                in json.dumps(requests[-1]["body"])
                if scenario == "turns"
                else None,
                "context_edits": len(session.get("context_edits", [])),
                "request_stub_counts": [
                    json.dumps(r["body"]).count("[cleared:") for r in requests
                ],
                "chat_usage_requested": all(
                    r["body"].get("stream_options", {}).get("include_usage")
                    for r in requests
                )
                if wire == "chat" and enabled
                else None,
                "archives_valid": archives_valid,
                "session": session,
            }
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


def compare_main_wire(main_binary, wire="responses"):
    """Compare complete flag-off HTTP bodies on the same native runner fixture."""
    with tempfile.TemporaryDirectory() as home:
        workspace = Path(home) / "workspace"
        workspace.mkdir()
        server = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
        server.scenario = "turns"
        server.steps = 0
        server.fail_summary = False
        server.capture_raw = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = dict(os.environ)
        for name in list(env):
            if name.endswith("_API_KEY") or name.endswith("_BASE_URL"):
                env.pop(name)
        env.pop("TNY_ISOLATE", None)
        env.update(
            HOME=home,
            OPENAI_API_KEY="fixture",
            OPENAI_BASE_URL=f"http://127.0.0.1:{server.server_port}/v1",
            OPENAI_WIRE_API=wire,
            TNY_TOOLS="terminal",
            TNY_SELF_IMPROVE="0",
            TNY_EXP_COMPACT="0",
        )

        def capture(binary):
            shutil.rmtree(Path(home) / ".tny", ignore_errors=True)
            server.requests = []
            server.compactions = 0
            server.normal_count = 0
            server.phase = 0
            for turn in range(20):
                command = [binary, "--cwd", str(workspace), "--yolo", "ask", "--json"]
                if turn:
                    command += ["--resume", "last"]
                command += [f"user turn {turn}: preserve this exact request"]
                result = subprocess.run(
                    command, env=env, capture_output=True, timeout=120
                )
                assert result.returncode == 0, (
                    result.returncode,
                    result.stdout[-1000:],
                    result.stderr[-1000:],
                )
            return [request["raw"] for request in server.requests]

        try:
            main = capture(str(Path(main_binary).resolve()))
            branch = capture(TNY)
            return {
                "main_binary": str(Path(main_binary).resolve()),
                "branch_binary": TNY,
                "default_isolation": True,
                "wire": wire,
                "requests": len(branch),
                "main_request_bytes": [len(raw) for raw in main],
                "branch_request_bytes": [len(raw) for raw in branch],
                "main_sha256": hashlib.sha256(b"".join(main)).hexdigest(),
                "branch_sha256": hashlib.sha256(b"".join(branch)).hexdigest(),
                "identical": main == branch,
            }
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


class CompactTests(unittest.TestCase):
    def test_image_billing_allows_repeated_compaction(self):
        result = run_case("image", True, steps=65, tokens=20000, isolate=True)
        self.assertGreater(result["image_bytes"], 300000)
        self.assertGreater(result["image_requests"], 0)
        self.assertGreaterEqual(result["compactions"], 5)
        self.assertLess(result["max_real_input_tokens"], 60000)
        self.assertFalse(result["session"].get("last_compact_estimated"))
        self.assertIn(
            result["last_compact_after_tokens"], result["normal_input_tokens"]
        )
        self.assertTrue(result["archives_valid"])

    def test_responses_between_turns(self):
        result = run_case("turns", True, turns=3, tokens=1200, isolate=True)
        self.assertGreaterEqual(result["compactions"], 1)
        self.assertEqual(result["summary_positions"][0], 2)
        self.assertTrue(result["archives_valid"])

    def test_responses_inside_turn_and_fallback(self):
        result = run_case(
            "long", True, steps=6, fail_summary=True, tokens=3000, isolate=True
        )
        self.assertGreaterEqual(result["compactions"], 2)
        self.assertGreaterEqual(result["recorded_compactions"], 2)
        self.assertTrue(result["archives_valid"])
        self.assertIn("GOAL=continue", result["session"]["compact"]["summary"])

    def test_chat_inside_turn(self):
        result = run_case("long", True, wire="chat", steps=5, tokens=3000, isolate=True)
        self.assertGreaterEqual(result["compactions"], 1)
        self.assertTrue(result["archives_valid"])
        self.assertIn("GOAL=continue", result["session"]["compact"]["summary"])
        self.assertTrue(result["chat_usage_requested"])

    def test_summary_retry_discards_partial(self):
        result = run_case(
            "long",
            True,
            steps=6,
            tokens=3000,
            isolate=True,
            partial_summary_retry=True,
        )
        self.assertGreaterEqual(result["compactions"], 2)
        self.assertNotIn("PARTIALSTART", result["session"]["compact"]["summary"])

    def test_context_overflow_recovers_without_usage(self):
        result = run_case(
            "turns", True, turns=2, tokens=128000, isolate=True, overflow_once=True
        )
        self.assertEqual(0, result["summary_requests"])
        self.assertGreaterEqual(result["recorded_compactions"], 1)
        self.assertTrue(result["latest_prompt_on_final_wire"])

    def test_both_flags_default_isolation(self):
        result = run_case(
            "long",
            True,
            steps=24,
            tokens=7000,
            isolate=True,
            ctx_edit=True,
            ctx_trigger=3000,
            ctx_step=3000,
        )
        self.assertGreaterEqual(result["compactions"], 1)
        self.assertGreaterEqual(result["context_edits"], 1)
        for index in result["summary_positions"]:
            if index:
                self.assertEqual(
                    result["request_stub_counts"][index - 1],
                    result["request_stub_counts"][index],
                )
            if index + 1 < len(result["request_stub_counts"]):
                self.assertLessEqual(
                    result["request_stub_counts"][index + 1],
                    result["request_stub_counts"][index],
                )

    def test_default_isolation_runner(self):
        for wire in ("responses", "chat"):
            with self.subTest(wire=wire):
                result = run_case(
                    "long", True, wire=wire, steps=6, tokens=3000, isolate=True
                )
                self.assertGreaterEqual(result["compactions"], 1)
                self.assertGreaterEqual(result["recorded_compactions"], 1)
                self.assertTrue(result["archives_valid"])


if __name__ == "__main__":
    if "--compare-main" in sys.argv:
        result = compare_main_wire(
            sys.argv[sys.argv.index("--compare-main") + 1],
            "chat" if "--wire-chat" in sys.argv else "responses",
        )
        print(json.dumps(result, indent=2))
        if not result["identical"]:
            sys.exit(1)
    elif "--measure" in sys.argv or "--measure-isolated" in sys.argv:
        isolated = "--measure-isolated" in sys.argv
        output = [
            {
                k: v
                for k, v in run_case("turns", flag, turns=20, isolate=isolated).items()
                if k != "session"
            }
            for flag in (False, True)
        ]
        output += [
            {
                k: v
                for k, v in run_case("long", flag, steps=120, isolate=isolated).items()
                if k != "session"
            }
            for flag in (False, True)
        ]
        print(json.dumps(output, indent=2))
    else:
        unittest.main(argv=[sys.argv[0]])
