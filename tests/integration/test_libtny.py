#!/usr/bin/env python3
"""Build a clean C consumer and run a full libtny turn against the mock."""
import os
import ctypes
import faulthandler
import select
import socket
import re
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MOCK = os.path.join(ROOT, "tests", "integration", "mock_openai.py")
MOCK_READY_TIMEOUT = 10.0
TURN_TIMEOUT = float(os.environ.get("TNY_TEST_TURN_TIMEOUT", "45"))
SUITE_TIMEOUT = float(os.environ.get("TNY_TEST_SUITE_TIMEOUT", "120"))
if TURN_TIMEOUT <= 0 or SUITE_TIMEOUT <= 0:
    raise ValueError("libtny test timeouts must be positive")


class TnyBytes(ctypes.Structure):
    _fields_ = [("ptr", ctypes.c_char_p), ("len", ctypes.c_uint64)]


class RuntimeOptions(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("permission_mode", ctypes.c_uint32),
        ("persistence", ctypes.c_uint32),
        ("max_steps", ctypes.c_uint32),
        ("max_tool_result_bytes", ctypes.c_uint64),
        ("workspace", TnyBytes), ("state_dir", TnyBytes),
        ("provider", TnyBytes), ("model", TnyBytes),
        ("base_url", TnyBytes), ("api_key", TnyBytes),
        ("wire_api", TnyBytes),
        ("reserved", ctypes.c_uint64 * 8),
    ]


class EventView(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32), ("kind", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32), ("tool_ok", ctypes.c_uint32),
        ("permission_options", ctypes.c_uint32), ("stop_reason", ctypes.c_uint32),
        ("error_code", ctypes.c_int32), ("has_cost", ctypes.c_uint32),
        ("sequence", ctypes.c_uint64), ("timestamp_ms", ctypes.c_int64),
        ("input_tokens", ctypes.c_int64), ("output_tokens", ctypes.c_int64),
        ("context_used", ctypes.c_int64), ("context_size", ctypes.c_int64),
        ("cost", ctypes.c_double),
        ("provider", TnyBytes), ("session_id", TnyBytes), ("turn_id", TnyBytes),
        ("text", TnyBytes), ("message_id", TnyBytes),
        ("tool_name", TnyBytes), ("tool_id", TnyBytes), ("tool_detail", TnyBytes),
        ("permission_id", TnyBytes), ("permission_summary", TnyBytes),
        ("message_type", TnyBytes), ("reserved", ctypes.c_uint64 * 8),
    ]


class Capabilities(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("provider_selected", ctypes.c_uint32),
        ("provider_initialized", ctypes.c_uint32),
        ("endpoint_reachability", ctypes.c_uint32),
        ("threading_model", ctypes.c_uint32),
        ("cancel_model", ctypes.c_uint32),
        ("provider_available_mask", ctypes.c_uint64),
        ("feature_available_mask", ctypes.c_uint64),
        ("feature_enabled_mask", ctypes.c_uint64),
        ("event_queue_max", ctypes.c_uint32),
        ("event_reserved", ctypes.c_uint32),
        ("event_payload_bytes_max", ctypes.c_uint64),
        ("event_reserved_bytes", ctypes.c_uint64),
        ("library_version", TnyBytes),
        ("platform_family", TnyBytes),
        ("architecture", TnyBytes),
        ("transport", TnyBytes),
        ("tls_implementation", TnyBytes),
        ("linkage", TnyBytes),
        ("reserved", ctypes.c_uint64 * 8),
    ]


# ABI-0 v0 layouts are immutable. These are the original LP64 sizes and
# reserved-tail offsets; a larger future shape must be a new v1 type/symbol.
assert (ctypes.sizeof(RuntimeOptions), RuntimeOptions.reserved.offset) == (200, 136)
assert (ctypes.sizeof(EventView), EventView.reserved.offset) == (328, 264)
assert (ctypes.sizeof(Capabilities), Capabilities.reserved.offset) == (240, 176)


def as_bytes(value):
    raw = value.encode()
    return raw, TnyBytes(raw, len(raw))


