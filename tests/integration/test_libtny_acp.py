#!/usr/bin/env python3
"""C ABI ACP command/event ownership and explicit embedded code refusal."""

from __future__ import annotations

import ctypes
import json
import os
import platform
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from unittest.mock import patch

from test_acp_client import AGENT, ROOT, TNY, clean_env
from test_libtny import EventView
from test_libtny_custom_tools import (
    INVOKE,
    RuntimeOptionsV0,
    TnyBytes,
    ToolResult,
    ToolSpec,
    byte_view,
)

CUSTOM_SCHEMA = {
    "type": "object",
    "properties": {"value": {"type": "string"}},
    "required": ["value"],
    "additionalProperties": False,
}


def bind(library):
    lib = ctypes.CDLL(str(library))
    signatures = {
        "tny_runtime_options_init": (
            [ctypes.POINTER(RuntimeOptionsV0), ctypes.c_uint64],
            ctypes.c_int32,
        ),
        "tny_runtime_create": (
            [
                ctypes.POINTER(RuntimeOptionsV0),
                ctypes.c_uint64,
                ctypes.POINTER(ctypes.c_void_p),
                ctypes.POINTER(ctypes.c_void_p),
            ],
            ctypes.c_int32,
        ),
        "tny_runtime_set_acp_command": (
            [ctypes.c_void_p, TnyBytes, ctypes.POINTER(ctypes.c_void_p)],
            ctypes.c_int32,
        ),
        "tny_runtime_register_tool": (
            [
                ctypes.c_void_p,
                ctypes.POINTER(ToolSpec),
                ctypes.POINTER(ctypes.c_void_p),
                ctypes.POINTER(ctypes.c_void_p),
            ],
            ctypes.c_int32,
        ),
        "tny_tool_spec_v1_init": (
            [ctypes.POINTER(ToolSpec), ctypes.c_uint64],
            ctypes.c_int32,
        ),
        "tny_tool_result_v1_init": (
            [ctypes.POINTER(ToolResult), ctypes.c_uint64],
            ctypes.c_int32,
        ),
        "tny_tool_call_complete": (
            [
                ctypes.c_void_p,
                ctypes.c_uint64,
                ctypes.POINTER(ToolResult),
                ctypes.POINTER(ctypes.c_void_p),
            ],
            ctypes.c_int32,
        ),
        "tny_tool_call_release": ([ctypes.c_void_p], None),
        "tny_session_create": (
            [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_void_p),
                ctypes.POINTER(ctypes.c_void_p),
            ],
            ctypes.c_int32,
        ),
        "tny_session_send": (
            [ctypes.c_void_p, TnyBytes, ctypes.POINTER(ctypes.c_void_p)],
            ctypes.c_int32,
        ),
        "tny_session_next_event": (
            [
                ctypes.c_void_p,
                ctypes.c_uint32,
                ctypes.POINTER(ctypes.c_void_p),
                ctypes.POINTER(ctypes.c_void_p),
            ],
            ctypes.c_int32,
        ),
        "tny_session_cancel": (
            [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)],
            ctypes.c_int32,
        ),
        "tny_event_get_kind": ([ctypes.c_void_p], ctypes.c_uint32),
        "tny_event_stop_reason": ([ctypes.c_void_p], ctypes.c_uint32),
        "tny_event_tool_ok": ([ctypes.c_void_p], ctypes.c_uint32),
        "tny_event_tool_name": ([ctypes.c_void_p], TnyBytes),
        "tny_event_tool_detail": ([ctypes.c_void_p], TnyBytes),
        "tny_event_error_code": ([ctypes.c_void_p], ctypes.c_int32),
        "tny_event_text": ([ctypes.c_void_p], TnyBytes),
        "tny_event_cost_currency": ([ctypes.c_void_p], TnyBytes),
        "tny_event_cost_cumulative": ([ctypes.c_void_p], ctypes.c_uint32),
        "tny_event_tokens_reported": ([ctypes.c_void_p], ctypes.c_uint32),
        "tny_event_view_init": (
            [ctypes.POINTER(EventView), ctypes.c_uint64],
            ctypes.c_int32,
        ),
        "tny_event_read": (
            [ctypes.c_void_p, ctypes.POINTER(EventView), ctypes.c_uint64],
            ctypes.c_int32,
        ),
        "tny_event_free": ([ctypes.c_void_p], None),
        "tny_session_free": ([ctypes.c_void_p], None),
        "tny_runtime_free": ([ctypes.c_void_p], None),
        "tny_error_message": ([ctypes.c_void_p], TnyBytes),
        "tny_error_free": ([ctypes.c_void_p], None),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(lib, name)
        function.argtypes = arguments
        function.restype = result
    return lib


