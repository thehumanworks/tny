#!/usr/bin/env python3
"""Public C ABI refuses code execution before invoking a registered host tool."""

import ctypes
import json
import os
import platform
import subprocess
import sys
import time
import unittest
from pathlib import Path
from unittest.mock import patch

import test_execution_code_mode as fx
import test_libtny_acp as abi


class EmbeddedExecutionRefusal(unittest.TestCase):
    setUp = fx.ExecutionCodeMode.setUp
    close_server = fx.ExecutionCodeMode.close_server

    @classmethod
    def setUpClass(cls):
        configured = os.environ.get("TNY_LIBRARY")
        if not configured:
            subprocess.run(
                ["make", "-s", "lib-shared-active"],
                cwd=fx.ROOT,
                check=True,
                timeout=180,
            )
        suffix = "libtny.1.dylib" if platform.system() == "Darwin" else "libtny.so.1"
        cls.lib = abi.bind(Path(configured or fx.ROOT / "build/lib" / suffix))

    def exercise(self, provider):
        lib = self.lib
        self.server.call_name = "run_code"
        self.server.arguments = {"code": 'print(tools.call("host_echo", "{}"))'}
        state = self.home / "acp.json"
        env = dict(
            abi.clean_env(self.home),
            ACP_FIXTURE_STATE=str(state),
            ACP_FIXTURE_MCP="1",
            ACP_FIXTURE_CALLS=json.dumps([{"name": "host_echo", "arguments": {}}]),
        )
        keepalive, invocations, completed = [], [], []
        runtime, session, registration, error = (ctypes.c_void_p() for _ in range(4))

        def check(status):
            detail = ""
            if error.value:
                view = lib.tny_error_message(error)
                detail = ctypes.string_at(view.ptr, view.len).decode()
                lib.tny_error_free(error)
                error.value = None
            self.assertEqual(status, 0, detail)

        @abi.INVOKE
        def invoke(_user, _call, _generation, _arguments, _result):
            invocations.append(True)
            return -1

        with patch.dict(os.environ, env, clear=True):
            options = abi.RuntimeOptionsV0()
            check(
                lib.tny_runtime_options_init(
                    ctypes.byref(options), ctypes.sizeof(options)
                )
            )
            for field, value in (
                ("workspace", str(self.workspace)),
                ("provider", provider),
                ("model", "selected-model"),
                ("api_key", "synthetic-not-real"),
                ("base_url", f"http://127.0.0.1:{self.server.server_port}/v1"),
            ):
                raw, view = abi.byte_view(value)
                keepalive.append(raw)
                setattr(options, field, view)
            options.permission_mode = 2
            try:
                check(
                    lib.tny_runtime_create(
                        ctypes.byref(options),
                        ctypes.sizeof(options),
                        ctypes.byref(runtime),
                        ctypes.byref(error),
                    )
                )
                if provider == "acp":
                    raw, command = abi.byte_view(json.dumps([str(abi.AGENT)]))
                    check(
                        lib.tny_runtime_set_acp_command(
                            runtime, command, ctypes.byref(error)
                        )
                    )
                    keepalive.append(raw)
                spec = abi.ToolSpec()
                check(
                    lib.tny_tool_spec_v1_init(ctypes.byref(spec), ctypes.sizeof(spec))
                )
                for field, value in (
                    ("name", "host_echo"),
                    ("description", "Must never execute in the embedding process"),
                    ("input_schema_json", '{"type":"object","properties":{}}'),
                ):
                    raw, view = abi.byte_view(value)
                    keepalive.append(raw)
                    setattr(spec, field, view)
                spec.invoke = invoke
                check(
                    lib.tny_runtime_register_tool(
                        runtime,
                        ctypes.byref(spec),
                        ctypes.byref(registration),
                        ctypes.byref(error),
                    )
                )
                check(
                    lib.tny_session_create(
                        runtime, ctypes.byref(session), ctypes.byref(error)
                    )
                )
                raw, prompt = abi.byte_view("exercise the code execution boundary")
                check(lib.tny_session_send(session, prompt, ctypes.byref(error)))
                keepalive.append(raw)
                deadline, terminal = time.monotonic() + 15, False
                while time.monotonic() < deadline:
                    event = ctypes.c_void_p()
                    status = lib.tny_session_next_event(
                        session, 100, ctypes.byref(event), ctypes.byref(error)
                    )
                    if status == 3:
                        break
                    if status == 2:
                        continue
                    self.assertEqual(status, 1)
                    try:
                        view = abi.EventView()
                        check(
                            lib.tny_event_view_init(
                                ctypes.byref(view), ctypes.sizeof(view)
                            )
                        )
                        check(
                            lib.tny_event_read(
                                event, ctypes.byref(view), ctypes.sizeof(view)
                            )
                        )
                        if lib.tny_event_get_kind(event) == 3:
                            completed.append(
                                (
                                    ctypes.string_at(
                                        view.tool_name.ptr, view.tool_name.len
                                    ).decode(),
                                    ctypes.string_at(
                                        view.tool_detail.ptr, view.tool_detail.len
                                    ).decode(),
                                    lib.tny_event_tool_ok(event),
                                )
                            )
                        if lib.tny_event_get_kind(event) == 7:
                            terminal = True
                    finally:
                        lib.tny_event_free(event)
                self.assertTrue(terminal)
                self.assertEqual(invocations, [])
                self.assertTrue(completed, completed)
                self.assertTrue(
                    all(
                        name == "run_code"
                        and not ok
                        and "unavailable for embedded host/custom callbacks; no direct fallback"
                        in detail
                        for name, detail, ok in completed
                    ),
                    completed,
                )
            finally:
                if session.value:
                    lib.tny_session_free(session)
                if runtime.value:
                    lib.tny_runtime_free(runtime)
        if provider == "openai":
            self.assertEqual(
                {t["name"] for t in self.server.bodies[0]["tools"]}, {"run_code"}
            )

    def test_native_public_abi_refuses_host_callback(self):
        self.exercise("openai")

    def test_acp_public_abi_refuses_host_callback(self):
        self.exercise("acp")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