def load_lib(libpath):
    lib = ctypes.CDLL(libpath)
    lib.tny_runtime_options_init.argtypes = [ctypes.POINTER(RuntimeOptions)]
    lib.tny_runtime_create.argtypes = [ctypes.POINTER(RuntimeOptions),
                                       ctypes.POINTER(ctypes.c_void_p),
                                       ctypes.POINTER(ctypes.c_void_p)]
    lib.tny_runtime_create.restype = ctypes.c_int32
    lib.tny_capabilities_init.argtypes = [ctypes.POINTER(Capabilities)]
    lib.tny_runtime_get_capabilities.argtypes = [ctypes.c_void_p,
                                                 ctypes.POINTER(Capabilities)]
    lib.tny_runtime_get_capabilities.restype = ctypes.c_int32
    lib.tny_session_create.argtypes = [ctypes.c_void_p,
                                       ctypes.POINTER(ctypes.c_void_p),
                                       ctypes.POINTER(ctypes.c_void_p)]
    lib.tny_session_create.restype = ctypes.c_int32
    lib.tny_session_open.argtypes = [ctypes.c_void_p, TnyBytes,
                                     ctypes.POINTER(ctypes.c_void_p),
                                     ctypes.POINTER(ctypes.c_void_p)]
    lib.tny_session_open.restype = ctypes.c_int32
    lib.tny_session_send.argtypes = [ctypes.c_void_p, TnyBytes,
                                     ctypes.POINTER(ctypes.c_void_p)]
    lib.tny_session_send.restype = ctypes.c_int32
    lib.tny_session_next_event.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                           ctypes.POINTER(ctypes.c_void_p),
                                           ctypes.POINTER(ctypes.c_void_p)]
    lib.tny_session_next_event.restype = ctypes.c_int32
    lib.tny_event_view_init.argtypes = [ctypes.POINTER(EventView)]
    lib.tny_event_read.argtypes = [ctypes.c_void_p, ctypes.POINTER(EventView)]
    lib.tny_event_read.restype = ctypes.c_int32
    lib.tny_event_get_kind.argtypes = [ctypes.c_void_p]
    lib.tny_event_get_kind.restype = ctypes.c_uint32
    lib.tny_event_text.argtypes = [ctypes.c_void_p]
    lib.tny_event_text.restype = TnyBytes
    lib.tny_event_permission_id.argtypes = [ctypes.c_void_p]
    lib.tny_event_permission_id.restype = TnyBytes
    lib.tny_event_stop_reason.argtypes = [ctypes.c_void_p]
    lib.tny_event_stop_reason.restype = ctypes.c_uint32
    lib.tny_event_error_code.argtypes = [ctypes.c_void_p]
    lib.tny_event_error_code.restype = ctypes.c_int32
    lib.tny_session_respond_permission.argtypes = [ctypes.c_void_p, TnyBytes,
                                                   ctypes.c_uint32,
                                                   ctypes.POINTER(ctypes.c_void_p)]
    lib.tny_session_respond_permission.restype = ctypes.c_int32
    lib.tny_session_cancel.argtypes = [ctypes.c_void_p,
                                       ctypes.POINTER(ctypes.c_void_p)]
    lib.tny_session_cancel.restype = ctypes.c_int32
    lib.tny_event_free.argtypes = [ctypes.c_void_p]
    lib.tny_error_free.argtypes = [ctypes.c_void_p]
    lib.tny_session_free.argtypes = [ctypes.c_void_p]
    lib.tny_runtime_free.argtypes = [ctypes.c_void_p]
    return lib


def runtime_options(lib, base_url, workspace, state,
                    api_key="test-key-not-real", persistence=1):
    opts = RuntimeOptions()
    lib.tny_runtime_options_init(ctypes.byref(opts))
    opts.persistence = persistence
    keep = []
    fields = [("workspace", workspace), ("base_url", base_url)]
    if state is not None:
        fields.append(("state_dir", state))
    if api_key is not None:
        fields.append(("api_key", api_key))
    for field, value in fields:
        raw, view = as_bytes(value)
        keep.append(raw)
        setattr(opts, field, view)
    return opts, keep