def run_case(
    lib, root, asynchronous, cancel=False, tool_error=False, oom=False, guarded=False
):
    if oom:
        for name in ("settlement_count", "settlement_allocations"):
            function = getattr(lib, "tny_alloc_test_" + name)
            function.argtypes = []
            function.restype = ctypes.c_size_t
        lib.tny_alloc_test_scope_injected.argtypes = []
        lib.tny_alloc_test_scope_injected.restype = ctypes.c_bool
    home, workspace = root / "home", root / "workspace"
    (home / ".tny").mkdir(parents=True)
    workspace.mkdir()
    (home / ".tny/settings.json").write_text("{}")
    state = root / "state.json"
    env = dict(
        clean_env(home),
        ACP_FIXTURE_STATE=str(state),
        ACP_FIXTURE_MCP="1",
        ACP_FIXTURE_CALLS=json.dumps(
            [{"name": "host_echo", "arguments": {"value": "hello"}}]
        ),
        ACP_FIXTURE_USAGE=json.dumps(
            [
                {"used": 7, "size": 100, "cost": {"amount": 0.1, "currency": "EUR"}},
                {"used": 9, "size": 100, "cost": {"amount": 0.25, "currency": "EUR"}},
            ]
        ),
    )
    if cancel:
        # Cancellation/OOM remain actual active-provider tests. No model-owned
        # custom callback can be pending under the embedded refusal contract.
        env["ACP_FIXTURE_MODE"] = "cancel"
        env["ACP_FIXTURE_CALLS"] = "[]"
    if guarded:
        env.update(
            TNY_ACP_REQUIRE_TOOLS_AUTHORITY="1",
            ACP_FIXTURE_NAME="@agentclientprotocol/claude-agent-acp",
            ACP_FIXTURE_VERSION="0.75.1",
        )
    with patch.dict(os.environ, env, clear=True):
        options = RuntimeOptionsV0()
        assert (
            lib.tny_runtime_options_init(ctypes.byref(options), ctypes.sizeof(options))
            == 0
        )
        keepalive = []
        for field, value in (
            ("workspace", str(workspace)),
            ("provider", "acp"),
            ("model", "selected-model"),
        ):
            raw, view = byte_view(value)
            keepalive.append(raw)
            setattr(options, field, view)
        options.permission_mode = 2
        runtime, session, registration, error = (ctypes.c_void_p() for _ in range(4))
        threads = []
        completions = []
        release = threading.Event()
        invocations = []
        callback_errors = []
        oom_errors = []
        retained_usage = []
        injected = False
        result_bytes = b"ASYNC-HOST-OK" if asynchronous else b"SYNC-HOST-OK"

        def check(status):
            if status != 0:
                detail = ""
                if error.value:
                    view = lib.tny_error_message(error)
                    detail = ctypes.string_at(view.ptr, view.len).decode(
                        "utf-8", "replace"
                    )
                    lib.tny_error_free(error)
                    error.value = None
                raise AssertionError(f"C ABI status {status}: {detail}")

        def complete(call, generation):
            if cancel:
                assert release.wait(10), "cancel fixture completion was not released"
            else:
                time.sleep(0.04)
            result = ToolResult()
            lib.tny_tool_result_v1_init(ctypes.byref(result), ctypes.sizeof(result))
            result.data = TnyBytes(result_bytes, len(result_bytes))
            result.is_error = int(tool_error)
            completions.append(
                lib.tny_tool_call_complete(call, generation, ctypes.byref(result), None)
            )
            lib.tny_tool_call_release(call)

        @INVOKE
        def invoke(_user, call, generation, arguments, result):
            try:
                invocations.append(
                    json.loads(ctypes.string_at(arguments.ptr, arguments.len))
                )
                if asynchronous:
                    thread = threading.Thread(target=complete, args=(call, generation))
                    threads.append(thread)
                    thread.start()
                    return 1
                lib.tny_tool_result_v1_init(result, ctypes.sizeof(result.contents))
                result.contents.data = TnyBytes(result_bytes, len(result_bytes))
                result.contents.is_error = int(tool_error)
                return 0
            except Exception as exc:
                callback_errors.append(str(exc))
                return -1

        try:
            check(
                lib.tny_runtime_create(
                    ctypes.byref(options),
                    ctypes.sizeof(options),
                    ctypes.byref(runtime),
                    ctypes.byref(error),
                )
            )
            # Setter copies argv; caller storage is destroyed before the turn.
            encoded = json.dumps([str(AGENT), "SDK argument with spaces", ""]).encode()
            buffer = ctypes.create_string_buffer(encoded)
            check(
                lib.tny_runtime_set_acp_command(
                    runtime,
                    TnyBytes(ctypes.cast(buffer, ctypes.c_char_p), len(encoded)),
                    ctypes.byref(error),
                )
            )
            ctypes.memset(buffer, ord("X"), len(encoded))
            # Rejected replacements leave the previous copied argv intact.
            for invalid in (
                "[]",
                '[""]',
                '["valid", 3]',
                '["valid", "bad\\u0000argument"]',
            ):
                raw, view = byte_view(invalid)
                assert lib.tny_runtime_set_acp_command(runtime, view, None) == -1, (
                    invalid
                )
                assert raw
            spec = ToolSpec()
            check(lib.tny_tool_spec_v1_init(ctypes.byref(spec), ctypes.sizeof(spec)))
            for field, value in (
                ("name", "host_echo"),
                ("description", "ACP host callback fixture"),
                ("input_schema_json", json.dumps(CUSTOM_SCHEMA)),
            ):
                raw, view = byte_view(value)
                keepalive.append(raw)
                setattr(spec, field, view)
            spec.invoke = invoke
            spec.max_argument_bytes = spec.max_result_bytes = 1024
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
            assert not state.exists(), "runtime/session construction spawned ACP"
            for turn in range(1 if cancel else 2):
                raw, prompt = byte_view(f"custom fixture {turn}")
                check(lib.tny_session_send(session, prompt, ctypes.byref(error)))
                assert raw
                terminal, tool_ends = 0, 0
                cancel_ready = False
                cancelled = False
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    if cancel and cancel_ready and not cancelled:
                        if oom:
                            os.environ["TNY_TEST_ALLOC_SCOPE"] = "next_event"
                            os.environ["TNY_TEST_ALLOC_FAIL_AT"] = "1"
                            release.set()
                        check(lib.tny_session_cancel(session, ctypes.byref(error)))
                        cancelled = True
                    event = ctypes.c_void_p()
                    status = lib.tny_session_next_event(
                        session, 100, ctypes.byref(event), ctypes.byref(error)
                    )
                    if oom and lib.tny_alloc_test_scope_injected():
                        injected = True
                        assert lib.tny_alloc_test_settlement_count() >= 1
                        assert lib.tny_alloc_test_settlement_allocations() == 0
                        os.environ["TNY_TEST_ALLOC_SCOPE"] = "disabled"
                    if status == 3:
                        break
                    if status == 2:
                        continue
                    assert status == 1 and event.value, f"next_event status {status}"
                    kind = lib.tny_event_get_kind(event)
                    try:
                        if kind == 7:
                            terminal += 1
                            stop = lib.tny_event_stop_reason(event)
                            assert stop == (4 if oom else 1 if cancel else 0), (
                                "stop",
                                stop,
                                "cancel",
                                cancel,
                                "invocations",
                                invocations,
                            )
                        elif kind == 3:
                            tool_ends += 1
                            if not cancel:
                                name = lib.tny_event_tool_name(event)
                                detail = lib.tny_event_tool_detail(event)
                                assert (
                                    ctypes.string_at(name.ptr, name.len) == b"run_code"
                                )
                                refusal = ctypes.string_at(detail.ptr, detail.len)
                                assert not lib.tny_event_tool_ok(event)
                                assert b"execution server unavailable" in refusal, (
                                    refusal
                                )
                                assert b"no direct fallback" in refusal, refusal
                        elif kind == 0:
                            text = lib.tny_event_text(event)
                            cancel_ready |= b"CANCEL-READY" in ctypes.string_at(
                                text.ptr, text.len
                            )
                        elif kind == 6:
                            retained_usage.append(event)
                            event = None
                        elif kind == 8:
                            if oom:
                                oom_errors.append(lib.tny_event_error_code(event))
                                continue
                            text = lib.tny_event_text(event)
                            raise AssertionError(ctypes.string_at(text.ptr, text.len))
                    finally:
                        if event is not None:
                            lib.tny_event_free(event)
                assert terminal == 1, terminal
                if not cancel:
                    assert tool_ends == 1, tool_ends
                if oom:
                    assert injected and oom_errors == [-4], (injected, oom_errors)
                    assert tool_ends == 0, "OOM settlement emitted post-tool callbacks"
                release.set()
            for thread in threads:
                thread.join(timeout=2)
                assert not thread.is_alive()
            assert not callback_errors, callback_errors
            assert invocations == [] and completions == [] and threads == [], (
                invocations,
                completions,
                threads,
            )
            facts = json.loads(state.read_text())
            assert facts["argv"] == ["SDK argument with spaces", ""]
            assert facts["model_at_prompt"] == "selected-model"
            assert [tool["name"] for tool in facts["tools"]] == ["run_code"], facts[
                "tools"
            ]
            if not cancel:
                results = facts["tool_results"]
                assert results and results[0]["result"].get("isError"), results
                assert "execution server unavailable" in json.dumps(results), results
                assert "no direct fallback" in json.dumps(results), results
            assert list(workspace.iterdir()) == [], (
                "refused model call changed workspace"
            )

        finally:
            release.set()
            if session.value:
                lib.tny_session_free(session)
            for thread in threads:
                thread.join(timeout=2)
            if runtime.value:
                lib.tny_runtime_free(runtime)
            # A popped event owns its currency after later updates, turns and
            # runtime destruction; the public frozen view remains unchanged.
            try:
                for index, event in enumerate(retained_usage):
                    view = EventView()
                    assert (
                        lib.tny_event_view_init(ctypes.byref(view), ctypes.sizeof(view))
                        == 0
                    )
                    assert (
                        lib.tny_event_read(
                            event, ctypes.byref(view), ctypes.sizeof(view)
                        )
                        == 0
                    )
                    currency = lib.tny_event_cost_currency(event)
                    assert ctypes.string_at(currency.ptr, currency.len) == b"EUR"
                    assert lib.tny_event_cost_cumulative(event) == 1
                    assert lib.tny_event_tokens_reported(event) == 0
                    assert view.has_cost == 1
                    assert view.cost == (0.1, 0.25)[index % 2]
                    assert (view.input_tokens, view.output_tokens) == (0, 0)
                    assert view.context_used == (7, 9)[index % 2]
                if not cancel:
                    assert len(retained_usage) == 4, len(retained_usage)
            finally:
                for event in retained_usage:
                    lib.tny_event_free(event)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--fault-library":
        lib = bind(Path(sys.argv[2]).resolve(strict=True))
        with tempfile.TemporaryDirectory(prefix="tny-acp-oom-") as temporary:
            run_case(lib, Path(temporary) / "plain", True, cancel=True, oom=True)
            run_case(
                lib,
                Path(temporary) / "guarded",
                True,
                cancel=True,
                oom=True,
                guarded=True,
            )
        print(
            "PASS plain/guarded ACP active-turn OOM settles without allocation or host callback effects"
        )
        return
    suffix = "libtny.1.dylib" if platform.system() == "Darwin" else "libtny.so.1"
    configured = os.environ.get("TNY_LIBRARY")
    if not configured:
        subprocess.run(["make", "lib-shared-active"], cwd=ROOT, check=True, timeout=180)
    library = Path(configured) if configured else ROOT / "build/lib" / suffix
    assert TNY.is_file()
    lib = bind(library.resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="tny-lib-acp-") as temporary:
        for asynchronous in (False, True):
            root = Path(temporary) / ("async" if asynchronous else "sync")
            run_case(lib, root, asynchronous)
        run_case(lib, Path(temporary) / "cancel", True, cancel=True)
        run_case(lib, Path(temporary) / "sync-error", False, tool_error=True)
        run_case(lib, Path(temporary) / "async-error", True, tool_error=True)
    print(
        "PASS libtny ACP copied argv, code-only embedded refusal, cancellation and retained usage across repeated turns"
    )


if __name__ == "__main__":
    main()
