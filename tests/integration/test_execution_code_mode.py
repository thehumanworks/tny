#!/usr/bin/env python3
"""Execution code mode through the production binary and local provider wires.

Synthetic credentials, private HOME/workspace, real process and file effects.
Raw wire fixtures deliberately bypass shared fixture conversion for direct-call
rejection checks. No live inference, external account, or executor mock.
"""

from __future__ import annotations

import json
import os
import signal
import socket
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

from code_mode_fixture import lua_string

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

    def assert_schema(self, wire):
        for body in self.server.bodies:
            entries = [t["function"] if wire == "chat" else t for t in body["tools"]]
            self.assertEqual([t["name"] for t in entries], ["run_code"])
            schema = entries[0]["parameters"]
            self.assertEqual(schema["required"], ["code"])
            self.assertEqual(schema["properties"]["code"]["type"], "string")
            self.assertEqual(schema["properties"]["timeout_ms"]["type"], "integer")
            self.assertFalse(schema.get("additionalProperties", True))

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
                        'local catalog = tools.list(); assert(string.find(catalog, "read_file"))',
                        'assert(string.find(tools.describe("write_file"), "content"))',
                        f'print(tools.call("write_file", {lua_string(json.dumps({"path": wire + ".txt", "content": "written by code"}))}))',
                        f'print(tools.call("read_file", {lua_string(json.dumps({"path": wire + ".txt"}))}))',
                    ]
                )
                result = self.run_code(code, wire=wire)
                self.assertIn("written by code", result)
                self.assertEqual(
                    (self.workspace / (wire + ".txt")).read_text(), "written by code"
                )
                self.assert_schema(wire)

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

    def test_each_cell_has_fresh_lua_state(self):
        self.server.sequence = [
            {"code": "previous_cell = 42; print(previous_cell)"},
            {"code": 'assert(previous_cell == nil); print("fresh state")'},
        ]
        process = self.start()
        stdout, stderr = process.communicate(timeout=15)
        self.assertEqual(process.returncode, 0, (stdout, stderr))
        self.assertEqual(len(self.server.outputs), 2)
        self.assertIn("42", self.server.outputs[0])
        self.assertIn("fresh state", self.server.outputs[1])

    def test_restricted_lua_and_json(self):
        output = self.run_code("""
assert(io == nil and os == nil and package == nil and debug == nil)
assert(require == nil and dofile == nil and loadfile == nil and load == nil)
local value = json.decode('{"value":42}')
print(json.encode({answer = value.value + 1}))
""")
        self.assertIn('"answer":43', output.replace(" ", ""))

    def test_profile_cannot_be_widened_by_code(self):
        output = self.run_code(
            'print(tools.call("write_file", \'{"path":"denied","content":"no"}\'))',
            env={"TNY_TOOLS": "terminal"},
        )
        self.assertIn("error", output.lower())
        self.assertFalse((self.workspace / "denied").exists())

    def test_runtime_errors_and_bounds(self):
        cases = [
            ({"code": "local ="}, "error"),
            ({"code": "while true do end", "timeout_ms": 40}, "error"),
            ({"code": 'print(string.rep("x", 70000))'}, "error"),
            (
                {
                    "code": 'local t = {}; while true do t[#t+1] = string.rep("x", 100000) end'
                },
                "error",
            ),
            (
                {
                    "code": 'for i=1,65 do tools.call("list_files", \'{"path":"."}\') end'
                },
                "error",
            ),
            ({"code": "print(1)", "timeout_ms": 30001}, "error"),
        ]
        for arguments, expected in cases:
            with self.subTest(arguments=arguments):
                started = time.monotonic()
                self.assertIn(expected, self.run_code(arguments=arguments).lower())
                self.assertLess(time.monotonic() - started, 8)

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