def run_ctypes(libpath, base_url, workspace, state, repeat=False,
               api_key="test-key-not-real", expect_send_error=None,
               persistence=1):
    lib = load_lib(libpath)

    opts, keep = runtime_options(lib, base_url, workspace, state,
                                 api_key, persistence)

    invalid = RuntimeOptions.from_buffer_copy(opts)
    invalid_workspace = b"bad\0path"
    invalid.workspace = TnyBytes(invalid_workspace, len(invalid_workspace))
    invalid_runtime = ctypes.c_void_p()
    invalid_error = ctypes.c_void_p()
    assert lib.tny_runtime_create(ctypes.byref(invalid),
                                  ctypes.byref(invalid_runtime),
                                  ctypes.byref(invalid_error)) == -1
    lib.tny_error_free(invalid_error)

    unsupported = RuntimeOptions.from_buffer_copy(opts)
    cursor_raw = b"cursor"
    unsupported.provider = TnyBytes(cursor_raw, len(cursor_raw))
    unsupported_runtime = ctypes.c_void_p()
    unsupported_error = ctypes.c_void_p()
    assert lib.tny_runtime_create(ctypes.byref(unsupported),
                                  ctypes.byref(unsupported_runtime),
                                  ctypes.byref(unsupported_error)) == -9
    lib.tny_error_free(unsupported_error)

    oversized_steps = RuntimeOptions.from_buffer_copy(opts)
    oversized_steps.max_steps = 0x80000000
    oversized_runtime = ctypes.c_void_p()
    oversized_error = ctypes.c_void_p()
    assert lib.tny_runtime_create(
        ctypes.byref(oversized_steps), ctypes.byref(oversized_runtime),
        ctypes.byref(oversized_error)) == -1
    lib.tny_error_free(oversized_error)

    if persistence:
        missing_state = RuntimeOptions.from_buffer_copy(opts)
        missing_state.state_dir = TnyBytes()
        missing_runtime = ctypes.c_void_p()
        missing_error = ctypes.c_void_p()
        assert lib.tny_runtime_create(ctypes.byref(missing_state),
                                      ctypes.byref(missing_runtime),
                                      ctypes.byref(missing_error)) == -1
        lib.tny_error_free(missing_error)

    runtime = ctypes.c_void_p()
    error = ctypes.c_void_p()
    assert lib.tny_runtime_create(ctypes.byref(opts), ctypes.byref(runtime),
                                  ctypes.byref(error)) == 0

    def capabilities():
        value = Capabilities()
        lib.tny_capabilities_init(ctypes.byref(value))
        assert value.struct_size == ctypes.sizeof(Capabilities)
        assert lib.tny_runtime_get_capabilities(runtime,
                                                ctypes.byref(value)) == 0
        return value

    tiny_caps = Capabilities()
    tiny_caps.struct_size = 8
    assert lib.tny_runtime_get_capabilities(runtime,
                                            ctypes.byref(tiny_caps)) == -1
    prefix_caps = Capabilities()
    prefix_caps.struct_size = Capabilities.library_version.offset
    assert lib.tny_runtime_get_capabilities(runtime,
                                            ctypes.byref(prefix_caps)) == 0
    assert prefix_caps.schema_version == 1 and prefix_caps.provider_selected == 1

    caps = capabilities()
    assert caps.schema_version == 1 and caps.abi_version & 0xffff == 5
    assert caps.provider_selected == 1 and caps.provider_initialized == 0
    assert caps.endpoint_reachability == 0
    assert caps.threading_model == 1 and caps.cancel_model == 2
    assert caps.provider_available_mask == 1
    assert caps.feature_available_mask & 0x87 == 0x87
    assert not caps.feature_available_mask & ~0x87
    expected_enabled = 0x84 | (0x2 if persistence else 0)
    if base_url.startswith("https://"):
        expected_enabled |= 0x1
    assert caps.feature_enabled_mask == expected_enabled
    assert (caps.event_queue_max, caps.event_reserved) == (256, 2)
    assert (caps.event_payload_bytes_max, caps.event_reserved_bytes) == (1048576, 1024)
    assert ctypes.string_at(caps.transport.ptr, caps.transport.len) == b"native-http1"
    assert ctypes.string_at(caps.linkage.ptr, caps.linkage.len) == b"shared"
    platform = ctypes.string_at(caps.platform_family.ptr, caps.platform_family.len)
    tls = ctypes.string_at(caps.tls_implementation.ptr, caps.tls_implementation.len)
    if sys.platform == "darwin":
        assert platform == b"macos" and tls == b"securetransport"
    else:
        assert platform == b"linux-glibc" and tls == b"openssl-dynamic"
    second = ctypes.c_void_p()
    second_error = ctypes.c_void_p()
    assert lib.tny_runtime_create(ctypes.byref(opts), ctypes.byref(second),
                                  ctypes.byref(second_error)) == 0
    lib.tny_runtime_free(second)

    bad_raw, bad_id = as_bytes("../../outside")
    keep.append(bad_raw)
    bad_session = ctypes.c_void_p()
    bad_error = ctypes.c_void_p()
    assert lib.tny_session_open(runtime, bad_id, ctypes.byref(bad_session),
                                ctypes.byref(bad_error)) == -5
    lib.tny_error_free(bad_error)

    session = ctypes.c_void_p()
    assert lib.tny_session_create(runtime, ctypes.byref(session),
                                  ctypes.byref(error)) == 0
    assert capabilities().provider_initialized == 0
    cross_thread = []
    def wrong_thread_cancel():
        thread_error = ctypes.c_void_p()
        cross_thread.append(lib.tny_session_cancel(session,
                                                   ctypes.byref(thread_error)))
        thread_caps = Capabilities()
        lib.tny_capabilities_init(ctypes.byref(thread_caps))
        cross_thread.append(lib.tny_runtime_get_capabilities(
            runtime, ctypes.byref(thread_caps)))
        lib.tny_error_free(thread_error)
    thread = threading.Thread(target=wrong_thread_cancel, daemon=True)
    thread.start()
    thread.join(timeout=2)
    assert not thread.is_alive()
    assert cross_thread == [0, -2]
    output = bytearray()
    saw_permission = False
    stop_reason = None
    error_codes = []
    event_sequences = []
    prompts = ["list files in .", "again"] if repeat else ["list files in ."]
    for prompt_text in prompts:
        turn_deadline = time.monotonic() + TURN_TIMEOUT
        prior_reachability = capabilities().endpoint_reachability
        prompt_raw, prompt = as_bytes(prompt_text)
        keep.append(prompt_raw)
        send_status = lib.tny_session_send(session, prompt, ctypes.byref(error))
        if expect_send_error is not None:
            assert send_status == expect_send_error, send_status
            failed_caps = capabilities()
            assert failed_caps.endpoint_reachability == (
                2 if expect_send_error == -7 else 0)
            lib.tny_error_free(error)
            lib.tny_session_free(session)
            lib.tny_runtime_free(runtime)
            return b"", False, None, [send_status]
        assert send_status == 0
        ready_caps = capabilities()
        assert ready_caps.provider_initialized == 1
        assert ready_caps.endpoint_reachability == prior_reachability
        while True:
            event = ctypes.c_void_p()
            status = lib.tny_session_next_event(session, 5000, ctypes.byref(event),
                                                ctypes.byref(error))
            if status == 2:
                if time.monotonic() >= turn_deadline:
                    raise AssertionError(
                        f"libtny turn did not settle within {TURN_TIMEOUT:.0f}s")
                continue
            if status == 3:
                break
            assert status == 1, status
            kind = lib.tny_event_get_kind(event)
            view = EventView()
            lib.tny_event_view_init(ctypes.byref(view))
            assert view.struct_size == ctypes.sizeof(EventView)
            if not event_sequences:
                tiny = EventView()
                tiny.struct_size = 8
                assert lib.tny_event_read(event, ctypes.byref(tiny)) == -1
            assert lib.tny_event_read(event, ctypes.byref(view)) == 0
            assert view.schema_version == 1 and view.kind == kind
            assert view.sequence > 0 and view.timestamp_ms >= 0
            assert ctypes.string_at(view.provider.ptr, view.provider.len) == b"openai"
            assert view.session_id.len > 0 and view.turn_id.len > 0
            if event_sequences:
                assert view.sequence > event_sequences[-1]
            event_sequences.append(view.sequence)
            if kind == 0:
                view = lib.tny_event_text(event)
                output.extend(ctypes.string_at(view.ptr, view.len))
            elif kind == 4:
                saw_permission = True
                request_id = lib.tny_event_permission_id(event)
                request_raw = ctypes.string_at(request_id.ptr, request_id.len)
                assert lib.tny_session_respond_permission(session, request_id, 2,
                                                           ctypes.byref(error)) == 0
                stale_error = ctypes.c_void_p()
                stale = TnyBytes(request_raw, len(request_raw))
                assert lib.tny_session_respond_permission(
                    session, stale, 2, ctypes.byref(stale_error)) == -2
                lib.tny_error_free(stale_error)
            elif kind == 7:
                stop_reason = lib.tny_event_stop_reason(event)
            elif kind == 8:
                error_codes.append(lib.tny_event_error_code(event))
            lib.tny_event_free(event)
        observed = capabilities().endpoint_reachability
        if error_codes and error_codes[-1] == -7 and not output:
            assert observed == 2
        else:
            assert observed == 1
    if repeat:
        cancel_raw, cancel_prompt = as_bytes("cancel this turn")
        keep.append(cancel_raw)
        assert lib.tny_session_send(session, cancel_prompt,
                                    ctypes.byref(error)) == 0
        assert lib.tny_session_cancel(session, ctypes.byref(error)) == 0
        cancelled = None
        cancel_deadline = time.monotonic() + TURN_TIMEOUT
        while True:
            event = ctypes.c_void_p()
            status = lib.tny_session_next_event(session, 5000,
                                                ctypes.byref(event),
                                                ctypes.byref(error))
            if status == 3:
                break
            if status == 2 and time.monotonic() >= cancel_deadline:
                raise AssertionError(
                    f"libtny cancellation did not settle within {TURN_TIMEOUT:.0f}s")
            assert status in (1, 2), status
            if status == 1:
                if lib.tny_event_get_kind(event) == 7:
                    cancelled = lib.tny_event_stop_reason(event)
                lib.tny_event_free(event)
        assert cancelled == 1
    lib.tny_session_free(session)
    lib.tny_runtime_free(runtime)
    return bytes(output), saw_permission, stop_reason, error_codes


