#!/usr/bin/env python3
"""Exhaustive, process-isolated libtny allocation-failure checks.

The test-only shared library exposes the allocation count and injection state
of the current named public-call scope. A clean discovery run establishes the
exact high-water mark for each scenario, then a fresh child sweeps every
reachable allocation index. Children communicate only through a private
report file; any host stdout/stderr byte, signal, timeout, or missed injection
fails the parent.
"""

import ctypes
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

from test_libtny import as_bytes, load_lib, runtime_options  # noqa: E402

OOM = -4
EVENT = 1
TIMEOUT = 2
DRAINED = 3
PERMISSION_KIND = 4
TERMINAL_KIND = 7
ERROR_KIND = 8

# Stream coalescing makes a few tail allocations rare (the delivery probe
# reached index 308 in only 1/32 targeted fresh processes). Keep the complete
# discovered high-water mark and a bounded opportunity to reach those paths.
# Only a missed injection is retried; any injected failure is still fatal.
MISSED_INJECTION_ATTEMPTS = 512


def die(code):
    os._exit(code)


def free_error(lib, error):
    if error.value:
        lib.tny_error_free(error)
        error.value = None


def instrument(lib):
    byte_type = type(as_bytes("")[1])
    lib.tny_alloc_test_scope_count.argtypes = []
    lib.tny_alloc_test_scope_count.restype = ctypes.c_size_t
    lib.tny_alloc_test_scope_injected.argtypes = []
    lib.tny_alloc_test_scope_injected.restype = ctypes.c_bool
    for name in ("settlement_count", "settlement_allocations"):
        function = getattr(lib, "tny_alloc_test_" + name)
        function.argtypes = []
        function.restype = ctypes.c_size_t
    lib.tny_tools_test_walk.argtypes = [ctypes.c_char_p]
    lib.tny_tools_test_walk.restype = ctypes.c_int
    lib.tny_session_steer.argtypes = [
        ctypes.c_void_p,
        byte_type,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_session_steer.restype = ctypes.c_int32
    lib.tny_session_id.argtypes = [ctypes.c_void_p]
    lib.tny_session_id.restype = byte_type
    lib.tny_toolkit_job_create.argtypes = [
        byte_type,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_toolkit_job_create.restype = ctypes.c_int32
    lib.tny_toolkit_job_run.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_toolkit_job_run.restype = ctypes.c_int32
    lib.tny_toolkit_job_destroy.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
    lib.tny_toolkit_job_destroy.restype = ctypes.c_int32


def observe(lib, stats):
    stats[0] = max(stats[0], int(lib.tny_alloc_test_scope_count()))
    injected = bool(lib.tny_alloc_test_scope_injected())
    stats[1] = stats[1] or injected
    return injected


def write_report(path, stats):
    with open(path, "w", encoding="ascii") as report:
        report.write(f"{stats[0]} {int(stats[1])}\n")


def create_pair(lib, base_url, root, persistence=0, create_session=True):
    workspace = os.path.join(root, "workspace")
    state = os.path.join(root, "state")
    os.makedirs(workspace, exist_ok=True)
    os.makedirs(state, exist_ok=True)
    opts, keep = runtime_options(
        lib,
        base_url,
        workspace,
        state,
        api_key="fault-test-key",
        persistence=persistence,
    )
    runtime = ctypes.c_void_p()
    session = ctypes.c_void_p()
    error = ctypes.c_void_p()
    if (
        lib.tny_runtime_create(
            ctypes.byref(opts),
            ctypes.sizeof(opts),
            ctypes.byref(runtime),
            ctypes.byref(error),
        )
        != 0
        or not runtime.value
    ):
        die(20)
    free_error(lib, error)
    if not create_session:
        return runtime, session, error, keep
    if (
        lib.tny_session_create(runtime, ctypes.byref(session), ctypes.byref(error)) != 0
        or not session.value
    ):
        die(21)
    free_error(lib, error)
    return runtime, session, error, keep


def seed_walk_workspace(root):
    """Make the default list/glob batch exercise every walker path level."""
    workspace = os.path.join(root, "workspace")
    nested = os.path.join(workspace, "nested")
    os.makedirs(nested, exist_ok=True)
    with open(os.path.join(workspace, "root.txt"), "w", encoding="ascii") as value:
        value.write("root\n")
    with open(os.path.join(nested, "child.txt"), "w", encoding="ascii") as value:
        value.write("child\n")


def next_event(lib, session, error, stats=None):
    event = ctypes.c_void_p()
    status = lib.tny_session_next_event(
        session, 5000, ctypes.byref(event), ctypes.byref(error)
    )
    assert lib.tny_alloc_test_settlement_allocations() == 0, (
        "allocation during reserved OOM settlement"
    )
    if stats is not None:
        observe(lib, stats)
    return status, event


def drain(
    lib,
    session,
    error,
    stats=None,
    expect_oom=None,
    require_oom_if_injected=False,
    expect_success=False,
):
    kinds = []
    errors = []
    stops = []
    for _ in range(128):
        status, event = next_event(lib, session, error, stats)
        if status == DRAINED:
            break
        if status == TIMEOUT:
            continue
        if status != EVENT or not event.value:
            die(30)
        kind = lib.tny_event_get_kind(event)
        kinds.append(kind)
        if kind == ERROR_KIND:
            errors.append(lib.tny_event_error_code(event))
        if kind == TERMINAL_KIND:
            stops.append(lib.tny_event_stop_reason(event))
        lib.tny_event_free(event)
    else:
        die(31)
    if kinds.count(TERMINAL_KIND) != 1 or kinds[-1] != TERMINAL_KIND:
        die(32)
    saw_oom = OOM in errors
    if saw_oom and kinds[-2:] != [ERROR_KIND, TERMINAL_KIND]:
        die(33)
    if expect_oom is not None and saw_oom != expect_oom:
        die(34)
    if require_oom_if_injected and (stats is None or stats[1] != saw_oom):
        die(35)
    if expect_success:
        assert errors == [] and stops == [0], (errors, stops)
    return saw_oom


def wait_permission(lib, session, error):
    for _ in range(128):
        status, event = next_event(lib, session, error)
        if status != EVENT or not event.value:
            die(40)
        if lib.tny_event_get_kind(event) == PERMISSION_KIND:
            value = lib.tny_event_permission_id(event)
            permission = ctypes.string_at(value.ptr, value.len).decode()
            lib.tny_event_free(event)
            return permission
        lib.tny_event_free(event)
    die(41)


def make_persisted_session(lib, base_url, root):
    runtime, session, error, keep = create_pair(lib, base_url, root, persistence=1)
    raw, prompt = as_bytes("persist before session_open fault sweep")
    keep.append(raw)
    if lib.tny_session_send(session, prompt, ctypes.byref(error)) != 0:
        die(42)
    drain(lib, session, error)
    value = lib.tny_session_id(session)
    session_id = ctypes.string_at(value.ptr, value.len).decode()
    lib.tny_session_free(session)
    lib.tny_runtime_free(runtime)
    return session_id, keep


def child_case(libpath, scenario, base_url, report_path):
    lib = load_lib(libpath)
    instrument(lib)
    stats = [0, False]
    with tempfile.TemporaryDirectory() as root:
        if scenario in ("toolkit_create", "toolkit_run"):
            # Unknown provider stops before credentials/network/audio; every
            # context/JSON allocation still belongs to the public OOM scope.
            settings = os.path.join(root, "settings.json")
            with open(settings, "w", encoding="ascii") as value:
                value.write("{}")
            raw, request = as_bytes(
                json.dumps(
                    {
                        "version": 1,
                        "operation": "speak",
                        "config": {"workspace": root, "settings_path": settings},
                        "request": {
                            "text": "fixture text",
                            "provider": "fixture-unsupported",
                            "output_file": "speech.mp3",
                        },
                    }
                )
            )
            job = ctypes.c_void_p()
            error = ctypes.c_void_p()
            rc = lib.tny_toolkit_job_create(
                request, ctypes.byref(job), ctypes.byref(error)
            )
            if scenario == "toolkit_run":
                if rc or not job.value:
                    die(54)
                rc = lib.tny_toolkit_job_run(job, ctypes.byref(error))
            injected = observe(lib, stats)
            expected = OOM if injected else (0 if scenario == "toolkit_create" else -9)
            if rc != expected or (
                scenario == "toolkit_create" and injected and job.value
            ):
                die(55)
            free_error(lib, error)
            if lib.tny_toolkit_job_destroy(ctypes.byref(job)) != 0 or job.value:
                die(56)
            del raw

        elif scenario == "tools_fs_walk":
            seed_walk_workspace(root)
            workspace = os.path.join(root, "workspace").encode()
            rc = lib.tny_tools_test_walk(workspace)
            injected = observe(lib, stats)
            if (injected and rc != -1) or (not injected and rc != 2):
                die(53)

        elif scenario in ("tool_register", "tool_error"):
            from test_libtny_custom_tools import INVOKE, TnyBytes, ToolSpec

            workspace = os.path.join(root, "workspace")
            os.makedirs(workspace)
            opts, keep = runtime_options(lib, base_url, workspace, None, persistence=0)
            runtime = ctypes.c_void_p()
            error = ctypes.c_void_p()
            assert (
                lib.tny_runtime_create(
                    ctypes.byref(opts),
                    ctypes.sizeof(opts),
                    ctypes.byref(runtime),
                    ctypes.byref(error),
                )
                == 0
            )
            lib.tny_runtime_register_tool.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ToolSpec),
                ctypes.POINTER(ctypes.c_void_p),
                ctypes.POINTER(ctypes.c_void_p),
            ]
            lib.tny_runtime_register_tool.restype = ctypes.c_int32
            spec = ToolSpec()
            spec.abi_version = 1
            spec.struct_size = ctypes.sizeof(spec)
            spec.name = TnyBytes(
                b"list_files" if scenario == "tool_error" else b"fault_tool", 10
            )
            description = b"copied metadata beyond small string capacity"
            schema = b'{"type":"object","properties":{"value":{"type":"string"}}}'
            spec.description = TnyBytes(description, len(description))
            spec.input_schema_json = TnyBytes(schema, len(schema))
            callback = INVOKE(lambda *_args: 0)
            spec.invoke = callback
            registration = ctypes.c_void_p()
            rc = lib.tny_runtime_register_tool(
                runtime,
                ctypes.byref(spec),
                ctypes.byref(registration),
                ctypes.byref(error),
            )
            injected = observe(lib, stats)
            expected = -1 if scenario == "tool_error" else 0
            if (injected and (rc != OOM or registration.value)) or (
                not injected
                and (rc != expected or bool(registration.value) != (expected == 0))
            ):
                die(57)
            free_error(lib, error)
            if injected:
                os.environ["TNY_TEST_ALLOC_SCOPE"] = "disabled"
                spec.name = TnyBytes(b"fault_tool", 10)
                if (
                    lib.tny_runtime_register_tool(
                        runtime,
                        ctypes.byref(spec),
                        ctypes.byref(registration),
                        ctypes.byref(error),
                    )
                    != 0
                ):
                    die(58)
            lib.tny_runtime_free(runtime)
            del keep, callback

        elif scenario == "runtime_create":
            workspace = os.path.join(root, "workspace")
            state = os.path.join(root, "state")
            os.makedirs(workspace)
            opts, keep = runtime_options(
                lib, base_url, workspace, state, api_key="fault-test-key"
            )
            runtime = ctypes.c_void_p()
            error = ctypes.c_void_p()
            rc = lib.tny_runtime_create(
                ctypes.byref(opts),
                ctypes.sizeof(opts),
                ctypes.byref(runtime),
                ctypes.byref(error),
            )
            injected = observe(lib, stats)
            if (injected and (rc != OOM or runtime.value)) or (
                not injected and (rc != 0 or not runtime.value)
            ):
                die(10)
            free_error(lib, error)
            if runtime.value:
                lib.tny_runtime_free(runtime)
            del keep

        elif scenario == "session_create":
            workspace = os.path.join(root, "workspace")
            state = os.path.join(root, "state")
            os.makedirs(workspace)
            opts, keep = runtime_options(
                lib, base_url, workspace, state, api_key="fault-test-key"
            )
            runtime = ctypes.c_void_p()
            session = ctypes.c_void_p()
            error = ctypes.c_void_p()
            if (
                lib.tny_runtime_create(
                    ctypes.byref(opts),
                    ctypes.sizeof(opts),
                    ctypes.byref(runtime),
                    ctypes.byref(error),
                )
                != 0
            ):
                die(11)
            rc = lib.tny_session_create(
                runtime, ctypes.byref(session), ctypes.byref(error)
            )
            injected = observe(lib, stats)
            if (injected and (rc != OOM or session.value)) or (
                not injected and (rc != 0 or not session.value)
            ):
                die(12)
            free_error(lib, error)
            if session.value:
                lib.tny_session_free(session)
            lib.tny_runtime_free(runtime)
            del keep

        elif scenario == "session_open":
            session_id, keep = make_persisted_session(lib, base_url, root)
            workspace = os.path.join(root, "workspace")
            state = os.path.join(root, "state")
            opts, more_keep = runtime_options(
                lib, base_url, workspace, state, api_key="fault-test-key"
            )
            keep.extend(more_keep)
            runtime = ctypes.c_void_p()
            session = ctypes.c_void_p()
            error = ctypes.c_void_p()
            if (
                lib.tny_runtime_create(
                    ctypes.byref(opts),
                    ctypes.sizeof(opts),
                    ctypes.byref(runtime),
                    ctypes.byref(error),
                )
                != 0
            ):
                die(13)
            raw_id, session_view = as_bytes(session_id)
            keep.append(raw_id)
            rc = lib.tny_session_open(
                runtime, session_view, ctypes.byref(session), ctypes.byref(error)
            )
            injected = observe(lib, stats)
            if (injected and (rc != OOM or session.value)) or (
                not injected and (rc != 0 or not session.value)
            ):
                die(14)
            free_error(lib, error)
            if session.value:
                lib.tny_session_free(session)
            lib.tny_runtime_free(runtime)

        elif scenario in (
            "session_send",
            "session_send_rearm",
            "next_event",
            "session_steer",
            "respond_permission",
        ):
            runtime, session, error, keep = create_pair(lib, base_url, root)
            if scenario == "session_send_rearm":
                target_scope = os.environ.get("TNY_TEST_ALLOC_SCOPE", "")
                target_index = os.environ.get("TNY_TEST_ALLOC_FAIL_AT")
                os.environ["TNY_TEST_ALLOC_SCOPE"] = "next_event"
                os.environ["TNY_TEST_ALLOC_FAIL_AT"] = "1"
                raw, prompt = as_bytes("consume oom reserves before rearm")
                keep.append(raw)
                if lib.tny_session_send(session, prompt, ctypes.byref(error)) != 0:
                    die(43)
                drain(lib, session, error, expect_oom=True)
                os.environ["TNY_TEST_ALLOC_SCOPE"] = target_scope
                if target_index is None:
                    os.environ.pop("TNY_TEST_ALLOC_FAIL_AT", None)
                else:
                    os.environ["TNY_TEST_ALLOC_FAIL_AT"] = target_index

            if scenario in ("session_send", "session_send_rearm"):
                raw, prompt = as_bytes("allocation fault turn")
                keep.append(raw)
                rc = lib.tny_session_send(session, prompt, ctypes.byref(error))
                injected = observe(lib, stats)
                if rc not in (0, OOM) or (injected and rc == 0 and error.value):
                    die(15)
                free_error(lib, error)
                if rc == 0:
                    drain(lib, session, error, expect_oom=injected)
                elif not injected:
                    die(44)

            elif scenario == "next_event":
                # The strict mock requests list_files followed by glob_files.
                # Seed a nested tree so the exhaustive allocation sweep covers
                # root, entry-name, absolute-path, and recursive walk failures.
                seed_walk_workspace(root)
                raw, prompt = as_bytes("allocation fault event")
                keep.append(raw)
                if lib.tny_session_send(session, prompt, ctypes.byref(error)) != 0:
                    die(16)
                drain(lib, session, error, stats=stats, require_oom_if_injected=True)

            elif scenario == "session_steer":
                raw, prompt = as_bytes("slow active turn")
                keep.append(raw)
                if lib.tny_session_send(session, prompt, ctypes.byref(error)) != 0:
                    die(17)
                raw_steer, steer = as_bytes("follow-up steering")
                keep.append(raw_steer)
                rc = lib.tny_session_steer(session, steer, ctypes.byref(error))
                injected = observe(lib, stats)
                if rc not in (0, OOM):
                    die(18)
                free_error(lib, error)
                if rc == OOM:
                    os.environ["TNY_TEST_ALLOC_SCOPE"] = "disabled"
                    if lib.tny_session_cancel(session, ctypes.byref(error)) != 0:
                        die(19)
                drain(lib, session, error, expect_oom=injected if rc == 0 else False)

            else:
                raw, prompt = as_bytes("request a sensitive operation")
                keep.append(raw)
                if lib.tny_session_send(session, prompt, ctypes.byref(error)) != 0:
                    die(22)
                permission = wait_permission(lib, session, error)
                raw_id, permission_id = as_bytes(permission)
                keep.append(raw_id)
                rc = lib.tny_session_respond_permission(
                    session, permission_id, 2, ctypes.byref(error)
                )
                injected = observe(lib, stats)
                if rc not in (0, OOM):
                    die(23)
                free_error(lib, error)
                if rc == OOM:
                    os.environ["TNY_TEST_ALLOC_SCOPE"] = "disabled"
                    if (
                        lib.tny_session_respond_permission(
                            session, permission_id, 2, ctypes.byref(error)
                        )
                        != 0
                    ):
                        die(24)
                drain(lib, session, error, expect_oom=injected if rc == 0 else False)
            lib.tny_session_free(session)
            lib.tny_runtime_free(runtime)

        elif scenario in (
            "session_free_active",
            "runtime_free_active",
            "session_free_cancelled",
            "runtime_free_cancelled",
        ):
            target_session = scenario.startswith("session_")
            for cycle in range(3):
                cycle_root = os.path.join(root, str(cycle))
                os.makedirs(cycle_root)
                runtime, session, error, keep = create_pair(lib, base_url, cycle_root)
                raw, prompt = as_bytes("slow active teardown")
                keep.append(raw)
                if lib.tny_session_send(session, prompt, ctypes.byref(error)) != 0:
                    die(25)
                if scenario.endswith("_cancelled"):
                    if lib.tny_session_cancel(session, ctypes.byref(error)) != 0:
                        die(45)
                    free_error(lib, error)
                started = time.monotonic()
                if target_session:
                    lib.tny_session_free(session)
                else:
                    lib.tny_runtime_free(runtime)
                observe(lib, stats)
                if time.monotonic() - started > 1.0:
                    die(26)
                if target_session:
                    lib.tny_runtime_free(runtime)

        elif scenario in ("session_free_permission", "runtime_free_permission"):
            target_session = scenario.startswith("session_")
            runtime, session, error, keep = create_pair(lib, base_url, root)
            raw, prompt = as_bytes("request a sensitive operation")
            keep.append(raw)
            if lib.tny_session_send(session, prompt, ctypes.byref(error)) != 0:
                die(46)
            wait_permission(lib, session, error)
            started = time.monotonic()
            if target_session:
                lib.tny_session_free(session)
            else:
                lib.tny_runtime_free(runtime)
            observe(lib, stats)
            if time.monotonic() - started > 1.0:
                die(47)
            if target_session:
                lib.tny_runtime_free(runtime)

        elif scenario in ("session_free_failed", "runtime_free_failed"):
            target_session = scenario.startswith("session_")
            runtime, session, error, keep = create_pair(lib, base_url, root)
            raw, prompt = as_bytes("unreachable teardown")
            keep.append(raw)
            rc = lib.tny_session_send(session, prompt, ctypes.byref(error))
            if rc == 0:
                drain(lib, session, error)
            free_error(lib, error)
            if target_session:
                lib.tny_session_free(session)
            else:
                lib.tny_runtime_free(runtime)
            observe(lib, stats)
            if target_session:
                lib.tny_runtime_free(runtime)

        else:
            die(27)

    write_report(report_path, stats)


