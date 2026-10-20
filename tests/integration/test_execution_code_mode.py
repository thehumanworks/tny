#!/usr/bin/env python3
"""Execution code mode through the production binary and local provider wires.

Synthetic credentials, private HOME/workspace, real process and file effects.
Raw wire fixtures deliberately bypass shared fixture conversion for direct-call
rejection checks. No live inference, external account, or executor mock.
"""

from __future__ import annotations

import json
import os
import shutil
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from code_mode_fixture import python_string

ROOT = Path(__file__).resolve().parents[2]
TNY = str(Path(os.environ.get("TNY", ROOT / "build/tny")).resolve())
WASM = "/wasm/" in TNY or bool(os.environ.get("TNY_TEST_EXPECT_WASM"))


def events(wire, name=None, arguments=None, call_id="execution_1"):
    if wire == "chat":
        delta = {"content": "EXEC-OK"}
        if name:
            delta = {
                "tool_calls": [
                    {
                        "index": 0,
                        "id": call_id,
                        "type": "function",
                        "function": {"name": name, "arguments": json.dumps(arguments)},
                    }
                ]
            }
        frames = [
            {"choices": [{"index": 0, "delta": delta}]},
            {
                "choices": [
                    {
                        "index": 0,
                        "delta": {},
                        "finish_reason": "tool_calls" if name else "stop",
                    }
                ]
            },
        ]
        return (
            b"".join(("data: " + json.dumps(f) + "\n\n").encode() for f in frames)
            + b"data: [DONE]\n\n"
        )
    frames = [{"type": "response.created", "response": {"status": "in_progress"}}]
    if name:
        item = {
            "type": "function_call",
            "id": "fc_execution",
            "call_id": call_id,
            "name": name,
            "arguments": json.dumps(arguments),
        }
        frames += [
            {"type": "response.output_item.added", "output_index": 0, "item": item},
            {
                "type": "response.output_item.done",
                "output_index": 0,
                "item": dict(item, status="completed"),
            },
        ]
    else:
        frames += [
            {
                "type": "response.output_text.delta",
                "output_index": 0,
                "item_id": "msg_execution",
                "delta": "EXEC-OK",
            }
        ]
    frames += [{"type": "response.completed", "response": {"status": "completed"}}]
    return b"".join(
        ("event: " + f["type"] + "\ndata: " + json.dumps(f) + "\n\n").encode()
        for f in frames
    )


def mint_loopback_certificate(cert, key, subject, alt_names, openssl="openssl"):
    """Write a throwaway self-signed certificate and key for loopback TLS.

    An explicit minimal config keeps this independent of the host's
    openssl.cnf, which a Homebrew install may lack or point elsewhere.
    Failures carry openssl's own diagnostics.
    """
    config = Path(key).with_suffix(".cnf")
    config.write_text("[req]\ndistinguished_name = dn\n[dn]\n", encoding="utf-8")
    run = subprocess.run(
        [
            openssl,
            "req",
            "-config",
            str(config),
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-days",
            "1",
            "-subj",
            f"/CN={subject}",
            "-addext",
            f"subjectAltName={alt_names}",
            "-keyout",
            str(key),
            "-out",
            str(cert),
        ],
        capture_output=True,
        text=True,
        timeout=60,
    )
    if run.returncode:
        raise AssertionError(
            f"openssl req exited {run.returncode}: {run.stderr.strip() or run.stdout}"
        )