def run_cancel_wake(libpath, base_url, workspace, state):
    """Cancel while next_event is blocked, from a non-owner thread."""
    lib = load_lib(libpath)
    opts, keep = runtime_options(lib, base_url, workspace, state)
    runtime = ctypes.c_void_p()
    session = ctypes.c_void_p()
    error = ctypes.c_void_p()
    assert lib.tny_runtime_create(ctypes.byref(opts), ctypes.byref(runtime),
                                  ctypes.byref(error)) == 0
    assert lib.tny_session_create(runtime, ctypes.byref(session),
                                  ctypes.byref(error)) == 0
    prompt_raw, prompt = as_bytes("wait for cancellation")
    keep.append(prompt_raw)
    assert lib.tny_session_send(session, prompt, ctypes.byref(error)) == 0

    cancel_results = []
    def cancel_from_scheduler():
        time.sleep(0.05)
        for _ in range(8):
            thread_error = ctypes.c_void_p()
            cancel_results.append(lib.tny_session_cancel(
                session, ctypes.byref(thread_error)))
            assert not thread_error.value

    scheduler = threading.Thread(target=cancel_from_scheduler, daemon=True)
    scheduler.start()
    started = time.monotonic()
    terminals = []
    deadline = started + TURN_TIMEOUT
    while True:
        event = ctypes.c_void_p()
        status = lib.tny_session_next_event(session, 5000,
                                            ctypes.byref(event),
                                            ctypes.byref(error))
        if status == 3:
            break
        if status == 2 and time.monotonic() >= deadline:
            raise AssertionError(
                f"cross-thread cancellation did not settle within {TURN_TIMEOUT:.0f}s")
        assert status == 1, status
        if lib.tny_event_get_kind(event) == 7:
            terminals.append(lib.tny_event_stop_reason(event))
        lib.tny_event_free(event)
    elapsed = time.monotonic() - started
    scheduler.join(timeout=2)
    assert not scheduler.is_alive()
    assert cancel_results == [0] * 8
    assert terminals == [1], terminals
    assert elapsed < 1.0, f"cross-thread cancellation took {elapsed:.3f}s"
    # Inactive cancellation is also an idempotent no-op and cannot poison
    # teardown or a future session.
    assert lib.tny_session_cancel(session, ctypes.byref(error)) == 0
    lib.tny_session_free(session)
    lib.tny_runtime_free(runtime)
    return elapsed