def child_repeat_oom(libpath, base_url):
    lib = load_lib(libpath)
    instrument(lib)
    with tempfile.TemporaryDirectory() as root:
        runtime, session, error, keep = create_pair(lib, base_url, root)
        for turn in range(2):
            raw, prompt = as_bytes(f"repeat oom turn {turn}")
            keep.append(raw)
            if lib.tny_session_send(session, prompt, ctypes.byref(error)) != 0:
                die(50)
            drain(lib, session, error, expect_oom=True)
        os.environ["TNY_TEST_ALLOC_SCOPE"] = "disabled"
        raw, prompt = as_bytes("successful retry after two oom turns")
        keep.append(raw)
        if lib.tny_session_send(session, prompt, ctypes.byref(error)) != 0:
            die(52)
        drain(lib, session, error, expect_oom=False, expect_success=True)
        lib.tny_session_free(session)
        lib.tny_runtime_free(runtime)


def child_reserved_settlement(libpath, base_url, scenario):
    from test_libtny_custom_tools import INVOKE, TnyBytes, ToolResult, ToolSpec

    lib = load_lib(libpath)
    instrument(lib)
    retained = []

    @INVOKE
    def pending(_user, call, generation, _arguments, _result):
        retained.append((call, generation))
        return 1  # ASYNC transfers a host lease to this fixture.

    with tempfile.TemporaryDirectory() as root:
        runtime, session, error, keep = create_pair(
            lib, base_url, root, persistence=1, create_session=scenario != "custom"
        )
        if scenario == "custom":
            lib.tny_runtime_register_tool.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ToolSpec),
                ctypes.POINTER(ctypes.c_void_p),
                ctypes.POINTER(ctypes.c_void_p),
            ]
            spec = ToolSpec()
            spec.abi_version = 1
            spec.struct_size = ctypes.sizeof(spec)
            spec.name = TnyBytes(b"host_pending", 12)
            spec.description = TnyBytes(b"pending OOM fixture", 19)
            spec.input_schema_json = TnyBytes(b'{"type":"object"}', 17)
            spec.invoke = pending
            registration = ctypes.c_void_p()
            assert (
                lib.tny_runtime_register_tool(
                    runtime,
                    ctypes.byref(spec),
                    ctypes.byref(registration),
                    ctypes.byref(error),
                )
                == 0
            )
            lib.tny_tool_call_complete.argtypes = [
                ctypes.c_void_p,
                ctypes.c_uint64,
                ctypes.POINTER(ToolResult),
                ctypes.POINTER(ctypes.c_void_p),
            ]
            lib.tny_tool_call_release.argtypes = [ctypes.c_void_p]
            assert (
                lib.tny_session_create(
                    runtime, ctypes.byref(session), ctypes.byref(error)
                )
                == 0
            )

        for turn in range(2):
            raw, prompt = as_bytes(f"reserved {scenario} {turn}")
            keep.append(raw)
            assert lib.tny_session_send(session, prompt, ctypes.byref(error)) == 0
            kinds = []
            for _ in range(128):
                event = ctypes.c_void_p()
                status = lib.tny_session_next_event(
                    session, 20, ctypes.byref(event), ctypes.byref(error)
                )
                if status == EVENT:
                    kind = lib.tny_event_get_kind(event)
                    kinds.append(kind)
                    lib.tny_event_free(event)
                    assert kind not in (ERROR_KIND, TERMINAL_KIND)
                    if (scenario in ("text", "later") and kind == 0) or (
                        scenario == "permission" and kind == PERMISSION_KIND
                    ):
                        break
                elif status != TIMEOUT:
                    raise AssertionError((scenario, status))
                if scenario == "custom" and retained:
                    break
            else:
                raise AssertionError(
                    ("provider never reached parked state", scenario, kinds)
                )

            snapshot = {
                path: path.read_bytes() for path in Path(root, "state").rglob("*.json")
            }
            if scenario == "later":
                # The second response is parked after text, with first-step
                # usage already accumulated and persisted. Grow its SSE buffer.
                os.environ["TNY_TEST_ALLOC_SCOPE"] = "next_event"
                os.environ["TNY_TEST_ALLOC_FAIL_AT"] = "1"
                with urllib.request.urlopen(base_url + "/release", timeout=5) as reply:
                    assert reply.status == 200
                for _ in range(128):
                    status, event = next_event(lib, session, error)
                    if status == EVENT:
                        kind = lib.tny_event_get_kind(event)
                        kinds.append(kind)
                        assert kind == ERROR_KIND
                        assert lib.tny_event_error_code(event) == OOM
                        lib.tny_event_free(event)
                        break
                    assert status == TIMEOUT
                else:
                    raise AssertionError("later response never failed")
                assert lib.tny_alloc_test_scope_injected()
                assert lib.tny_alloc_test_settlement_count() >= 1
                assert snapshot
                assert all(path.read_bytes() == data for path, data in snapshot.items())
            else:
                # Allocation 1 copies input; 2 grows the effective prompt.
                os.environ["TNY_TEST_ALLOC_SCOPE"] = "session_steer"
                os.environ["TNY_TEST_ALLOC_FAIL_AT"] = "2"
                raw, steer = as_bytes("cancel through injected OOM")
                keep.append(raw)
                assert lib.tny_session_steer(session, steer, ctypes.byref(error)) == 0
                assert lib.tny_alloc_test_scope_injected()
                assert lib.tny_alloc_test_settlement_count() == 1
            assert lib.tny_alloc_test_settlement_allocations() == 0, (
                "allocation during reserved OOM settlement"
            )
            os.environ["TNY_TEST_ALLOC_SCOPE"] = "disabled"
            errors, terminals = ([OOM] if scenario == "later" else []), []
            for _ in range(128):
                status, event = next_event(lib, session, error)
                # Every remaining event is already owned; public delivery and
                # terminal bookkeeping must also make zero allocation attempts.
                assert lib.tny_alloc_test_scope_count() == 0
                if status == DRAINED:
                    break
                assert status == EVENT
                kind = lib.tny_event_get_kind(event)
                kinds.append(kind)
                if kind == ERROR_KIND:
                    errors.append(lib.tny_event_error_code(event))
                if kind == TERMINAL_KIND:
                    terminals.append(lib.tny_event_stop_reason(event))
                lib.tny_event_free(event)
            assert errors == [OOM], errors
            assert terminals == [4], terminals  # TNY_STOP_ERROR
            assert kinds[-2:] == [ERROR_KIND, TERMINAL_KIND]
            if retained:
                call, generation = retained.pop()
                result = ToolResult()
                result.abi_version = 1
                result.struct_size = ctypes.sizeof(result)
                result.data = TnyBytes(b"late", 4)
                assert (
                    lib.tny_tool_call_complete(
                        call, generation, ctypes.byref(result), None
                    )
                    == -2
                )
                lib.tny_tool_call_release(call)

        raw, prompt = as_bytes("successful retry after reserved settlement")
        keep.append(raw)
        assert lib.tny_session_send(session, prompt, ctypes.byref(error)) == 0
        drain(lib, session, error, expect_oom=False, expect_success=True)
        lib.tny_session_free(session)
        lib.tny_runtime_free(runtime)


