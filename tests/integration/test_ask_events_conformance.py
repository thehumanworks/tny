#!/usr/bin/env python3
"""One provider response, two public readers.

`tny ask --events=jsonl` and libtny's own event readers must describe the same
turn identically: same order, same numeric kinds and stop reason, same payload
values, same empty-versus-absent choices. A schema file cannot prove that, so
this drives the real CLI and the real shared library against one deterministic
fixture and compares the results field by field (ADR 0030, ADR 0090).

Both libtny reader boundaries are exercised: the tny_event_read view and the
individual tny_event_* getters.
"""

from __future__ import annotations

import ctypes
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

# The very same loopback fixture the CLI suite uses; one scenario source.
from test_ask_events import (  # noqa: E402
    Fail,
    Workspace,
    check,
    parse_stream,
    start_fixture,
    stop_fixture,
)

TNY = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", "build/tny")
TNY = os.path.abspath(TNY)
TURN_TIMEOUT_MS = 20000

STATUS_OK, STATUS_EVENT, STATUS_TIMEOUT, STATUS_DRAINED = 0, 1, 2, 3


class TnyBytes(ctypes.Structure):
    _fields_ = [("ptr", ctypes.c_char_p), ("len", ctypes.c_uint64)]


class RuntimeOptions(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("permission_mode", ctypes.c_uint32),
        ("persistence", ctypes.c_uint32),
        ("max_steps", ctypes.c_uint32),
        ("max_tool_result_bytes", ctypes.c_uint64),
        ("workspace", TnyBytes),
        ("state_dir", TnyBytes),
        ("provider", TnyBytes),
        ("model", TnyBytes),
        ("base_url", TnyBytes),
        ("api_key", TnyBytes),
        ("wire_api", TnyBytes),
        ("reserved", ctypes.c_uint64 * 8),
    ]