def stress_independent_teardown(libpath, workspace_a, workspace_b):
    lib = load_lib(libpath)
    opts_a, keep_a = runtime_options(
        lib, "http://127.0.0.1:1/v1", workspace_a, None,
        api_key="runtime-a", persistence=0)
    opts_b, keep_b = runtime_options(
        lib, "http://127.0.0.1:2/v1", workspace_b, None,
        api_key="runtime-b", persistence=0)
    assert keep_a and keep_b
    for i in range(64):
        runtime_a = ctypes.c_void_p()
        runtime_b = ctypes.c_void_p()
        session_a = ctypes.c_void_p()
        session_b = ctypes.c_void_p()
        error = ctypes.c_void_p()
        assert lib.tny_runtime_create(ctypes.byref(opts_a),
                                      ctypes.byref(runtime_a),
                                      ctypes.byref(error)) == 0
        assert lib.tny_runtime_create(ctypes.byref(opts_b),
                                      ctypes.byref(runtime_b),
                                      ctypes.byref(error)) == 0
        assert lib.tny_session_create(runtime_a, ctypes.byref(session_a),
                                      ctypes.byref(error)) == 0
        assert lib.tny_session_create(runtime_b, ctypes.byref(session_b),
                                      ctypes.byref(error)) == 0
        if i & 1:
            lib.tny_runtime_free(runtime_a)  # closes its child only
            lib.tny_session_free(session_b)
            lib.tny_runtime_free(runtime_b)
        else:
            lib.tny_runtime_free(runtime_b)
            caps = Capabilities()
            lib.tny_capabilities_init(ctypes.byref(caps))
            assert lib.tny_runtime_get_capabilities(
                runtime_a, ctypes.byref(caps)) == 0
            lib.tny_session_free(session_a)
            lib.tny_runtime_free(runtime_a)


def verify_fork_rejection(libpath, workspace):
    if not hasattr(os, "fork"):
        return
    lib = load_lib(libpath)
    opts, keep = runtime_options(
        lib, "http://127.0.0.1:1/v1", workspace, None,
        api_key="fork-test", persistence=0)
    assert keep
    runtime = ctypes.c_void_p()
    session = ctypes.c_void_p()
    error = ctypes.c_void_p()
    assert lib.tny_runtime_create(ctypes.byref(opts), ctypes.byref(runtime),
                                  ctypes.byref(error)) == 0
    assert lib.tny_session_create(runtime, ctypes.byref(session),
                                  ctypes.byref(error)) == 0
    read_fd, write_fd = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(read_fd)
        caps = Capabilities()
        lib.tny_capabilities_init(ctypes.byref(caps))
        cap_status = lib.tny_runtime_get_capabilities(runtime,
                                                      ctypes.byref(caps))
        cancel_status = lib.tny_session_cancel(session, ctypes.byref(error))
        os.write(write_fd, f"{cap_status},{cancel_status}".encode())
        os._exit(0)
    os.close(write_fd)
    if not select.select([read_fd], [], [], 5.0)[0]:
        os.kill(pid, 9)
        os.waitpid(pid, 0)
        os.close(read_fd)
        raise AssertionError("fork-rejection child did not respond within 5s")
    child_result = os.read(read_fd, 64)
    os.close(read_fd)
    _, status = os.waitpid(pid, 0)
    assert os.waitstatus_to_exitcode(status) == 0
    assert child_result == b"-2,-2", child_result
    lib.tny_session_free(session)
    lib.tny_runtime_free(runtime)


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def stage(name):
    print(f"test_libtny: stage {name}", file=sys.stderr, flush=True)