def reserved_settlement_fixture(script, libpath, scenario):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_GET(self):
            self.server.release.set()
            self.send_response(200)
            self.end_headers()

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            items = request["input"]
            pending_ids = set()
            for item in items:
                if item.get("type") == "function_call":
                    assert item["call_id"] not in pending_ids
                    pending_ids.add(item["call_id"])
                elif item.get("type") == "function_call_output":
                    assert item["call_id"] in pending_ids
                    pending_ids.remove(item["call_id"])
                elif item.get("role") == "user":
                    assert not pending_ids, (
                        "unanswered tool call before later user turn"
                    )
            assert not pending_ids
            self.server.requests += 1
            retry = self.server.requests == (5 if scenario == "later" else 3)
            later_body = scenario == "later" and self.server.requests % 2 == 0
            events = [{"type": "response.output_text.delta", "delta": "partial answer"}]
            if scenario == "later" and not retry and not later_body:
                events = []
            if not retry and scenario != "text" and not later_body:
                name = "write_file" if scenario == "permission" else "host_pending"
                arguments = (
                    '{"path":"permission.txt","content":"allowed"}'
                    if scenario == "permission"
                    else "{}"
                )
                if scenario == "later":
                    name, arguments = "list_files", "{}"
                item = {
                    "type": "function_call",
                    "id": "fc_oom",
                    "call_id": f"oom_{self.server.requests}",
                    "name": name,
                    "arguments": arguments,
                }
                events += [
                    {
                        "type": "response.output_item.added",
                        "output_index": 0,
                        "item": item,
                    },
                    {
                        "type": "response.output_item.done",
                        "output_index": 0,
                        "item": item,
                    },
                ]
            if not later_body and (retry or scenario != "text"):
                events.append(
                    {
                        "type": "response.completed",
                        "response": {
                            "status": "completed",
                            "usage": {"input_tokens": 123, "output_tokens": 7},
                        },
                    }
                )
            body = "".join(
                "data: " + json.dumps(event) + "\n\n" for event in events
            ).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            if not later_body and (retry or scenario != "text"):
                self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if later_body:
                self.server.release.clear()
            self.wfile.write(body)
            self.wfile.flush()
            if later_body:
                assert self.server.release.wait(10)
                tail = (
                    b'data: {"type":"response.output_text.delta","delta":"'
                    + b"x" * 32768
                    + b'"}\n\n'
                )
                try:
                    self.wfile.write(tail)
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass
            if not retry and scenario == "text":
                # Keep a genuine streaming response open until OOM closes it.
                self.connection.settimeout(10)
                self.connection.recv(1)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.requests = 0
    server.release = threading.Event()
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        run_child(
            script,
            [
                "--reserved-settlement",
                libpath,
                f"http://127.0.0.1:{server.server_port}/v1",
                scenario,
            ],
            dict(os.environ),
        )
        assert server.requests == (5 if scenario == "later" else 3)
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def run_child(script, args, env, timeout=20):
    asan_runtime = env.get("TNY_TEST_ASAN_RUNTIME")
    if asan_runtime:
        if sys.platform == "darwin":
            env["DYLD_INSERT_LIBRARIES"] = asan_runtime
        else:
            runtimes = [asan_runtime]
            if cxx_runtime := env.get("TNY_TEST_CXX_RUNTIME"):
                runtimes.append(cxx_runtime)
            env["LD_PRELOAD"] = ":".join(runtimes)
    executable = env.get("TNY_TEST_PYTHON_EXEC", sys.executable)
    run = subprocess.run(
        [executable, script, *args],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )
    if run.returncode != 0 or run.stdout or run.stderr:
        raise AssertionError(
            f"child {args}: rc={run.returncode} "
            f"stdout={run.stdout!r} stderr={run.stderr!r}"
        )


