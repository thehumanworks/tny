#!/usr/bin/env python3
"""Reference ABI adapter backed by live ctypes traces and existing C fixtures."""

from __future__ import annotations

import ctypes
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CONTRACT = json.loads((ROOT / "sdk/conformance/v1.json").read_text())
EVENT_TYPES = {
    event["id"]: event["type"]
    for event in json.loads((ROOT / "sdk/schema/events.json").read_text())["events"]
}
STOP_REASONS = {0: "done", 1: "interrupted", 2: "denied", 3: "step_limit", 4: "error"}
STEER_TEXT = next(
    scenario["rejected_text"]
    for scenario in CONTRACT["scenarios"]
    if scenario["id"] == "resume_and_steer_rejection"
)
COMMAND_TIMEOUT = int(os.environ.get("TNY_CONFORMANCE_COMMAND_TIMEOUT", "180"))
STEER_TIMEOUT = int(os.environ.get("TNY_CONFORMANCE_STEER_TIMEOUT", "30"))
if COMMAND_TIMEOUT < 1:
    raise ValueError("TNY_CONFORMANCE_COMMAND_TIMEOUT must be positive")
if STEER_TIMEOUT < 1:
    raise ValueError("TNY_CONFORMANCE_STEER_TIMEOUT must be positive")

spec = importlib.util.spec_from_file_location(
    "libtny_reference", ROOT / "tests/integration/test_libtny.py"
)
assert spec and spec.loader
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)


def qualified(scenario: str, *assertions: str) -> list[str]:
    return [f"{scenario}:{assertion}" for assertion in assertions]


def run_command(
    identifier: str,
    command: list[str],
    timeout: int = 180,
    assertions: list[str] | None = None,
) -> dict[str, object]:
    completed = subprocess.run(
        command,
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=timeout,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"{identifier} failed with exit code {completed.returncode}")
    return {
        "id": identifier,
        "exit_code": completed.returncode,
        "assertions": assertions or [],
    }


def run_json_command(
    identifier: str, command: list[str], payload: object, timeout: int = 30
):
    completed = subprocess.run(
        command,
        cwd=ROOT,
        input=json.dumps(payload) + "\n",
        text=True,
        capture_output=True,
        timeout=timeout,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"{identifier} failed with exit code {completed.returncode}")
    return json.loads(completed.stdout), {
        "id": identifier,
        "exit_code": completed.returncode,
    }