def start_mock(**extra_env):
    """Start the fixture on a kernel-assigned port with bounded readiness."""
    process = subprocess.Popen(
        [sys.executable, MOCK, "0"],
        env=dict(os.environ, **extra_env),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert process.stdout is not None and process.stderr is not None
    deadline = time.monotonic() + MOCK_READY_TIMEOUT
    while time.monotonic() < deadline:
        if process.poll() is not None:
            stderr = process.stderr.read().decode("utf-8", "replace")
            stop_mock(process)
            raise AssertionError(
                f"mock exited before ready (rc={process.returncode}): {stderr}")
        remaining = max(0.0, deadline - time.monotonic())
        if select.select([process.stdout], [], [], min(0.1, remaining))[0]:
            line = process.stdout.readline().decode("utf-8", "replace").strip()
            if not line:
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    pass
                stderr = process.stderr.read().decode("utf-8", "replace")
                stop_mock(process)
                raise AssertionError(
                    f"mock closed stdout before ready (rc={process.returncode}): "
                    f"{stderr}")
            match = re.fullmatch(r"ready on (\d+)", line)
            if not match:
                stop_mock(process)
                raise AssertionError(f"unexpected mock ready line: {line!r}")
            return process, int(match.group(1))
    stop_mock(process)
    raise AssertionError(
        f"mock did not become ready within {MOCK_READY_TIMEOUT:.0f}s")


def stop_mock(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
    if process.stdout is not None:
        process.stdout.close()
    if process.stderr is not None:
        process.stderr.close()


def main():
    if sys.platform not in ("darwin", "linux"):
        print("test_libtny: skip (ABI 0 ships on Darwin/Linux only)")
        return
    stage("build shared library")
    subprocess.run(["make", "lib-shared"], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL, timeout=120)
    libdir = os.path.join(ROOT, "build", "lib")
    libname = "libtny.0.dylib" if sys.platform == "darwin" else "libtny.so.0"
    libpath = os.path.join(libdir, libname)
    if sys.platform == "darwin":
        expected = {line.strip().removeprefix("_") for line in
                    open(os.path.join(ROOT, "abi", "libtny.exports.macos"))
                    if line.strip()}
        symbols = subprocess.check_output(["nm", "-gU", libpath], text=True)
    else:
        expected = set(re.findall(r"^\s+(tny_[A-Za-z0-9_]+);", open(
            os.path.join(ROOT, "abi", "libtny.map")).read(), re.M))
        symbols = subprocess.check_output(["nm", "-D", "--defined-only", libpath],
                                          text=True)
    actual = {line.split()[-1].split("@")[0].removeprefix("_")
              for line in symbols.splitlines() if line.split() and
              line.split()[-1].split("@")[0].removeprefix("_").startswith("tny_")}
    assert actual == expected, (sorted(actual - expected), sorted(expected - actual))

    stage("synchronous error classification")
    core_only = os.environ.get("TNY_LIBTNY_CORE_ONLY") == "1"
    with tempfile.TemporaryDirectory() as root:
        workspace = os.path.join(root, "workspace")
        state = os.path.join(root, "state")
        os.makedirs(workspace)
        _, _, _, errors = run_ctypes(
            libpath, "https://api.example.invalid/v1", workspace, state,
            api_key=None, expect_send_error=-6)
        assert errors == [-6]

    # Installed header/library must work without source-tree include or lib
    # paths. The no-argument run reaches main and returns its usage status.
    stage("clean-prefix consumers")
    with tempfile.TemporaryDirectory() as install_root:
        prefix = os.path.join(install_root, "prefix")
        subprocess.run(["make", "install-lib", "PREFIX=" + prefix], cwd=ROOT,
                       check=True, stdout=subprocess.DEVNULL)
        installed_exe = os.path.join(install_root, "embed")
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I" + os.path.join(prefix, "include"),
            os.path.join(ROOT, "examples", "embed.c"),
            "-L" + os.path.join(prefix, "lib"), "-ltny",
            "-Wl,-rpath," + os.path.join(prefix, "lib"), "-o", installed_exe,
        ], check=True)
        installed = subprocess.run([installed_exe], capture_output=True)
        assert installed.returncode == 2 and b"usage:" in installed.stderr

        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
            "-Werror", "-I" + os.path.join(prefix, "include"), "-x", "c",
            "-fsyntax-only", "-",
        ], input=(b'#include <tny/tny.h>\n'
                  b'int main(void) { tny_capabilities_v0 c; '
                  b'tny_capabilities_init(&c); '
                  b'return (int)c.abi_version; }\n'), check=True)

        cxx = os.environ.get("CXX", "c++")
        subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I" + os.path.join(prefix, "include"), "-x", "c++",
                        "-fsyntax-only", "-"],
                       input=(b'#include <tny/tny.h>\n'
                              b'int main() { tny_capabilities_v0 c; '
                              b'tny_capabilities_init(&c); '
                              b'return int(c.abi_version); }\n'),
                       check=True)
    exe = os.path.join(ROOT, "build", "libtny-embed-test")
    rpath = libdir if sys.platform == "darwin" else "$ORIGIN/lib"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I" + os.path.join(ROOT, "include"),
        os.path.join(ROOT, "examples", "embed.c"),
        "-L" + libdir, "-ltny", "-Wl,-rpath," + rpath, "-o", exe,
    ], check=True)

    stage("multi-runtime teardown and fork rejection")
    with tempfile.TemporaryDirectory() as root:
        workspace_a = os.path.join(root, "teardown-a")
        workspace_b = os.path.join(root, "teardown-b")
        os.makedirs(workspace_a)
        os.makedirs(workspace_b)
        stress_independent_teardown(libpath, workspace_a, workspace_b)
        if not core_only:
            verify_fork_rejection(libpath, workspace_a)

    if core_only:
        print("test_libtny: core ABI and ownership consumers passed")
        return

    stage("strict mock turns and ephemeral state")
    mock, port = start_mock(MOCK_EXPECT_WIRE="responses",
                            MOCK_REJECT_INSTRUCTIONS="HOME-SECRET")
    try:
        with tempfile.TemporaryDirectory() as root:
            workspace = os.path.join(root, "workspace")
            state = os.path.join(root, "state")
            host_home = os.path.join(root, "host-home")
            os.makedirs(workspace)
            os.makedirs(os.path.join(host_home, ".tny"))
            skill_dir = os.path.join(host_home, ".codex", "skills", "leak")
            os.makedirs(skill_dir)
            open(os.path.join(host_home, ".tny", "AGENTS.md"), "w").write(
                "HOME-SECRET must never enter an embedded prompt\n")
            open(os.path.join(skill_dir, "SKILL.md"), "w").write(
                "---\nname: leak\ndescription: HOME-SECRET skill metadata\n---\n"
                "HOME-SECRET body\n")
            for name in ("a.txt", "b.txt", "c.txt"):
                open(os.path.join(workspace, name), "w").write("x\n")
            run = subprocess.run([
                exe, f"http://127.0.0.1:{port}/v1", workspace, state,
            ], env=dict(os.environ, HOME=host_home),
               capture_output=True, text=True, timeout=30)
            assert run.returncode == 0, run.stderr
            assert "MOCK-OK" in run.stdout, run.stdout
            assert "test-key-not-real" not in run.stdout + run.stderr
            old_home = os.environ.get("HOME")
            os.environ["HOME"] = host_home
            try:
                output, permission, stop, errors = run_ctypes(
                    libpath, f"http://127.0.0.1:{port}/v1", workspace, state,
                    repeat=True)
            finally:
                if old_home is None:
                    os.environ.pop("HOME", None)
                else:
                    os.environ["HOME"] = old_home
            assert b"MOCK-OK" in output and not permission and stop == 0
            assert not errors

            ephemeral_workspace = os.path.join(root, "ephemeral-workspace")
            ephemeral_home = os.path.join(root, "ephemeral-home")
            explicit_ephemeral_state = os.path.join(root, "ephemeral-state")
            os.makedirs(ephemeral_workspace)
            os.makedirs(ephemeral_home)
            before = set(os.listdir(ephemeral_workspace))
            old_home = os.environ.get("HOME")
            os.environ["HOME"] = ephemeral_home
            try:
                output, permission, stop, errors = run_ctypes(
                    libpath, f"http://127.0.0.1:{port}/v1",
                    ephemeral_workspace, None, persistence=0)
                run_ctypes(libpath, f"http://127.0.0.1:{port}/v1",
                           ephemeral_workspace, explicit_ephemeral_state,
                           persistence=0)
            finally:
                if old_home is None:
                    os.environ.pop("HOME", None)
                else:
                    os.environ["HOME"] = old_home
            assert b"MOCK-OK" in output and not permission and stop == 0
            assert not errors
            assert not os.path.exists(explicit_ephemeral_state)
            assert not os.listdir(ephemeral_home)
            assert set(os.listdir(ephemeral_workspace)) == before
    finally:
        stop_mock(mock)

    # Keep one real truncated-terminal shield. The response is logically
    # complete, but its chunked transport is known dead and must be discarded
    # before the tool-output POST rather than retained as a stale keep-alive.
    stage("abrupt terminal transport close")
    mock, port = start_mock(MOCK_EXPECT_WIRE="responses",
                            MOCK_TRUNCATED_TERMINAL="1")
    try:
        with tempfile.TemporaryDirectory() as root:
            workspace = os.path.join(root, "workspace")
            state = os.path.join(root, "state")
            os.makedirs(workspace)
            started = time.monotonic()
            output, permission, stop, errors = run_ctypes(
                libpath, f"http://127.0.0.1:{port}/v1", workspace, state)
            elapsed = time.monotonic() - started
            assert b"MOCK-OK" in output and not permission and stop == 0
            assert not errors
            assert elapsed < TURN_TIMEOUT
            print(f"test_libtny: terminal close settled in {elapsed:.3f}s",
                  file=sys.stderr, flush=True)
    finally:
        stop_mock(mock)

    # Two owner threads drive isolated runtimes at the same time. Distinct
    # endpoints, workspaces, credentials and state roots must not interfere.
    stage("simultaneous owner threads")
    started_mocks = []
    try:
        for _ in range(2):
            started_mocks.append(
                start_mock(MOCK_EXPECT_WIRE="responses", MOCK_SLOW_MS="150"))
        mocks = [item[0] for item in started_mocks]
        ports = [item[1] for item in started_mocks]
        with tempfile.TemporaryDirectory() as root:
            results = [None, None]
            def drive(index):
                workspace = os.path.join(root, f"workspace-{index}")
                state = os.path.join(root, f"state-{index}")
                os.makedirs(workspace)
                results[index] = run_ctypes(
                    libpath, f"http://127.0.0.1:{ports[index]}/v1",
                    workspace, state, api_key=f"runtime-{index}-key")
            owners = [threading.Thread(target=drive, args=(i,), daemon=True)
                      for i in range(2)]
            for owner in owners:
                owner.start()
            for owner in owners:
                owner.join(timeout=20)
                assert not owner.is_alive()
            for result in results:
                output, permission, stop, errors = result
                assert b"MOCK-OK" in output and not permission and stop == 0
                assert not errors
    finally:
        for process, _port in started_mocks:
            stop_mock(process)

    # A scheduler-thread cancel must interrupt a blocking 5-second provider
    # wait promptly and settle with exactly one interrupted terminal.
    stage("cross-thread cancellation wake")
    mock, port = start_mock(MOCK_EXPECT_WIRE="responses", MOCK_SLOW_MS="5000")
    try:
        with tempfile.TemporaryDirectory() as root:
            workspace = os.path.join(root, "workspace")
            state = os.path.join(root, "state")
            os.makedirs(workspace)
            run_cancel_wake(libpath, f"http://127.0.0.1:{port}/v1",
                            workspace, state)
    finally:
        stop_mock(mock)

    # Public ask mode parks a sensitive native call until the embedder answers.
    stage("permission parking")
    mock, port = start_mock(MOCK_EXPECT_WIRE="responses", MOCK_SENSITIVE="1")
    try:
        with tempfile.TemporaryDirectory() as root:
            workspace = os.path.join(root, "workspace")
            state = os.path.join(root, "state")
            os.makedirs(workspace)
            output, permission, stop, errors = run_ctypes(
                libpath,
                f"http://127.0.0.1:{port}/v1", workspace, state)
            assert permission and not output and stop == 2 and not errors
            assert not os.path.exists(os.path.join(workspace, "permission.txt"))
    finally:
        stop_mock(mock)

    # Post-start authentication failures are typed ERROR events followed by
    # exactly one error terminal, not an unclassified string or sync failure.
    stage("asynchronous authentication error")
    mock, port = start_mock(MOCK_HTTP_STATUS="401")
    try:
        with tempfile.TemporaryDirectory() as root:
            workspace = os.path.join(root, "workspace")
            state = os.path.join(root, "state")
            os.makedirs(workspace)
            output, permission, stop, errors = run_ctypes(
                libpath, f"http://127.0.0.1:{port}/v1", workspace, state)
            assert not output and not permission and stop == 4
            assert errors == [-6], errors
    finally:
        stop_mock(mock)

    # A failed ordinary connection attempt updates reachability without the
    # capability query itself probing the endpoint.
    stage("unreachable endpoint capability")
    dead_port = free_port()
    with tempfile.TemporaryDirectory() as root:
        workspace = os.path.join(root, "workspace")
        state = os.path.join(root, "state")
        os.makedirs(workspace)
        _, _, stop, errors = run_ctypes(
            libpath, f"http://127.0.0.1:{dead_port}/v1", workspace, state)
        assert errors == [-7]
        assert stop == 4
    print("test_libtny: C and Python ctypes consumers passed")


if __name__ == "__main__":
    faulthandler.dump_traceback_later(SUITE_TIMEOUT, exit=True)
    try:
        main()
    finally:
        faulthandler.cancel_dump_traceback_later()