def run_measured(script, libpath, scenario, scope, base_url, fail_at=None):
    # Socket reads may coalesce adjacent flushed HTTP chunks differently in a
    # fresh process. Retry only a not-reached index; an injected run still has
    # exactly one chance and remains release-blocking on any bad outcome.
    attempts = MISSED_INJECTION_ATTEMPTS if fail_at is not None else 1
    maximum = 0
    for _ in range(attempts):
        with tempfile.TemporaryDirectory() as report_dir:
            report = os.path.join(report_dir, "count")
            env = dict(os.environ)
            env["TNY_TEST_ALLOC_SCOPE"] = scope
            if fail_at is None:
                env.pop("TNY_TEST_ALLOC_FAIL_AT", None)
            else:
                env["TNY_TEST_ALLOC_FAIL_AT"] = str(fail_at)
            run_child(script, ["--child", libpath, scenario, base_url, report], env)
            with open(report, encoding="ascii") as value:
                count, injected = (int(item) for item in value.read().split())
            maximum = max(maximum, count)
            if fail_at is None or injected:
                return count
    raise AssertionError(
        f"{scenario} allocation {fail_at} was not reached in {attempts} "
        f"fresh processes (maximum observed {maximum})"
    )


def sweep(script, libpath, scenario, scope, base_url):
    # Preserve the highest observed index, including scheduling-dependent
    # buffer growth. run_measured retries only an index that was not injected;
    # it never retries an injected failure or silently drops the observed tail.
    count = max(
        run_measured(script, libpath, scenario, scope, base_url) for _ in range(4)
    )
    for index in range(1, count + 1):
        run_measured(script, libpath, scenario, scope, base_url, index)
    return count