class Provider(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.bodies.append(body)
        wire = "chat" if self.path.endswith("chat/completions") else "responses"
        if wire == "chat":
            output = [m["content"] for m in body["messages"] if m.get("role") == "tool"]
        else:
            output = [
                m["output"]
                for m in body["input"]
                if m.get("type") == "function_call_output"
            ]
        self.server.outputs = output
        if self.server.sequence is not None and len(output) < len(self.server.sequence):
            data = events(
                wire,
                "run_code",
                self.server.sequence[len(output)],
                f"execution_{len(output) + 1}",
            )
        else:
            data = (
                events(wire)
                if output
                else events(wire, self.server.call_name, self.server.arguments)
            )
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass


class Loopback(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_GET(self):
        body = f"loopback:{self.path}".encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


@unittest.skipIf(WASM, "wasm execution is covered by the clean-error seam")
class ExecutionCodeMode(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="tny-execution-test-")
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.workspace = self.home / "workspace"
        self.workspace.mkdir()
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
        self.server.bodies = []
        self.server.outputs = []
        self.server.sequence = None
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.close_server)
        self.env = {
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "HOME": str(self.home),
            "TMPDIR": str(self.home),
            "TNY_ISOLATE": "0",
            "TNY_TOOLS": "all",
            "TNY_SELF_IMPROVE": "0",
            "OPENAI_API_KEY": "execution-fixture-not-real",
            "OPENAI_BASE_URL": f"http://127.0.0.1:{self.server.server_port}/v1",
        }

    def close_server(self):
        self.server.shutdown()
        self.thread.join(timeout=3)
        self.server.server_close()

    def start(
        self,
        code=None,
        *,
        wire="responses",
        name="run_code",
        arguments=None,
        env=None,
        flags=(),
    ):
        self.server.bodies.clear()
        self.server.outputs.clear()
        self.server.call_name = name
        self.server.arguments = arguments if arguments is not None else {"code": code}
        return subprocess.Popen(
            [
                TNY,
                "--cwd",
                str(self.workspace),
                "--provider",
                "openai",
                "--wire-api",
                wire,
                *flags,
                "ask",
                "--json",
                "--no-save",
                "execution fixture",
            ],
            env=dict(self.env, **(env or {})),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def run_code(self, code=None, **kwargs):
        process = self.start(code, **kwargs)
        try:
            stdout, stderr = process.communicate(timeout=15)
        except BaseException:
            process.kill()
            process.wait(timeout=5)
            raise
        self.assertEqual(process.returncode, 0, (stdout, stderr))
        self.assertIn("EXEC-OK", json.loads(stdout)["output"])
        self.assertEqual(len(self.server.outputs), 1, self.server.outputs)
        return self.server.outputs[0]

    @unittest.skipIf(WASM, "fresh-exec parallel workers require native processes")
    def test_parallel_reads_overlap_and_keep_result_order_on_both_wires(self):
        # A serial implementation cannot pass this rendezvous: no HTTP
        # response is sent until four independent effects have arrived.
        barrier = threading.Barrier(4)
        observations = []

        class Rendezvous(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                observations.append(self.path)
                try:
                    barrier.wait(timeout=5)
                    body = self.path.encode()
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                except threading.BrokenBarrierError:
                    self.send_error(500, "calls did not overlap")

        web = ThreadingHTTPServer(("127.0.0.1", 0), Rendezvous)
        thread = threading.Thread(target=web.serve_forever, daemon=True)
        thread.start()
        try:
            for wire in ("responses", "chat"):
                with self.subTest(wire=wire):
                    calls = [
                        {
                            "name": "web_fetch",
                            "arguments": {
                                "url": f"http://127.0.0.1:{web.server_port}/{index}"
                            },
                        }
                        for index in range(8)
                    ]
                    output = self.run_code(
                        f"print(tools.parallel({json.dumps(calls)!r}))", wire=wire
                    )
                    results = json.loads(output)
                    self.assertEqual(len(results), 8)
                    for index, result in enumerate(results):
                        self.assertTrue(result.startswith("HTTP 200"), result)
                        self.assertTrue(result.endswith(f"/{index}"), result)
                    self.assertNotIn("error:", output)
            self.assertEqual(len(observations), 16)
        finally:
            web.shutdown()
            thread.join(timeout=3)
            web.server_close()

    @unittest.skipIf(WASM, "fresh-exec parallel workers require native processes")
    def test_parallel_stateful_barriers_and_failure_results(self):
        calls = [
            {
                "name": "write_file",
                "arguments": {"path": "ordered.txt", "content": "first"},
            },
            {"name": "read_file", "arguments": {"path": "ordered.txt"}},
            {"name": "read_file", "arguments": {"path": "missing.txt"}},
            {
                "name": "write_file",
                "arguments": {"path": "ordered.txt", "content": "last"},
            },
            {"name": "read_file", "arguments": {"path": "ordered.txt"}},
        ]
        output = self.run_code(f"print(tools.parallel({json.dumps(calls)!r}))")
        results = json.loads(output)
        self.assertEqual(len(results), len(calls))
        self.assertEqual(results[1], "first")
        self.assertTrue(results[2].startswith("error:"), results[2])
        self.assertEqual(results[4], "last")
        self.assertEqual((self.workspace / "ordered.txt").read_text(), "last")

    @unittest.skipIf(WASM, "fresh-exec parallel workers require native processes")
    def test_parallel_rejects_malformed_batch_before_effects(self):
        calls = [
            {
                "name": "write_file",
                "arguments": {"path": "must-not-exist", "content": "x"},
            },
            {"name": "__parallel", "arguments": {}},
        ]
        output = self.run_code(f"print(tools.parallel({json.dumps(calls)!r}))")
        self.assertIn("recursive batches forbidden", output)
        self.assertFalse((self.workspace / "must-not-exist").exists())

    @unittest.skipIf(WASM, "fresh-exec parallel workers require native processes")
    def test_parallel_spills_remain_readable_and_profile_is_not_widened(self):
        (self.workspace / "large-a").write_text("a" * 40000)
        (self.workspace / "large-b").write_text("b" * 40000)
        calls = [
            {"name": "read_file", "arguments": {"path": path}}
            for path in ("large-a", "large-b")
        ]
        output = self.run_code(
            f"results = json.loads(tools.parallel({json.dumps(calls)!r}))\n"
            "import re\n"
            "for result in results:\n"
            '    handle = re.search(r\'handle "([^"]+)"\', result).group(1)\n'
            "    print(tools.call('read_tool_result', json.dumps({'handle': handle, 'offset': 39998, 'length': 2})))\n"
        )
        self.assertIn("aa", output)
        self.assertIn("bb", output)
        denied = self.run_code(
            f"print(tools.parallel({json.dumps(calls)!r}))",
            env={"TNY_TOOLS": "terminal"},
        )
        self.assertIn("error:", denied)
        self.assertNotIn("aaaa", denied)

    @unittest.skipIf(WASM, "fresh-exec parallel workers require native processes")
    def test_parallel_owner_stop_cancels_outstanding_read_and_later_write(self):
        entered = threading.Event()
        release = threading.Event()

        class Reads(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                if self.path == "/slow":
                    entered.set()
                    release.wait(timeout=8)
                    return
                if not entered.wait(timeout=3):
                    self.send_error(500, "second worker never entered")
                    return
                self.send_response(200)
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"ok")

        web = ThreadingHTTPServer(("127.0.0.1", 0), Reads)
        thread = threading.Thread(target=web.serve_forever, daemon=True)
        thread.start()
        try:
            calls = [
                {
                    "name": "web_fetch",
                    "arguments": {"url": f"http://127.0.0.1:{web.server_port}/{path}"},
                }
                for path in ("fast", "slow")
            ] + [
                {
                    "name": "write_file",
                    "arguments": {"path": "after-stop.txt", "content": "forbidden"},
                }
            ]
            started = time.monotonic()
            output, _ = self.private_cell(
                f"print(tools.parallel({json.dumps(calls)!r}))",
                timeout=5000,
                stop_control=3,
            )
            self.assertTrue(entered.is_set())
            self.assertIn("cancelled by owner policy", output)
            self.assertNotIn("timeout", output)
            self.assertLess(time.monotonic() - started, 4)
            self.assertFalse((self.workspace / "after-stop.txt").exists())
        finally:
            release.set()
            web.shutdown()
            thread.join(timeout=3)
            web.server_close()

    @unittest.skipIf(WASM, "fresh-exec parallel workers require native processes")
    def test_parallel_approval_pause_precedes_all_worker_deadlines(self):
        approved = threading.Event()

        class AfterApproval(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                if not approved.wait(timeout=4):
                    self.send_error(500, "approval never completed")
                    return
                self.send_response(200)
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"ok")

        web = ThreadingHTTPServer(("127.0.0.1", 0), AfterApproval)
        thread = threading.Thread(target=web.serve_forever, daemon=True)
        thread.start()
        try:
            calls = [
                {
                    "name": "web_fetch",
                    "arguments": {"url": f"http://127.0.0.1:{web.server_port}/{i}"},
                }
                for i in range(2)
            ]
            output, observed = self.private_cell(
                f"print(tools.parallel({json.dumps(calls)!r}))",
                timeout=1200,
                permission=0,
                prompt_gate=approved,
            )
            self.assertEqual(observed.count("prompt"), 2)
            results = json.loads(output)
            self.assertEqual(len(results), 2)
            for result in results:
                self.assertTrue(result.startswith("HTTP 200"), result)
        finally:
            approved.set()
            web.shutdown()
            thread.join(timeout=3)
            web.server_close()

    @unittest.skipIf(WASM, "fresh-exec parallel workers require native processes")
    def test_parallel_timeout_does_not_run_later_barrier(self):
        class Slow(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                self.server.entered.set()
                self.server.release.wait(timeout=8)
                self.close_connection = True

        web = ThreadingHTTPServer(("127.0.0.1", 0), Slow)
        web.entered = threading.Event()
        web.release = threading.Event()
        thread = threading.Thread(target=web.serve_forever, daemon=True)
        thread.start()
        calls = [
            {
                "name": "web_fetch",
                "arguments": {"url": f"http://127.0.0.1:{web.server_port}/"},
            },
            {"name": "write_file", "arguments": {"path": "too-late", "content": "x"}},
        ]
        try:
            output = self.run_code(
                arguments={
                    "code": f"print(tools.parallel({json.dumps(calls)!r}))",
                    "timeout_ms": 1200,
                }
            )
            self.assertTrue(web.entered.is_set(), output)
            self.assertIn("timeout", output)
            self.assertFalse((self.workspace / "too-late").exists())
        finally:
            web.release.set()
            web.shutdown()
            thread.join(timeout=3)
            web.server_close()

    def assert_schema(self, wire):
        for body in self.server.bodies:
            entries = [t["function"] if wire == "chat" else t for t in body["tools"]]
            self.assertEqual([t["name"] for t in entries], ["run_code"])
            schema = entries[0]["parameters"]
            self.assertEqual(schema["required"], ["code"])
            self.assertEqual(schema["properties"]["code"]["type"], "string")
            self.assertEqual(schema["properties"]["timeout_ms"]["type"], "integer")
            self.assertEqual(schema["properties"]["timeout_ms"]["default"], 600000)
            self.assertEqual(schema["properties"]["timeout_ms"]["maximum"], 600000)
            self.assertFalse(schema.get("additionalProperties", True))

    def test_progressive_discovery_preserves_filtered_authority(self):
        for wire in ("responses", "chat"):
            with self.subTest(wire=wire):
                output = self.run_code(
                    "full = json.loads(tools.list())\n"
                    "names = {x['function']['name'] for x in full}\n"
                    "matches = json.loads(tools.search('terminal'))\n"
                    "assert matches['total'] >= 1\n"
                    "assert all(x['name'] in names for x in matches['tools'])\n"
                    "assert all(set(x) == {'name', 'description'} for x in matches['tools'])\n"
                    "assert all(len(x['description'].encode()) <= 240 for x in matches['tools'])\n"
                    "assert tools.describe('write_file') is None\n"
                    "assert json.loads(tools.search('write_file'))['total'] == 0\n"
                    "assert len(tools.search('terminal').encode()) < len(tools.list().encode())\n"
                    "print('discovery-keeps-authority')\n",
                    wire=wire,
                    env={"TNY_TOOLS": "terminal"},
                )
                self.assertEqual(output, "discovery-keeps-authority\n")
                self.assert_schema(wire)

    def test_binary_output_and_unicode_error_do_not_break_protocol(self):
        # Cover every lead byte with representative continuation, overlong,
        # surrogate, non-continuation and truncation boundaries. Expected text
        # comes from CPython's strict decoder, not a copy of the C classifier.
        wire_bytes = (
            b"".join(
                bytes((lead, tail, tail, tail, 124))
                for lead in range(256)
                for tail in (0, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC0)
            )
            + b"\xe2\x82"
        )
        expected = []
        index = 0
        while index < len(wire_bytes):
            for count in range(1, min(4, len(wire_bytes) - index) + 1):
                piece = wire_bytes[index : index + count]
                try:
                    character = piece.decode("utf-8", errors="strict")
                except UnicodeDecodeError:
                    continue
                if len(character) == 1 and character != "\0":
                    expected.append(character)
                    index += count
                    break
            else:
                expected.append("\ufffd")
                index += 1
        for wire in ("responses", "chat"):
            with self.subTest(wire=wire):
                output = self.run_code(
                    f"import os\nos.write(1, {wire_bytes!r})\n", wire=wire
                )
                self.assertEqual(output, "".join(expected))
                error = self.run_code("raise ValueError('\u20ac' * 12000)\n", wire=wire)
                self.assertTrue(
                    error.startswith("error: code: ValueError:"), error[:160]
                )
                self.assertNotIn("failed protocol/cleanup", error)
                error.encode("utf-8", errors="strict")

    def test_model_instructions_match_bundled_host_modules(self):
        for wire in ("responses", "chat"):
            with self.subTest(wire=wire):
                output = self.run_code(
                    "import sqlite3, ctypes, bz2, lzma, compression.zstd, urllib.request, ssl\n"
                    "assert sqlite3.connect(':memory:').execute('select 42').fetchone() == (42,)\n"
                    "assert bz2.decompress(bz2.compress(b'hello')) == b'hello'\n"
                    "assert lzma.decompress(lzma.compress(b'hello', preset=0)) == b'hello'\n"
                    "print('bundled-host-modules-ok')\n",
                    wire=wire,
                )
                self.assertEqual(output, "bundled-host-modules-ok\n")
                prompts = json.dumps(self.server.bodies, ensure_ascii=False)
                self.assertNotIn("sqlite3, ctypes, bz2 and lzma are not built", prompts)
                self.assertIn("sqlite3, ctypes, bz2, lzma", prompts)
                self.assertIn("Direct Python effects are not mediated", prompts)

    def test_both_wires_no_tool_stream(self):
        for wire in ("responses", "chat"):
            with self.subTest(wire=wire):
                process = self.start(name=None, wire=wire)
                stdout, stderr = process.communicate(timeout=15)
                self.assertEqual(process.returncode, 0, (stdout, stderr))
                self.assertIn("EXEC-OK", json.loads(stdout)["output"])
                self.assert_schema(wire)
                self.assertEqual(self.server.outputs, [])

    def test_both_wires_exact_schema_and_real_file_effect(self):
        for wire in ("responses", "chat"):
            with self.subTest(wire=wire):
                code = "\n".join(
                    [
                        'catalog = tools.list(); assert "read_file" in catalog',
                        'assert "content" in tools.describe("write_file")',
                        f'print(tools.call("write_file", {python_string(json.dumps({"path": wire + ".txt", "content": "written by code"}))}))',
                        f'print(tools.call("read_file", {python_string(json.dumps({"path": wire + ".txt"}))}))',
                    ]
                )
                result = self.run_code(code, wire=wire)
                self.assertIn("written by code", result)
                self.assertEqual(
                    (self.workspace / (wire + ".txt")).read_text(), "written by code"
                )
                self.assert_schema(wire)

    def test_optional_reasons_do_not_leak_into_nested_arguments(self):
        directory = self.home / ".tny/extensions"
        directory.mkdir(parents=True)
        log = self.home / "arguments.jsonl"
        (directory / "observe.py").write_text(
            "import json\nfrom tny_ext import PreToolUseEvent, ToolStartEvent\n"
            "def setup(api):\n"
            "    @api.on(PreToolUseEvent)\n"
            "    def before(event):\n"
            f"        with open({str(log)!r}, 'a') as out: out.write(json.dumps(dict(name=event.tool_name, arguments=event.arguments)) + '\\n')\n"
            "    @api.on(ToolStartEvent)\n"
            "    def started(event):\n"
            f"        with open({str(log)!r}, 'a') as out: out.write(json.dumps(dict(start=event.tool_name, reason=event.payload.get('reason'), detail=event.detail)) + '\\n')\n"
        )
        (self.workspace / "fixture").write_text("reason-fixture")
        for wire in ("responses", "chat"):
            for enabled in (True, False):
                with self.subTest(wire=wire, enabled=enabled):
                    (self.home / ".tny/settings.json").write_text(
                        json.dumps({"ui": {"tool_call_reasons": enabled}})
                    )
                    code = (
                        'print(tools.call("read_file", json.dumps({"path":"fixture"}), reason="Read fixture"))\n'
                        'print(tools.call("read_file", json.dumps({"path":"fixture"})))\n'
                        'print(tools.parallel(json.dumps([{"name":"read_file", "arguments":{"path":"fixture"}, "reason":"Read independently"}])))'
                    )
                    output = self.run_code(
                        arguments={"code": code, "reason": "Inspect fixtures"},
                        wire=wire,
                    )
                    self.assertIn("reason-fixture", output)
                    body = self.server.bodies[0]
                    tool = body["tools"][0]
                    schema = (tool["function"] if wire == "chat" else tool)[
                        "parameters"
                    ]
                    self.assertEqual("reason" in schema["properties"], enabled)
                    self.assertEqual(schema["required"], ["code"])
                    prompt = (
                        body["messages"][0]["content"]
                        if wire == "chat"
                        else body["instructions"]
                    )
                    self.assertEqual("For each tool call, include" in prompt, enabled)
        observed = [json.loads(line) for line in log.read_text().splitlines()]
        nested = [
            entry["arguments"] for entry in observed if entry.get("name") == "read_file"
        ]
        self.assertEqual(len(nested), 12)
        self.assertTrue(all(args == {"path": "fixture"} for args in nested), nested)
        starts = [entry for entry in observed if "start" in entry]
        self.assertEqual(
            [entry["reason"] for entry in starts],
            ["Inspect fixtures", "Read fixture", None, "Read independently"] * 4,
        )
        self.assertTrue(
            all(
                json.loads(entry["detail"]) == {"path": "fixture"}
                for entry in starts
                if entry["start"] == "read_file"
            )
        )

    def test_direct_calls_fail_closed_on_both_wires(self):
        for wire in ("responses", "chat"):
            for tool in ("write_file", "terminal", "unknown_tool"):
                with self.subTest(wire=wire, tool=tool):
                    output = self.run_code(
                        wire=wire,
                        name=tool,
                        arguments={
                            "path": "forbidden.txt",
                            "content": "bad",
                            "command": "touch forbidden.txt",
                        },
                    )
                    self.assertIn("error", output.lower())
                    self.assertFalse((self.workspace / "forbidden.txt").exists())
                    self.assert_schema(wire)

    def test_each_cell_has_fresh_python_state(self):
        self.server.sequence = [
            {"code": "previous_cell = 42; print(previous_cell)"},
            {
                "code": "try:\n    previous_cell\n"
                'except NameError:\n    print("fresh state")\n'
                'else:\n    raise AssertionError("previous cell state leaked")'
            },
        ]
        process = self.start()
        stdout, stderr = process.communicate(timeout=15)
        self.assertEqual(process.returncode, 0, (stdout, stderr))
        self.assertEqual(len(self.server.outputs), 2)
        self.assertIn("42", self.server.outputs[0])
        self.assertIn("fresh state", self.server.outputs[1])

    def test_direct_host_python_on_both_wires(self):
        # Direct Python acts on the host as the OS user (ADR 0180): the --cwd
        # workspace (tny itself runs elsewhere), the inherited environment,
        # files, subprocesses and loopback HTTP, with no tool mediation.
        web = ThreadingHTTPServer(("127.0.0.1", 0), Loopback)
        threading.Thread(target=web.serve_forever, daemon=True).start()
        self.addCleanup(web.server_close)
        self.addCleanup(web.shutdown)
        url = python_string(f"http://127.0.0.1:{web.server_port}/plain")
        for wire in ("responses", "chat"):
            with self.subTest(wire=wire):
                output = self.run_code(
                    f"""
import os, pathlib, subprocess, sys, urllib.request
print("cwd", os.getcwd() == {python_string(str(self.workspace.resolve()))})
pathlib.Path("{wire}.txt").write_text("direct write")
fd = os.open("{wire}-os.txt", os.O_WRONLY | os.O_CREAT)
os.close(fd)
print("env", os.environ.get("TNY_CELL_FIXTURE"), repr(sys.executable))
done = subprocess.run(["sh", "-c", "cat {wire}.txt; echo; echo inherited-output; exit 5"])
print("exit", done.returncode)
with urllib.request.urlopen({url}, timeout=5) as reply:
    print("http", reply.status, reply.read().decode())
value = json.loads('{{"value":42}}')
print(json.dumps({{"answer": value["value"] + 1}}))
""",
                    wire=wire,
                    env={"TNY_CELL_FIXTURE": "visible-to-python"},
                )
                self.assertIn("cwd True", output)
                self.assertIn("env visible-to-python ''", output)
                self.assertIn("direct write\ninherited-output\nexit 5", output)
                self.assertIn("http 200 loopback:/plain", output)
                self.assertIn('"answer":43', output.replace(" ", ""))
                self.assertEqual(
                    (self.workspace / f"{wire}.txt").read_text(), "direct write"
                )
                self.assertTrue((self.workspace / f"{wire}-os.txt").exists())

    def test_direct_https_verifies_certificates(self):
        openssl = shutil.which("openssl")
        if not openssl:
            self.skipTest(
                "openssl CLI unavailable to mint a throwaway loopback certificate"
            )
        cert, key = self.home / "loopback.pem", self.home / "loopback.key"
        mint_loopback_certificate(cert, key, "127.0.0.1", "IP:127.0.0.1", openssl)
        web = ThreadingHTTPServer(("127.0.0.1", 0), Loopback)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        web.socket = context.wrap_socket(web.socket, server_side=True)
        threading.Thread(target=web.serve_forever, daemon=True).start()
        self.addCleanup(web.server_close)
        self.addCleanup(web.shutdown)
        url = python_string(f"https://127.0.0.1:{web.server_port}/tls")
        output = self.run_code(
            f"""
import ssl, urllib.request
try:
    urllib.request.urlopen({url}, timeout=5)
except urllib.error.URLError as error:
    print("default verification", type(error.reason).__name__)
trusted = ssl.create_default_context(cafile={python_string(str(cert))})
with urllib.request.urlopen({url}, timeout=5, context=trusted) as reply:
    print("https", reply.status, reply.read().decode(), ssl.OPENSSL_VERSION.split()[1])
"""
        )
        self.assertIn("default verification SSLCertVerificationError", output)
        self.assertIn("https 200 loopback:/tls 3.5.8", output)

    def test_timeout_stops_python_descendants_and_keeps_output(self):
        pid_file = self.home / "sleep.pid"
        started = time.monotonic()
        output = self.run_code(
            arguments={
                "code": "import subprocess, time\n"
                "child = subprocess.Popen(['sleep', '30'])\n"
                f"open({python_string(str(pid_file))}, 'w').write(str(child.pid))\n"
                "print('started child', flush=True)\n"
                "time.sleep(60)\n",
                "timeout_ms": 1500,
            }
        )
        self.assertLess(time.monotonic() - started, 10)
        self.assertIn("timeout", output)
        self.assertIn("started child", output)
        pid = int(pid_file.read_text())
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)

    def test_profile_cannot_be_widened_by_code(self):
        output = self.run_code(
            'print(tools.call("write_file", \'{"path":"denied","content":"no"}\'))',
            env={"TNY_TOOLS": "terminal"},
        )
        self.assertIn("error", output.lower())
        self.assertFalse((self.workspace / "denied").exists())

    def test_runtime_errors_and_bounds(self):
        cases = [
            ({"code": "value ="}, "error"),
            ({"code": "while True:\n    pass", "timeout_ms": 40}, "error"),
            (
                {"code": 't = []\nwhile True:\n    t.append("x" * 100000)'},
                "error",
            ),
            (
                {
                    "code": "for _ in range(65):\n"
                    '    tools.call("list_files", \'{"path":"."}\')'
                },
                "error",
            ),
            ({"code": "print(1)", "timeout_ms": 600001}, "error"),
        ]
        for arguments, expected in cases:
            with self.subTest(arguments=arguments):
                started = time.monotonic()
                self.assertIn(expected, self.run_code(arguments=arguments).lower())
                self.assertLess(time.monotonic() - started, 8)
        # Excess output is summarized, not fatal: later statements still run.
        output = self.run_code('print("x" * 70000)\nprint("after")')
        self.assertLess(len(output.encode()), 70000)
        self.assertIn("bytes of output omitted", output)
        self.assertTrue(output.endswith("after\n"), output[-200:])

    def test_nested_permission_and_pretool_hooks_use_owner(self):
        directory = self.home / ".tny/extensions"
        directory.mkdir(parents=True)
        log = self.home / "hook.log"
        (directory / "guard.py").write_text(
            "from tny_ext import PreToolUseEvent, PermissionRequestEvent, deny_tool, decide_permission\n"
            "def setup(api):\n"
            "    @api.on(PreToolUseEvent)\n"
            "    def before(event):\n"
            f"        with open({str(log)!r}, 'a') as out: out.write(event.tool_name + '\\n')\n"
            "        if event.tool_name == 'delete_file': return deny_tool('nested hook denial')\n"
            "    @api.on(PermissionRequestEvent)\n"
            "    def permission(event): return decide_permission('allow_once', 'nested fixture approval')\n"
        )
        result = self.run_code(
            'print(tools.call("write_file", \'{"path":"protected.txt","content":"kept"}\'))\n'
            'print(tools.call("delete_file", \'{"path":"protected.txt"}\'))',
            flags=("--permission-mode", "ask"),
        )
        self.assertEqual((self.workspace / "protected.txt").read_text(), "kept")
        self.assertIn("denied", result)
        observed = log.read_text().splitlines()
        self.assertIn("write_file", observed)
        self.assertIn("delete_file", observed)

    def test_concurrent_contexts_do_not_share_profile_or_workspace(self):
        other = ExecutionCodeMode(methodName="test_profile_cannot_be_widened_by_code")
        other.setUp()
        try:
            code = 'print(tools.call("write_file", \'{"path":"same.txt","content":"owner only"}\'))'
            with ThreadPoolExecutor(max_workers=2) as pool:
                allowed = pool.submit(self.run_code, code)
                denied = pool.submit(
                    other.run_code, code, env={"TNY_TOOLS": "terminal"}
                )
                allowed.result(timeout=15)
                self.assertIn("error", denied.result(timeout=15).lower())
            self.assertEqual((self.workspace / "same.txt").read_text(), "owner only")
            self.assertFalse((other.workspace / "same.txt").exists())
        finally:
            other.doCleanups()

    def private_cell(
        self,
        code,
        *,
        timeout=150,
        permission=2,
        delay_state=0,
        delay_seconds=0.25,
        stop_control=0,
        prompt_gate=None,
    ):
        """Real exec-server with a trusted owner fixture delaying durable ACKs."""
        owner, peer = socket.socketpair()
        owner.settimeout(4)
        process = subprocess.Popen(
            [
                sys.executable,
                "-c",
                "import os,sys; os.dup2(int(sys.argv[1]),3); os.set_inheritable(3,True); "
                "os.execv(sys.argv[2],[sys.argv[2],'--exec-server'])",
                str(peer.fileno()),
                TNY,
            ],
            env=self.env,
            cwd=self.workspace,
            pass_fds=(peer.fileno(),),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        peer.close()
        state = {
            "session": None,
            "grants": [],
            "mem_results": [],
            "images": [],
            "learning": {
                "valid": False,
                "ok": False,
                "event": 0,
                "scope": 0,
                "intent": 0,
            },
            "pending_count": 0,
            "perm_blocked": False,
        }
        params = {
            "context": {
                "cwd": str(self.workspace),
                "tny_dir": str(self.home / ".tny"),
                "perm_mode": permission,
                "tool_profile": 0,
            },
            "state": state,
            "arguments": {"code": code, "timeout_ms": timeout},
            "prompt": prompt_gate is not None,
            "ask": False,
            "control": bool(stop_control),
            "pending_bytes": 0,
            "preview_ready": False,
            "preview_available": False,
            "cell_id": "0123456789abcdef",
            "session_sock": None,
            "session_id": None,
        }

        def send(value):
            data = json.dumps(dict(jsonrpc="2.0", version=1, **value)).encode()
            owner.sendall(struct.pack(">I", len(data)) + data)

        def receive_bytes(count):
            data = b""
            while len(data) < count:
                part = owner.recv(count - len(data))
                if not part:
                    raise EOFError("execution server disconnected before result")
                data += part
            return data

        observed = []
        states = 0
        prompts = 0
        try:
            send({"id": 1, "method": "execute", "params": params})
            while True:
                length = struct.unpack(">I", receive_bytes(4))[0]
                message = json.loads(receive_bytes(length))
                if "method" not in message:
                    self.assertEqual(message["id"], 1)
                    self.assertEqual(owner.recv(1), b"")
                    stdout, stderr = process.communicate(timeout=3)
                    self.assertEqual(process.returncode, 0, (stdout, stderr))
                    return message["result"]["output"], observed
                kind = message["method"]
                observed.append(kind)
                reply = {"ok": True, "pending_count": 0, "pending_bytes": 0}
                if kind == "state":
                    states += 1
                    if states == delay_state:
                        # An owner persistence/ACK delay, never extra tool authority.
                        (self.home / "ack-state.json").write_text(
                            json.dumps(message["params"])
                        )
                        time.sleep(delay_seconds)
                elif kind == "prompt":
                    prompts += 1
                    if prompt_gate is not None and prompts == 2:
                        time.sleep(
                            1.5
                        )  # Human time exceeds the original cell deadline.
                        prompt_gate.set()
                    reply = {"decision": 0 if prompt_gate is not None else 2}
                elif kind == "control":
                    reply = {
                        "arguments_json": None,
                        "result": None,
                        "extension": None,
                        "reason": None,
                        "permission": 0,
                        "deny": False,
                        "stop": message["params"]["kind"] == stop_control,
                        "result_replaced": False,
                        "result_is_error": False,
                    }
                else:
                    self.assertEqual(kind, "event")
                send({"id": message["id"], "result": reply})
        finally:
            owner.close()
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=5)

    def test_final_ack_has_separate_nonexecuting_budget(self):
        result, _ = self.private_cell(
            """print(tools.call("write_file", '{"path":"once.txt","content":"once"}')); """
            'print("settled")',
            delay_state=2,
        )
        self.assertIn("settled", result)
        self.assertNotIn("error", result)
        self.assertEqual((self.workspace / "once.txt").read_text(), "once")

    def test_final_ack_window_is_bounded_without_replay(self):
        started = time.monotonic()
        with self.assertRaises((BrokenPipeError, ConnectionResetError, EOFError)):
            self.private_cell(
                """tools.call("write_file", '{"path":"once.txt","content":"once"}')""",
                delay_state=2,
                delay_seconds=1.3,
            )
        self.assertLess(time.monotonic() - started, 3)
        self.assertEqual((self.workspace / "once.txt").read_text(), "once")

    def test_post_effect_ack_does_not_extend_execution_budget(self):
        result, _ = self.private_cell(
            """tools.call("write_file", '{"path":"once.txt","content":"once"}'); """
            """tools.call("write_file", '{"path":"late.txt","content":"forbidden"}')""",
            delay_state=1,
        )
        self.assertIn("timeout", result)
        self.assertNotIn("outcome unknown", result)
        self.assertEqual((self.workspace / "once.txt").read_text(), "once")
        self.assertFalse((self.workspace / "late.txt").exists())

    def test_no_prompt_owner_denies_without_phantom_request(self):
        result, observed = self.private_cell(
            """print(tools.call("write_file", '{"path":"denied.txt","content":"no"}'))""",
            timeout=1000,
            permission=0,
        )
        self.assertIn("permission", result)
        self.assertNotIn("prompt", observed)
        self.assertFalse((self.workspace / "denied.txt").exists())

    def test_owner_stop_interrupts_python_and_retains_completed_effect(self):
        # A controlled owner proves the returned diagnostic, including stop
        # before an effect and after an effect. The real extension follows.
        for kind in (1, 3):
            with self.subTest(control_kind=kind):
                result, _ = self.private_cell(
                    """tools.call("write_file", '{"path":"policy.txt","content":"once"}')\n"""
                    "while True:\n    pass",
                    timeout=5000,
                    stop_control=kind,
                )
                self.assertIn("cancelled by owner policy", result)
                self.assertNotIn("timeout", result)
                self.assertEqual((self.workspace / "policy.txt").exists(), kind == 3)
        directory = self.home / ".tny/extensions"
        directory.mkdir(parents=True)
        (directory / "stopper.py").write_text(
            "from tny_ext import PostToolUseEvent, stop\n"
            "def setup(api):\n"
            "    @api.on(PostToolUseEvent)\n"
            "    def after(event):\n"
            "        if event.tool_name == 'write_file': return stop('fixture policy stop')\n"
        )
        process = self.start(
            """tools.call("write_file", '{"path":"once.txt","content":"once"}')\n"""
            "while True:\n    pass"
        )
        started = time.monotonic()
        stdout, stderr = process.communicate(timeout=8)
        self.assertLess(time.monotonic() - started, 4)
        self.assertEqual((self.workspace / "once.txt").read_text(), "once")
        self.assertEqual(process.returncode, 130, (stdout, stderr))
        self.assertEqual(json.loads(stdout)["exit_code"], 130)
        self.assertEqual(len(self.server.bodies), 1)

    def test_private_entry_requires_connected_unix_stream(self):
        def reject(descriptor):
            for option in ("--exec-server", "--exec-command", "--exec-tool"):
                with self.subTest(option=option, descriptor=descriptor):
                    process = subprocess.run(
                        [
                            sys.executable,
                            "-c",
                            "import os,sys; os.dup2(int(sys.argv[1]),3); "
                            "os.set_inheritable(3,True); "
                            "os.execv(sys.argv[2],[sys.argv[2],sys.argv[3]])",
                            str(descriptor),
                            TNY,
                            option,
                        ],
                        env=self.env,
                        cwd=self.workspace,
                        pass_fds=(descriptor,),
                        capture_output=True,
                        timeout=3,
                    )
                    expected = 125 if option == "--exec-command" else 2
                    self.assertEqual(process.returncode, expected, process.stderr)
                    self.assertEqual(process.stdout, b"")

        read_fd, write_fd = os.pipe()
        try:
            reject(read_fd)
        finally:
            os.close(read_fd)
            os.close(write_fd)
        left, right = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        with left, right:
            reject(left.fileno())
        with socket.socket() as listener, socket.socket() as client:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            client.connect(listener.getsockname())
            server, _ = listener.accept()
            with server:
                reject(server.fileno())
        self.assertEqual(list(self.workspace.iterdir()), [])

    def test_private_protocol_rejects_malformed_unknown_and_eof(self):
        envelope = {
            "jsonrpc": "2.0",
            "version": 1,
            "id": 1,
            "method": "execute",
            "params": {},
        }
        encoded = json.dumps(envelope).encode()
        duplicate = encoded.replace(b'"id": 1', b'"id": 1, "id": 1')
        cases = [
            b"",
            b"\0\0",
            struct.pack(">I", 8 * 1024 * 1024 + 1),
            struct.pack(">I", 3) + b"bad",
            struct.pack(">I", len(duplicate)) + duplicate,
        ]
        for changes in (
            {"method": "unknown"},
            {"id": 0},
            {"id": 2},
            {"version": 2},
            {"extra": True},
            {"method": "state"},
        ):
            data = json.dumps(dict(envelope, **changes)).encode()
            cases.append(struct.pack(">I", len(data)) + data)
        for data in cases:
            with self.subTest(data=data[:120]):
                owner, peer = socket.socketpair()
                process = subprocess.Popen(
                    [
                        sys.executable,
                        "-c",
                        "import os,sys; os.dup2(int(sys.argv[1]),3); os.set_inheritable(3,True); os.execv(sys.argv[2],[sys.argv[2],'--exec-server'])",
                        str(peer.fileno()),
                        TNY,
                    ],
                    env=self.env,
                    pass_fds=(peer.fileno(),),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                peer.close()
                try:
                    owner.sendall(data)
                    owner.shutdown(socket.SHUT_WR)
                    process.communicate(timeout=3)
                    self.assertNotEqual(process.returncode, 0)
                    self.assertEqual(list(self.workspace.iterdir()), [])
                finally:
                    owner.close()
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=5)

    def test_execution_server_pid_and_crash_no_replay(self):
        process = self.start(
            'print(tools.call("terminal", \'{"command":"printf once >> effect; sleep 8"}\'))'
        )
        executor = None
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            rows = subprocess.run(
                ["ps", "-axo", "pid=,ppid=,args="],
                capture_output=True,
                text=True,
                check=True,
            ).stdout
            for row in rows.splitlines():
                parts = row.strip().split(None, 2)
                if (
                    len(parts) == 3
                    and int(parts[1]) == process.pid
                    and "--exec-server" in parts[2]
                ):
                    executor = int(parts[0])
            if executor and (self.workspace / "effect").exists():
                break
            time.sleep(0.02)
        try:
            self.assertIsNotNone(executor, "no production execution child observed")
            self.assertNotEqual(executor, process.pid)
            self.assertTrue((self.workspace / "effect").exists())
            os.kill(executor, signal.SIGKILL)
            stdout, stderr = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, (stdout, stderr))
            self.assertEqual((self.workspace / "effect").read_text(), "once")
            self.assertEqual(len(self.server.outputs), 1)
            self.assertIn("error", self.server.outputs[0].lower())
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)


@unittest.skipUnless(WASM, "wasm-only clean-error contract")
class WasmCodeMode(unittest.TestCase):
    setUp = ExecutionCodeMode.setUp
    close_server = ExecutionCodeMode.close_server
    start = ExecutionCodeMode.start
    run_code = ExecutionCodeMode.run_code
    assert_schema = ExecutionCodeMode.assert_schema

    def test_wasm_provider_streaming_and_explicit_execution_error(self):
        for wire in ("responses", "chat"):
            with self.subTest(wire=wire):
                process = self.start(name=None, wire=wire)
                stdout, stderr = process.communicate(timeout=20)
                self.assertEqual(process.returncode, 0, (stdout, stderr))
                self.assertIn("EXEC-OK", json.loads(stdout)["output"])
                self.assert_schema(wire)
                output = self.run_code('print("cannot execute")', wire=wire)
                self.assertIn("error", output.lower())
                self.assertIn("execution server unavailable on this platform", output)
                self.assertIn("no direct fallback", output)
                self.assertEqual(list(self.workspace.iterdir()), [])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