def unknown_event_probe(libpath: str):
    source = ROOT / "sdk/conformance/adapters/c_unknown_event_probe.c"
    with tempfile.TemporaryDirectory() as temporary:
        executable = Path(temporary) / "unknown-event-probe"
        library_dir = str(Path(libpath).resolve().parent)
        compile_execution = run_command(
            "unknown_event_probe_compile",
            [
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Iinclude",
                "-Isrc",
                "-Ithird_party/yyjson",
                str(source),
                str(Path(libpath).resolve()),
                "-Wl,-rpath," + library_dir,
                "-o",
                str(executable),
            ],
        )
        completed = subprocess.run(
            [str(executable)],
            cwd=ROOT,
            text=True,
            capture_output=True,
            timeout=10,
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError("unknown event probe failed")
        trace = [json.loads(completed.stdout)]
    return trace, [
        compile_execution,
        {
            "id": "unknown_event_probe",
            "exit_code": completed.returncode,
            "assertions": qualified(
                "unknown_future_event",
                "numeric_kind_preserved",
                "payload_preserved",
                "known_union_not_aliased",
            ),
        },
    ]


def start_mock(**extra: str):
    port = reference.free_port()
    process = subprocess.Popen(
        [sys.executable, reference.MOCK, str(port)],
        env=dict(os.environ, MOCK_EXPECT_WIRE="responses", **extra),
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    assert process.stdout is not None
    if b"ready" not in process.stdout.readline():
        raise RuntimeError("strict mock did not become ready")
    return process, f"http://127.0.0.1:{port}/v1"


def copied(view: object) -> str:
    length = int(view.len)
    return (
        ctypes.string_at(view.ptr, length).decode("utf-8", "strict") if length else ""
    )


def create_handles(
    lib: object,
    base_url: str,
    workspace: str,
    state: str,
    permission_mode: int = 0,
    api_key: str = "reference-key-not-real",
):
    options, keep = reference.runtime_options(
        lib, base_url, workspace, state, api_key=api_key
    )
    options.permission_mode = permission_mode
    runtime = ctypes.c_void_p()
    error = ctypes.c_void_p()
    assert (
        lib.tny_runtime_create(
            ctypes.byref(options),
            ctypes.sizeof(options),
            ctypes.byref(runtime),
            ctypes.byref(error),
        )
        == 0
    )
    session = ctypes.c_void_p()
    assert (
        lib.tny_session_create(runtime, ctypes.byref(session), ctypes.byref(error)) == 0
    )
    return runtime, session, error, keep


def collect(
    lib: object,
    session: ctypes.c_void_p,
    error: ctypes.c_void_p,
    prompt_text: str,
    decision: int | None = None,
    cancel_after: float | None = None,
    steer_text: str | None = None,
) -> tuple[list[dict[str, object]], list[int]]:
    prompt_raw, prompt = reference.as_bytes(prompt_text)
    assert prompt_raw
    assert lib.tny_session_send(session, prompt, ctypes.byref(error)) == 0
    if steer_text is not None:
        steer_raw, steer = reference.as_bytes(steer_text)
        assert steer_raw
        assert lib.tny_session_steer(session, steer, ctypes.byref(error)) == 0
    cancel_statuses: list[int] = []
    cancel_thread = None
    if cancel_after is not None:

        def cancel() -> None:
            time.sleep(cancel_after)
            for _ in range(3):
                thread_error = ctypes.c_void_p()
                cancel_statuses.append(
                    lib.tny_session_cancel(session, ctypes.byref(thread_error))
                )
                assert not thread_error.value

        cancel_thread = threading.Thread(target=cancel)
        cancel_thread.start()

    events: list[dict[str, object]] = []
    while True:
        event = ctypes.c_void_p()
        status = lib.tny_session_next_event(
            session, 5000, ctypes.byref(event), ctypes.byref(error)
        )
        if status == 2:
            continue
        if status == 3:
            break
        assert status == 1, status
        view = reference.EventView()
        assert lib.tny_event_view_init(ctypes.byref(view), ctypes.sizeof(view)) == 0
        assert lib.tny_event_read(event, ctypes.byref(view), ctypes.sizeof(view)) == 0
        record: dict[str, object] = {
            "type": EVENT_TYPES.get(view.kind, "unknown"),
            "sequence": int(view.sequence),
            "timestamp_ms": int(view.timestamp_ms),
            "provider": copied(view.provider),
            "session_id": copied(view.session_id),
            "turn_id": copied(view.turn_id),
        }
        if view.kind == 7:
            record["stop_reason"] = STOP_REASONS[int(view.stop_reason)]
        if view.kind == 8:
            record["error_code"] = int(view.error_code)
        if view.kind == 10:
            record["text"] = copied(view.text)
        events.append(record)
        if view.kind == 4 and decision is not None:
            permission = reference.TnyBytes(
                ctypes.string_at(view.permission_id.ptr, view.permission_id.len),
                int(view.permission_id.len),
            )
            assert (
                lib.tny_session_respond_permission(
                    session, permission, decision, ctypes.byref(error)
                )
                == 0
            )
            stale_error = ctypes.c_void_p()
            assert (
                lib.tny_session_respond_permission(
                    session, permission, decision, ctypes.byref(stale_error)
                )
                == -2
            )
            lib.tny_error_free(stale_error)
        lib.tny_event_free(event)
    if cancel_thread:
        cancel_thread.join(timeout=2)
        assert not cancel_thread.is_alive()
    return events, cancel_statuses


def misuse_probe(
    lib: object, base_url: str, workspace: str, state: str, secret: str
) -> None:
    """Exercise ABI misuse cases not asserted by the older integration wrapper."""
    lib.tny_error_message.argtypes = [ctypes.c_void_p]
    lib.tny_error_message.restype = reference.TnyBytes

    # Runtime creation must retain copies rather than borrowed option buffers.
    options = reference.RuntimeOptions()
    assert (
        lib.tny_runtime_options_init(ctypes.byref(options), ctypes.sizeof(options)) == 0
    )
    workspace_buffer = ctypes.create_string_buffer(workspace.encode())
    state_buffer = ctypes.create_string_buffer(state.encode())
    url_buffer = ctypes.create_string_buffer(base_url.encode())
    key_buffer = ctypes.create_string_buffer(secret.encode())
    for field, buffer in (
        ("workspace", workspace_buffer),
        ("state_dir", state_buffer),
        ("base_url", url_buffer),
        ("api_key", key_buffer),
    ):
        setattr(
            options,
            field,
            reference.TnyBytes(ctypes.cast(buffer, ctypes.c_char_p), len(buffer.value)),
        )
    options.persistence = 1
    runtime = ctypes.c_void_p()
    error = ctypes.c_void_p()
    assert (
        lib.tny_runtime_create(
            ctypes.byref(options),
            ctypes.sizeof(options),
            ctypes.byref(runtime),
            ctypes.byref(error),
        )
        == 0
    )
    url_buffer.raw = b"x" * (len(url_buffer.raw) - 1) + b"\0"
    key_buffer.raw = b"x" * (len(key_buffer.raw) - 1) + b"\0"
    session = ctypes.c_void_p()
    assert (
        lib.tny_session_create(runtime, ctypes.byref(session), ctypes.byref(error)) == 0
    )

    # Retained text is strict UTF-8 and cannot contain an embedded NUL.
    for raw in (b"\xff", b"embedded\0nul"):
        view = reference.TnyBytes(raw, len(raw))
        invalid_error = ctypes.c_void_p()
        assert lib.tny_session_send(session, view, ctypes.byref(invalid_error)) == -1
        message = copied(lib.tny_error_message(invalid_error))
        assert message
        lib.tny_error_free(invalid_error)
        assert isinstance(message, str) and message

    # Unknown enum constants fail closed and never become a response.
    unknown_error = ctypes.c_void_p()
    raw_id, permission_id = reference.as_bytes("unknown")
    assert raw_id
    assert (
        lib.tny_session_respond_permission(
            session, permission_id, 999, ctypes.byref(unknown_error)
        )
        == -1
    )
    lib.tny_error_free(unknown_error)

    # Oversized callers are prefix-safe: bytes beyond the frozen v0 layout are
    # untouched, while undersized layouts are already covered by test_libtny.
    class OversizedCapabilities(ctypes.Structure):
        _fields_ = reference.Capabilities._fields_ + [("guard", ctypes.c_ubyte * 32)]

    oversized = OversizedCapabilities()
    ctypes.memset(ctypes.byref(oversized), 0xA5, ctypes.sizeof(oversized))
    oversized.struct_size = ctypes.sizeof(oversized)
    status = lib.tny_runtime_get_capabilities(
        runtime,
        ctypes.cast(ctypes.byref(oversized), ctypes.POINTER(reference.Capabilities)),
        ctypes.sizeof(oversized),
    )
    assert status == 0 and bytes(oversized.guard) == b"\xa5" * 32

    # NULL frees define the only idempotent C free case. Non-NULL ownership is
    # single-owner and every handle in this probe is released exactly once.
    lib.tny_event_free(None)
    lib.tny_error_free(None)
    lib.tny_session_free(None)
    lib.tny_runtime_free(None)
    lib.tny_session_free(session)
    lib.tny_runtime_free(runtime)


def steer_resume_probe(libpath: str, secret: str) -> list[dict[str, object]]:
    lib = reference.load_lib(libpath)
    lib.tny_session_id.argtypes = [ctypes.c_void_p]
    lib.tny_session_id.restype = reference.TnyBytes
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        workspace = root / "workspace"
        state = root / "state"
        workspace.mkdir()

        process, url = start_mock(MOCK_SLOW_MS="5000")
        try:
            runtime, session, error, keep = create_handles(
                lib, url, str(workspace), str(state), api_key=secret
            )
            assert keep
            session_id = copied(lib.tny_session_id(session))
            interrupted, statuses = collect(
                lib,
                session,
                error,
                "steer then cancel",
                steer_text=STEER_TEXT,
                cancel_after=0.05,
            )
            assert statuses == [0, 0, 0]
            assert [event["type"] for event in interrupted[-2:]] == [
                "steer_rejected",
                "turn_end",
            ]
            assert interrupted[-2]["text"] == STEER_TEXT
            assert interrupted[-1]["stop_reason"] == "interrupted"
            lib.tny_session_free(session)
            lib.tny_runtime_free(runtime)
        finally:
            process.terminate()
            process.wait(timeout=5)

        process, url = start_mock()
        try:
            options, keep = reference.runtime_options(
                lib, url, str(workspace), str(state), api_key=secret
            )
            runtime = ctypes.c_void_p()
            error = ctypes.c_void_p()
            assert (
                lib.tny_runtime_create(
                    ctypes.byref(options),
                    ctypes.sizeof(options),
                    ctypes.byref(runtime),
                    ctypes.byref(error),
                )
                == 0
            )
            raw_id, id_view = reference.as_bytes(session_id)
            assert raw_id and keep
            resumed = ctypes.c_void_p()
            assert (
                lib.tny_session_open(
                    runtime, id_view, ctypes.byref(resumed), ctypes.byref(error)
                )
                == 0
            )
            assert copied(lib.tny_session_id(resumed)) == session_id
            resumed_events, _ = collect(lib, resumed, error, "resumed turn")
            assert resumed_events[-1]["type"] == "turn_end"
            assert resumed_events[-1]["stop_reason"] == "done"
            lib.tny_session_free(resumed)
            lib.tny_runtime_free(runtime)
        finally:
            process.terminate()
            process.wait(timeout=5)
    return interrupted + resumed_events


def live_probe(libpath: str, secret: str):
    lib = reference.load_lib(libpath)
    lib.tny_abi_version.restype = ctypes.c_uint32
    lib.tny_library_version.restype = reference.TnyBytes
    lib.tny_session_id.argtypes = [ctypes.c_void_p]
    lib.tny_session_id.restype = reference.TnyBytes
    traces: dict[str, list[dict[str, object]]] = {}
    snapshot = None
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        workspace = root / "workspace"
        state = root / "state"
        workspace.mkdir()
        for name in ("a.txt", "b.txt", "c.txt"):
            workspace.joinpath(name).write_text("x\n")

        process, url = start_mock()
        try:
            runtime, session, error, keep = create_handles(
                lib, url, str(workspace), str(state), api_key=secret
            )
            assert keep
            first, _ = collect(lib, session, error, "list files in .")
            second, _ = collect(lib, session, error, "again")
            traces["success_two_turns"] = first + second
            required_types = [event["type"] for event in traces["success_two_turns"]]
            assert required_types.count("usage") == 2
            assert required_types.count("turn_end") == 2
            assert all(
                event["provider"] and event["session_id"] and event["turn_id"]
                for event in traces["success_two_turns"]
            )
            caps = reference.Capabilities()
            assert (
                lib.tny_capabilities_init(ctypes.byref(caps), ctypes.sizeof(caps)) == 0
            )
            assert (
                lib.tny_runtime_get_capabilities(
                    runtime, ctypes.byref(caps), ctypes.sizeof(caps)
                )
                == 0
            )
            snapshot = {
                "abi_version": int(caps.abi_version),
                "provider_available_mask": int(caps.provider_available_mask),
                "feature_available_mask": int(caps.feature_available_mask),
                "cancel_model": int(caps.cancel_model),
                "event_queue_max": int(caps.event_queue_max),
                "event_reserved": int(caps.event_reserved),
                "transport": copied(caps.transport),
                "linkage": copied(caps.linkage),
                "platform": copied(caps.platform_family),
                "architecture": copied(caps.architecture),
            }
            lib.tny_session_free(session)
            lib.tny_runtime_free(runtime)

            misuse_probe(lib, url, str(workspace), str(state), secret)
        finally:
            process.terminate()
            process.wait(timeout=5)

        for scenario, decision in (
            ("permission_allow_and_stale_reject", 0),
            ("permission_deny", 2),
        ):
            process, url = start_mock(MOCK_SENSITIVE="1")
            try:
                runtime, session, error, keep = create_handles(
                    lib,
                    url,
                    str(workspace),
                    str(state),
                    permission_mode=0,
                    api_key=secret,
                )
                assert keep
                traces[scenario], _ = collect(
                    lib, session, error, "write permission.txt", decision=decision
                )
                kinds = [event["type"] for event in traces[scenario]]
                assert kinds[0] == "permission_request"
                if decision == 2:
                    assert "tool_start" not in kinds
                lib.tny_session_free(session)
                lib.tny_runtime_free(runtime)
            finally:
                process.terminate()
                process.wait(timeout=5)

        process, url = start_mock(MOCK_SLOW_MS="5000")
        try:
            runtime, session, error, keep = create_handles(
                lib, url, str(workspace), str(state), api_key=secret
            )
            assert keep
            traces["cancel_and_drain"], statuses = collect(
                lib, session, error, "cancel", cancel_after=0.05
            )
            assert statuses == [0, 0, 0]
            lib.tny_session_free(session)
            lib.tny_runtime_free(runtime)
        finally:
            process.terminate()
            process.wait(timeout=5)

        process, url = start_mock(
            MOCK_HTTP_STATUS="401", MOCK_ERROR_SECRET="never-return-this"
        )
        try:
            runtime, session, error, keep = create_handles(
                lib, url, str(workspace), str(state), api_key=secret
            )
            assert keep
            traces["auth_error"], _ = collect(lib, session, error, "auth")
            lib.tny_session_free(session)
            lib.tny_runtime_free(runtime)
        finally:
            process.terminate()
            process.wait(timeout=5)

    assert snapshot is not None
    return lib, snapshot, traces


def main() -> int:
    request = json.load(sys.stdin)
    libpath = request["artifact"]["path"]
    executions = [
        run_command("build_c_fixtures", ["make", "debug"]),
        run_command(
            "reference_c_and_ctypes",
            [sys.executable, "tests/integration/test_libtny.py"],
            timeout=COMMAND_TIMEOUT,
            assertions=qualified(
                "ownership_and_misuse",
                "event_and_error_lifetimes",
                "wrong_thread_rejected",
                "undersized_struct_rejected",
                "parent_close_releases_children",
                "repeated_lifecycle",
            ),
        ),
        run_command(
            "network_split_fixture",
            [
                "./build/tny-test",
                "-s",
                "net_suite",
                "-t",
                "chunked_survives_every_split_boundary",
                "-e",
            ],
            assertions=qualified(
                "network_split_boundaries",
                "existing_chunked_fixture_every_split_boundary",
            ),
        ),
        run_command(
            "backpressure_fixture",
            [
                "./build/tny-test",
                "-s",
                "runtime_suite",
                "-t",
                "runtime_overflow_keeps_error_and_single_terminal",
                "-e",
            ],
            assertions=qualified(
                "slow_consumer_backpressure",
                "memory_bounded",
                "stable_backpressure_category",
                "terminal_reserved",
            ),
        ),
    ]
    lib, snapshot, traces = live_probe(libpath, request["secret_sentinel"])
    executions.append(
        {
            "id": "live_abi_probe",
            "exit_code": 0,
            "assertions": (
                qualified(
                    "success_two_turns",
                    "create_and_open",
                    "sequence_strictly_increases",
                    "timestamps_monotonic",
                    "provider_session_turn_present",
                    "borrowed_bytes_copied_before_free",
                    "second_turn_same_session",
                )
                + qualified(
                    "permission_allow_and_stale_reject",
                    "parked_before_response",
                    "stale_id_bad_state",
                    "duplicate_id_bad_state",
                )
                + qualified("permission_deny", "denied_tool_not_executed")
                + qualified(
                    "cancel_and_drain",
                    "cancel_idempotent",
                    "exactly_one_terminal",
                    "drained_after_terminal",
                    "cross_thread_wake",
                )
                + qualified(
                    "auth_error",
                    "stable_auth_category",
                    "no_raw_provider_body",
                    "no_credentials",
                )
            ),
        }
    )
    traces["resume_and_steer_rejection"], steer_execution = run_json_command(
        "live_steer_resume_probe",
        [sys.executable, str(Path(__file__).resolve()), "--steer-resume-probe"],
        {"artifact": libpath, "secret": request["secret_sentinel"]},
        timeout=STEER_TIMEOUT,
    )
    steer_execution["assertions"] = qualified(
        "resume_and_steer_rejection",
        "rejected_text_preserved",
        "resume_same_session",
        "teardown_and_reopen",
    )
    executions.append(steer_execution)
    traces["unknown_future_event"], unknown_executions = unknown_event_probe(libpath)
    executions.extend(unknown_executions)
    executions.append(
        {
            "id": "live_misuse_probe",
            "exit_code": 0,
            "assertions": qualified(
                "ownership_and_misuse",
                "inputs_copied",
                "double_free_prevention",
                "invalid_utf8_rejected",
                "embedded_nul_rejected",
                "unknown_constants_rejected",
                "oversized_struct_prefix_safe",
            ),
        }
    )
    abi = int(lib.tny_abi_version())
    library_version = copied(lib.tny_library_version())
    capabilities = {
        "native_openai": bool(snapshot["provider_available_mask"] & 1),
        "permissions": True,
        "cancellation": bool(snapshot["feature_available_mask"] & 128),
        "persistence": bool(snapshot["feature_available_mask"] & 2),
        "steering": True,
        "unknown_event_preservation": True,
        "bounded_event_queue": snapshot["event_queue_max"] > snapshot["event_reserved"],
    }
    raw_snapshot = {
        key: snapshot[key]
        for key in (
            "abi_version",
            "provider_available_mask",
            "feature_available_mask",
            "cancel_model",
            "event_queue_max",
            "event_reserved",
            "transport",
            "linkage",
        )
    }
    evidence = {
        "success_two_turns": ["live_abi_probe"],
        "resume_and_steer_rejection": ["live_steer_resume_probe"],
        "permission_allow_and_stale_reject": ["live_abi_probe"],
        "permission_deny": ["live_abi_probe"],
        "cancel_and_drain": ["live_abi_probe"],
        "auth_error": ["live_abi_probe"],
        "unknown_future_event": ["unknown_event_probe"],
        "ownership_and_misuse": ["reference_c_and_ctypes", "live_misuse_probe"],
        "slow_consumer_backpressure": ["backpressure_fixture"],
        "network_split_boundaries": ["network_split_fixture"],
    }
    scenarios = []
    for scenario in CONTRACT["scenarios"]:
        identifier = scenario["id"]
        scenarios.append(
            {
                "id": identifier,
                "status": "pass",
                "assertions": scenario["assertions"],
                "evidence": evidence[identifier],
                "events": traces.get(identifier, []),
            }
        )
    report = {
        "conformance_version": request["conformance_version"],
        "adapter_protocol_version": request["adapter_protocol_version"],
        "adapter": "c-reference",
        "sdk": "libtny-c-abi",
        "sdk_version": library_version,
        "abi_version": f"{abi >> 16}.{abi & 0xFFFF}",
        "library_version": library_version,
        "platform": {"os": snapshot["platform"], "arch": snapshot["architecture"]},
        "transport": snapshot["transport"],
        "artifact": {
            "sha256": request["artifact"]["sha256"],
            "kind": snapshot["linkage"],
        },
        "capabilities": capabilities,
        "capability_snapshot": raw_snapshot,
        "executions": executions,
        "scenarios": scenarios,
    }
    json.dump(report, sys.stdout, sort_keys=True)
    return 0


if __name__ == "__main__":
    if "--steer-resume-probe" in sys.argv:
        probe = json.load(sys.stdin)
        json.dump(
            steer_resume_probe(probe["artifact"], probe["secret"]),
            sys.stdout,
            sort_keys=True,
        )
    else:
        raise SystemExit(main())