def start_mock(**settings):
    port = free_port()
    settings.setdefault("MOCK_CONNECTION_CLOSE", "1")
    settings.setdefault("MOCK_CHUNK_WIDTH", "1048576")
    settings.setdefault("MOCK_EXPECT_WIRE", "responses")
    env = dict(os.environ, **settings)
    mock = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "mock_openai.py"), str(port)],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    if "ready" not in mock.stdout.readline().decode():
        mock.terminate()
        mock.wait(timeout=5)
        raise AssertionError("mock did not start")
    return mock, f"http://127.0.0.1:{port}/v1"


def sweep_turn_allocations(run, provider):
    """Require actual injection at every index observed in any discovery run."""
    maximum = max(run(0)[0] for _ in range(3))
    index = 1
    while index <= maximum:
        for _attempt in range(MISSED_INJECTION_ATTEMPTS):
            count, injected = run(index)
            if injected:
                break
            maximum = max(maximum, count)
        else:
            raise AssertionError(
                (provider, index, "discovered allocation index never injected")
            )
        index += 1
    return maximum


def provider_turn_sweeps(libpath):
    """Sweep every discovered index of each complete active mock turn.

    Admission/constructor indices remain covered by the public API sweeps.
    Unlike those per-call scopes, the native host retains one scope across
    all dispatches, tool requests and finalization, then checks a later turn.
    """
    host = Path(libpath).with_name("provider-faults")
    mock, url = start_mock(MOCK_REASONING="item")
    chat, chat_url = start_mock(MOCK_EXPECT_WIRE="chat", MOCK_REASONING="details")
    ws = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "fake_acp_agent_ws.py"), "0"],
        env=dict(os.environ, FAKE_ACP_COALESCE="1"),
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    ws_url = "ws://127.0.0.1:" + ws.stdout.readline().decode().strip().split()[-1]
    counts = {}
    try:
        providers = ("openai", "openai-chat", "cursor", "acp", "acp-ws")
        # macos-15 runners: python under a throwaway HOME exceeds the 30 s
        # cursor ready-line timeout. Local Darwin still runs the fixture.
        if os.environ.get("CI") and sys.platform == "darwin":
            providers = ("openai", "openai-chat", "acp", "acp-ws")
        for provider in providers:
            provider_url = (
                ws_url
                if provider == "acp-ws"
                else chat_url
                if provider == "openai-chat"
                else url
            )

            def run(index):
                with tempfile.TemporaryDirectory(prefix="tny-turn-sweep-") as root:
                    report = os.path.join(root, "report")
                    process = subprocess.run(
                        [
                            str(host),
                            "--turn-sweep",
                            provider,
                            str(index),
                            provider_url,
                            root,
                            report,
                        ],
                        capture_output=True,
                        timeout=30,
                    )
                    detail = (
                        Path(report).read_text()
                        if Path(report).exists()
                        else "no report"
                    )
                    assert (
                        process.returncode == 0
                        and not process.stdout
                        and not process.stderr
                    ), (
                        provider,
                        index,
                        process.returncode,
                        detail,
                        process.stdout.decode(errors="replace"),
                        process.stderr.decode(errors="replace"),
                    )
                    count, injected, settlements, after_failure, status = map(
                        int, detail.split()
                    )
                    assert status == 0 and after_failure == 0
                    if injected:
                        assert settlements >= 1
                    return count, injected

            maximum = sweep_turn_allocations(run, provider)
            counts[provider] = maximum
            print(f"provider active-turn sweep: {provider}={maximum}", flush=True)
    finally:
        mock.terminate()
        mock.wait(timeout=5)
        ws.terminate()
        ws.wait(timeout=5)
        chat.terminate()
        chat.wait(timeout=5)
    return counts


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--provider-sweeps-only":
        provider_turn_sweeps(os.path.abspath(sys.argv[2]))
        return
    if len(sys.argv) >= 2 and sys.argv[1] == "--reserved-settlement":
        child_reserved_settlement(sys.argv[2], sys.argv[3], sys.argv[4])
        return
    if len(sys.argv) >= 2 and sys.argv[1] == "--reserved-only":
        for scenario in ("text", "permission", "custom", "later"):
            reserved_settlement_fixture(
                os.path.abspath(__file__), os.path.abspath(sys.argv[2]), scenario
            )
        print("test_libtny_faults: real provider reserved settlement passed")
        return
    if len(sys.argv) >= 2 and sys.argv[1] == "--child":
        child_case(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
        return
    if len(sys.argv) >= 2 and sys.argv[1] == "--repeat-oom":
        child_repeat_oom(sys.argv[2], sys.argv[3])
        return
    if len(sys.argv) != 2 or "libtny" not in os.path.basename(sys.argv[1]):
        print("test_libtny_faults: skip (run make test-libtny-fault)")
        return

    libpath = os.path.abspath(sys.argv[1])
    script = os.path.abspath(__file__)
    results = {}
    native_host = Path(libpath).with_name("provider-faults")
    for test in (
        "decoder_oom_mid_stream",
        "emergency_cancel_reaps",
        "request_construction_oom",
        "error_decode_oom",
        "message_oom",
        "pending_completion_oom",
        "store_callback_exhaustive",
        "callback_thread_oom",
        "bridge_stderr_oom",
        "immediate_observe_recovery_exhaustive",
    ):
        subprocess.run([str(native_host), "-t", test], check=True, timeout=60)
    results.update(provider_turn_sweeps(libpath))
    for scenario in ("text", "permission", "custom", "later"):
        reserved_settlement_fixture(script, libpath, scenario)

    results["tools_fs_walk"] = sweep(
        script, libpath, "tools_fs_walk", "tools_fs_walk", "unused"
    )
    for scope in ("toolkit_create", "toolkit_run"):
        results[scope] = sweep(script, libpath, scope, scope, "unused")

    mock, base_url = start_mock(MOCK_SLOW_MS="150")
    try:
        for scenario, scope in (
            ("runtime_create", "runtime_create"),
            ("tool_register", "tool_register"),
            ("tool_error", "tool_register"),
            ("session_create", "session_create"),
            ("session_open", "session_open"),
            ("session_send", "session_send"),
            ("session_send_rearm", "session_send"),
            ("next_event", "next_event"),
            ("session_steer", "session_steer"),
        ):
            results[scenario] = sweep(script, libpath, scenario, scope, base_url)
        env = dict(
            os.environ, TNY_TEST_ALLOC_SCOPE="next_event", TNY_TEST_ALLOC_FAIL_AT="1"
        )
        run_child(script, ["--repeat-oom", libpath, base_url], env)
    finally:
        mock.terminate()
        mock.wait(timeout=5)

    sensitive, sensitive_url = start_mock(MOCK_SENSITIVE="1")
    try:
        results["respond_permission"] = sweep(
            script, libpath, "respond_permission", "respond_permission", sensitive_url
        )
        for scenario, scope in (
            ("session_free_permission", "session_free"),
            ("runtime_free_permission", "runtime_free"),
        ):
            results[scenario] = sweep(script, libpath, scenario, scope, sensitive_url)
    finally:
        sensitive.terminate()
        sensitive.wait(timeout=5)

    slow, slow_url = start_mock(MOCK_SLOW_MS="5000")
    try:
        for scenario, scope in (
            ("session_free_active", "session_free"),
            ("runtime_free_active", "runtime_free"),
            ("session_free_cancelled", "session_free"),
            ("runtime_free_cancelled", "runtime_free"),
        ):
            results[scenario] = sweep(script, libpath, scenario, scope, slow_url)
    finally:
        slow.terminate()
        slow.wait(timeout=5)

    unreachable_url = f"http://127.0.0.1:{free_port()}/v1"
    for scenario, scope in (
        ("session_free_failed", "session_free"),
        ("runtime_free_failed", "runtime_free"),
    ):
        results[scenario] = sweep(script, libpath, scenario, scope, unreachable_url)

    counts = ", ".join(f"{name}={count}" for name, count in results.items())
    print("test_libtny_faults: exhaustive allocation sweeps passed; " + counts)


if __name__ == "__main__":
    main()