class EventView(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("kind", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("tool_ok", ctypes.c_uint32),
        ("permission_options", ctypes.c_uint32),
        ("stop_reason", ctypes.c_uint32),
        ("error_code", ctypes.c_int32),
        ("has_cost", ctypes.c_uint32),
        ("sequence", ctypes.c_uint64),
        ("timestamp_ms", ctypes.c_int64),
        ("input_tokens", ctypes.c_int64),
        ("output_tokens", ctypes.c_int64),
        ("context_used", ctypes.c_int64),
        ("context_size", ctypes.c_int64),
        ("cost", ctypes.c_double),
        ("provider", TnyBytes),
        ("session_id", TnyBytes),
        ("turn_id", TnyBytes),
        ("text", TnyBytes),
        ("message_id", TnyBytes),
        ("tool_name", TnyBytes),
        ("tool_id", TnyBytes),
        ("tool_detail", TnyBytes),
        ("permission_id", TnyBytes),
        ("permission_summary", TnyBytes),
        ("message_type", TnyBytes),
        ("reserved", ctypes.c_uint64 * 8),
    ]


def as_bytes(value):
    raw = value.encode()
    return raw, TnyBytes(raw, len(raw))


def view_bytes(value: TnyBytes) -> str:
    if not value.ptr or not value.len:
        return ""
    return ctypes.string_at(value.ptr, value.len).decode("utf-8", "replace")


def load_library():
    if sys.platform not in ("darwin", "linux"):
        return None
    subprocess.run(
        ["make", "lib-shared-active"],
        cwd=ROOT,
        check=True,
        stdout=subprocess.DEVNULL,
        timeout=600,
    )
    name = "libtny.1.dylib" if sys.platform == "darwin" else "libtny.so.1"
    lib = ctypes.CDLL(os.path.join(ROOT, "build", "lib", name))
    lib.tny_runtime_options_init.argtypes = [
        ctypes.POINTER(RuntimeOptions),
        ctypes.c_uint64,
    ]
    lib.tny_runtime_options_init.restype = ctypes.c_int32
    lib.tny_runtime_create.argtypes = [
        ctypes.POINTER(RuntimeOptions),
        ctypes.c_uint64,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_runtime_create.restype = ctypes.c_int32
    lib.tny_session_create.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_session_create.restype = ctypes.c_int32
    lib.tny_session_send.argtypes = [
        ctypes.c_void_p,
        TnyBytes,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_session_send.restype = ctypes.c_int32
    lib.tny_session_next_event.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_session_next_event.restype = ctypes.c_int32
    lib.tny_event_view_init.argtypes = [ctypes.POINTER(EventView), ctypes.c_uint64]
    lib.tny_event_view_init.restype = ctypes.c_int32
    lib.tny_event_read.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(EventView),
        ctypes.c_uint64,
    ]
    lib.tny_event_read.restype = ctypes.c_int32
    for name_, restype in (
        ("tny_event_get_kind", ctypes.c_uint32),
        ("tny_event_tool_ok", ctypes.c_uint32),
        ("tny_event_permission_options", ctypes.c_uint32),
        ("tny_event_stop_reason", ctypes.c_uint32),
        ("tny_event_error_code", ctypes.c_int32),
        ("tny_event_input_tokens", ctypes.c_int64),
        ("tny_event_output_tokens", ctypes.c_int64),
    ):
        getattr(lib, name_).argtypes = [ctypes.c_void_p]
        getattr(lib, name_).restype = restype
    for name_ in (
        "tny_event_text",
        "tny_event_tool_name",
        "tny_event_tool_id",
        "tny_event_tool_detail",
        "tny_event_permission_id",
        "tny_event_permission_summary",
        "tny_event_message_type",
    ):
        getattr(lib, name_).argtypes = [ctypes.c_void_p]
        getattr(lib, name_).restype = TnyBytes
    lib.tny_event_free.argtypes = [ctypes.c_void_p]
    lib.tny_event_free.restype = None
    lib.tny_session_free.argtypes = [ctypes.c_void_p]
    lib.tny_session_free.restype = None
    lib.tny_runtime_free.argtypes = [ctypes.c_void_p]
    lib.tny_runtime_free.restype = None
    lib.tny_error_message.argtypes = [ctypes.c_void_p]
    lib.tny_error_message.restype = TnyBytes
    lib.tny_error_free.argtypes = [ctypes.c_void_p]
    lib.tny_error_free.restype = None
    return lib


def library_events(lib, base_url, workspace, prompt):
    """Read one turn through both public reader boundaries."""
    options = RuntimeOptions()
    check(
        lib.tny_runtime_options_init(ctypes.byref(options), ctypes.sizeof(options))
        == 0,
        "init",
    )
    options.persistence = 0
    keep = []
    for field, value in (
        ("workspace", workspace),
        ("base_url", base_url),
        ("api_key", "integration-test-not-real"),
        ("model", "mock-model"),
        ("wire_api", "chat"),
    ):
        raw, view = as_bytes(value)
        keep.append(raw)
        setattr(options, field, view)
    runtime = ctypes.c_void_p()
    error = ctypes.c_void_p()
    status = lib.tny_runtime_create(
        ctypes.byref(options),
        ctypes.sizeof(options),
        ctypes.byref(runtime),
        ctypes.byref(error),
    )
    check(
        status == 0,
        f"runtime_create: {status} {view_bytes(lib.tny_error_message(error))}",
    )
    session = ctypes.c_void_p()
    check(
        lib.tny_session_create(runtime, ctypes.byref(session), ctypes.byref(error))
        == 0,
        "create",
    )
    raw, view = as_bytes(prompt)
    check(lib.tny_session_send(session, view, ctypes.byref(error)) == 0, "send")
    events = []
    while True:
        handle = ctypes.c_void_p()
        status = lib.tny_session_next_event(
            session, TURN_TIMEOUT_MS, ctypes.byref(handle), ctypes.byref(error)
        )
        if status == STATUS_DRAINED:
            break
        check(status == STATUS_EVENT, f"next_event status {status}")
        snapshot = EventView()
        check(
            lib.tny_event_view_init(ctypes.byref(snapshot), ctypes.sizeof(snapshot))
            == 0,
            "view_init",
        )
        check(
            lib.tny_event_read(handle, ctypes.byref(snapshot), ctypes.sizeof(snapshot))
            == 0,
            "event_read",
        )
        # Boundary two: the individual getters must agree with the snapshot.
        check(lib.tny_event_get_kind(handle) == snapshot.kind, "kind getter disagrees")
        check(
            view_bytes(lib.tny_event_text(handle)) == view_bytes(snapshot.text),
            "text getter",
        )
        check(
            view_bytes(lib.tny_event_tool_name(handle))
            == view_bytes(snapshot.tool_name),
            "tool_name getter",
        )
        check(
            view_bytes(lib.tny_event_tool_id(handle)) == view_bytes(snapshot.tool_id),
            "tool_id getter",
        )
        check(lib.tny_event_tool_ok(handle) == snapshot.tool_ok, "tool_ok getter")
        check(
            lib.tny_event_stop_reason(handle) == snapshot.stop_reason,
            "stop_reason getter",
        )
        check(
            lib.tny_event_error_code(handle) == snapshot.error_code, "error_code getter"
        )
        check(
            lib.tny_event_input_tokens(handle) == snapshot.input_tokens,
            "input_tokens getter",
        )
        check(
            lib.tny_event_output_tokens(handle) == snapshot.output_tokens,
            "output_tokens getter",
        )
        check(
            lib.tny_event_permission_options(handle) == snapshot.permission_options,
            "permission_options getter",
        )
        events.append(
            {
                "schema_version": int(snapshot.schema_version),
                "kind": int(snapshot.kind),
                "sequence": int(snapshot.sequence),
                "provider": view_bytes(snapshot.provider),
                "text": view_bytes(snapshot.text),
                "message_id": view_bytes(snapshot.message_id),
                "tool_name": view_bytes(snapshot.tool_name),
                "tool_id": view_bytes(snapshot.tool_id),
                "tool_detail": view_bytes(snapshot.tool_detail),
                "tool_ok": bool(snapshot.tool_ok),
                "permission_id": view_bytes(snapshot.permission_id),
                "permission_summary": view_bytes(snapshot.permission_summary),
                "permission_options": int(snapshot.permission_options),
                "message_type": view_bytes(snapshot.message_type),
                "input_tokens": int(snapshot.input_tokens),
                "output_tokens": int(snapshot.output_tokens),
                "context_used": int(snapshot.context_used),
                "context_size": int(snapshot.context_size),
                "cost": float(snapshot.cost) if snapshot.has_cost else None,
                "has_cost": bool(snapshot.has_cost),
                "stop_reason": int(snapshot.stop_reason),
                "error_code": int(snapshot.error_code),
            }
        )
        lib.tny_event_free(handle)
    lib.tny_session_free(session)
    lib.tny_runtime_free(runtime)
    return events


PAYLOAD_KEYS = {
    0: ["text", "message_id"],
    2: ["tool_name", "tool_id", "tool_detail"],
    3: ["tool_name", "tool_id", "tool_detail", "tool_ok"],
    4: ["permission_id", "permission_summary", "permission_options"],
    6: [
        "input_tokens",
        "output_tokens",
        "context_used",
        "context_size",
        "cost",
        "has_cost",
    ],
    7: ["stop_reason"],
    8: ["text", "error_code"],
    11: ["text", "message_id", "message_type"],
}


def compare(cli_events, lib_events, label):
    check(
        [e["kind"] for e in cli_events] == [e["kind"] for e in lib_events],
        f"{label}: kind order differs\n  cli={[e['kind'] for e in cli_events]}\n"
        f"  lib={[e['kind'] for e in lib_events]}",
    )
    for index, (cli, lib) in enumerate(zip(cli_events, lib_events)):
        check(
            cli["schema_version"] == lib["schema_version"],
            f"{label}[{index}] schema version",
        )
        check(cli["provider"] == lib["provider"], f"{label}[{index}] provider")
        for key in PAYLOAD_KEYS.get(cli["kind"], []):
            check(
                cli[key] == lib[key],
                f"{label}[{index}] {key}: cli={cli[key]!r} libtny={lib[key]!r}",
            )
    # Sequence numbers are session-local but must rise the same way.
    for events, who in ((cli_events, "cli"), (lib_events, "libtny")):
        rising = all(b["sequence"] > a["sequence"] for a, b in zip(events, events[1:]))
        check(rising, f"{label}: {who} sequence is not monotonic")


def run_case(lib, tmp, label, prompt, status, expect_exit):
    server, thread = start_fixture(status=status)
    try:
        port = server.server_address[1]
        ws = Workspace(os.path.join(tmp, label), port)
        run = ws.run("ask", "--ephemeral", "--events=jsonl", prompt)
        check(
            run.returncode == expect_exit,
            f"{label}: exit {run.returncode}: {run.stderr!r}",
        )
        cli_events = parse_stream(run.stdout)
        workspace = os.path.join(tmp, f"{label}-lib")
        os.makedirs(workspace, exist_ok=True)
        lib_events = library_events(
            lib, f"http://127.0.0.1:{port}/v1", workspace, prompt
        )
    finally:
        stop_fixture(server, thread)
    compare(cli_events, lib_events, label)
    return cli_events, lib_events


def main():
    if sys.platform not in ("darwin", "linux"):
        print("test_ask_events_conformance: skip (libtny ships on Darwin/Linux only)")
        return 0
    if not os.access(TNY, os.X_OK):
        print(f"test_ask_events_conformance: {TNY} is not executable", file=sys.stderr)
        return 1
    lib = load_library()
    with tempfile.TemporaryDirectory(prefix="tny-events-conformance-") as tmp:
        cli, libtny = run_case(lib, tmp, "text", "say hello", 200, 0)
        terminal = [e for e in cli if e["kind"] == 7][0]
        check(terminal["stop_reason"] == 0, terminal)
        check([e for e in libtny if e["kind"] == 7][0]["stop_reason"] == 0, libtny)
        usage = [e for e in cli if e["kind"] == 6]
        check(usage and usage[0]["input_tokens"] == 11, usage)
        check(all(e["message_id"] == "" for e in cli if e["kind"] == 0), cli)

        cli, libtny = run_case(lib, tmp, "auth", "say hello", 401, 2)
        errors = [e for e in cli if e["kind"] == 8]
        check(errors and errors[0]["error_code"] == -6, errors)
        check([e for e in libtny if e["kind"] == 8][0]["error_code"] == -6, libtny)

        cli, libtny = run_case(lib, tmp, "empty", "EMPTY please", 200, 0)
        check("".join(e["text"] for e in cli if e["kind"] == 0) == "", cli)
    print("ok  ask --events=jsonl matches the libtny view and getter boundaries")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (Fail, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1) from None
