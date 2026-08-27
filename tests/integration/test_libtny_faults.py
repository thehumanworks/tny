#!/usr/bin/env python3
"""Process-isolated libtny allocation-failure and host-stdio checks.

The fault library is a test-only build.  Each public operation starts a named
allocation scope; one selected allocation returns NULL and all later
allocations proceed so cleanup and the reserved terminal path can be tested.
Every trial runs in a fresh process.  A signal, exit, or byte written to the
host's stdout/stderr fails the parent test.
"""

import ctypes
import os
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

from test_libtny import as_bytes, load_lib, runtime_options  # noqa: E402

OOM = -4
EVENT = 1
TIMEOUT = 2
DRAINED = 3
ERROR_KIND = 8
TERMINAL_KIND = 7


def free_error(lib, error):
    if error.value:
        lib.tny_error_free(error)
        error.value = None


def create_pair(lib, libpath, base_url, root):
    workspace = os.path.join(root, "workspace")
    state = os.path.join(root, "state")
    os.makedirs(workspace)
    opts, keep = runtime_options(
        lib, base_url, workspace, state, api_key="fault-test-key")
    runtime = ctypes.c_void_p()
    session = ctypes.c_void_p()
    error = ctypes.c_void_p()
    rc = lib.tny_runtime_create(
        ctypes.byref(opts), ctypes.byref(runtime), ctypes.byref(error))
    if rc != 0 or not runtime.value:
        os._exit(20)
    free_error(lib, error)
    rc = lib.tny_session_create(
        runtime, ctypes.byref(session), ctypes.byref(error))
    if rc != 0 or not session.value:
        os._exit(21)
    free_error(lib, error)
    return runtime, session, error, keep


def drain(lib, session, error):
    kinds = []
    errors = []
    for _ in range(128):
        event = ctypes.c_void_p()
        status = lib.tny_session_next_event(
            session, 5000, ctypes.byref(event), ctypes.byref(error))
        if status == DRAINED:
            break
        if status == TIMEOUT:
            continue
        if status != EVENT or not event.value:
            os._exit(30)
        kind = lib.tny_event_get_kind(event)
        kinds.append(kind)
        if kind == ERROR_KIND:
            errors.append(lib.tny_event_error_code(event))
        lib.tny_event_free(event)
    else:
        os._exit(31)
    if kinds.count(TERMINAL_KIND) != 1:
        os._exit(32)
    if OOM in errors:
        pos = errors.index(OOM)
        del pos
        if kinds[-2:] != [ERROR_KIND, TERMINAL_KIND]:
            os._exit(33)


def child_case(libpath, scope, base_url):
    lib = load_lib(libpath)
    with tempfile.TemporaryDirectory() as root:
        if scope == "runtime_create":
            workspace = os.path.join(root, "workspace")
            state = os.path.join(root, "state")
            os.makedirs(workspace)
            opts, keep = runtime_options(
                lib, base_url, workspace, state, api_key="fault-test-key")
            runtime = ctypes.c_void_p()
            error = ctypes.c_void_p()
            rc = lib.tny_runtime_create(
                ctypes.byref(opts), ctypes.byref(runtime), ctypes.byref(error))
            if rc not in (0, OOM):
                os._exit(10)
            if rc == OOM and runtime.value:
                os._exit(11)
            free_error(lib, error)
            if runtime.value:
                lib.tny_runtime_free(runtime)
            del keep
            return

        if scope == "session_create":
            workspace = os.path.join(root, "workspace")
            state = os.path.join(root, "state")
            os.makedirs(workspace)
            opts, keep = runtime_options(
                lib, base_url, workspace, state, api_key="fault-test-key")
            runtime = ctypes.c_void_p()
            session = ctypes.c_void_p()
            error = ctypes.c_void_p()
            if lib.tny_runtime_create(
                    ctypes.byref(opts), ctypes.byref(runtime),
                    ctypes.byref(error)) != 0:
                os._exit(15)
            rc = lib.tny_session_create(
                runtime, ctypes.byref(session), ctypes.byref(error))
            if rc not in (0, OOM):
                os._exit(16)
            if rc == OOM and session.value:
                os._exit(17)
            free_error(lib, error)
            if session.value:
                lib.tny_session_free(session)
            lib.tny_runtime_free(runtime)
            return

        runtime, session, error, keep = create_pair(
            lib, libpath, base_url, root)
        if scope == "session_send":
            raw, prompt = as_bytes("allocation fault turn")
            keep.append(raw)
            rc = lib.tny_session_send(session, prompt, ctypes.byref(error))
            if rc not in (0, OOM):
                os._exit(12)
            free_error(lib, error)
            if rc == 0:
                drain(lib, session, error)
        elif scope == "next_event":
            raw, prompt = as_bytes("allocation fault event")
            keep.append(raw)
            if lib.tny_session_send(
                    session, prompt, ctypes.byref(error)) != 0:
                os._exit(13)
            drain(lib, session, error)
        elif scope == "respond_permission":
            raw, prompt = as_bytes("request a sensitive operation")
            keep.append(raw)
            if lib.tny_session_send(
                    session, prompt, ctypes.byref(error)) != 0:
                os._exit(18)
            permission = None
            for _ in range(128):
                event = ctypes.c_void_p()
                status = lib.tny_session_next_event(
                    session, 5000, ctypes.byref(event), ctypes.byref(error))
                if status != EVENT:
                    os._exit(19)
                if lib.tny_event_get_kind(event) == 4:
                    value = lib.tny_event_permission_id(event)
                    permission = ctypes.string_at(value.ptr, value.len)
                    lib.tny_event_free(event)
                    break
                lib.tny_event_free(event)
            if not permission:
                os._exit(22)
            raw_id, permission_id = as_bytes(permission.decode())
            keep.append(raw_id)
            rc = lib.tny_session_respond_permission(
                session, permission_id, 2, ctypes.byref(error))
            if rc not in (0, OOM):
                os._exit(23)
            free_error(lib, error)
            if rc == OOM:
                os.environ["TNY_TEST_ALLOC_SCOPE"] = "disabled"
                if lib.tny_session_respond_permission(
                        session, permission_id, 2, ctypes.byref(error)) != 0:
                    os._exit(24)
            drain(lib, session, error)
        elif scope == "teardown":
            raw, prompt = as_bytes("slow active turn")
            keep.append(raw)
            if lib.tny_session_send(
                    session, prompt, ctypes.byref(error)) != 0:
                os._exit(25)
            started = time.monotonic()
            lib.tny_runtime_free(runtime)
            if time.monotonic() - started > 1.0:
                os._exit(26)
            return
        else:
            os._exit(14)
        lib.tny_session_free(session)
        lib.tny_runtime_free(runtime)


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def run_trial(script, libpath, scope, index, base_url):
    env = dict(os.environ)
    env["TNY_TEST_ALLOC_SCOPE"] = scope
    env["TNY_TEST_ALLOC_FAIL_AT"] = str(index)
    run = subprocess.run(
        [sys.executable, script, "--child", libpath, scope, base_url],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
    if run.returncode != 0 or run.stdout or run.stderr:
        raise AssertionError(
            f"{scope} allocation {index}: rc={run.returncode} "
            f"stdout={run.stdout!r} stderr={run.stderr!r}")


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--child":
        child_case(sys.argv[2], sys.argv[3], sys.argv[4])
        return
    if len(sys.argv) != 2 or "libtny" not in os.path.basename(sys.argv[1]):
        # The ordinary integration runner passes the tny executable to every
        # test_*.py.  Fault injection is intentionally an explicit build lane.
        print("test_libtny_faults: skip (run make test-libtny-fault)")
        return
    libpath = os.path.abspath(sys.argv[1])
    script = os.path.abspath(__file__)
    port = free_port()
    mock = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "mock_openai.py"), str(port)],
        env=dict(os.environ, MOCK_EXPECT_WIRE="responses"),
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        if "ready" not in mock.stdout.readline().decode():
            raise AssertionError("mock did not start")
        base_url = f"http://127.0.0.1:{port}/v1"
        limits = {
            "runtime_create": 48,
            "session_create": 48,
            "session_send": 96,
            "next_event": 48,
        }
        for scope, limit in limits.items():
            for index in range(1, limit + 1):
                run_trial(script, libpath, scope, index, base_url)
    finally:
        mock.terminate()
        mock.wait(timeout=5)

    port = free_port()
    sensitive = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "mock_openai.py"), str(port)],
        env=dict(os.environ, MOCK_EXPECT_WIRE="responses", MOCK_SENSITIVE="1"),
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        if "ready" not in sensitive.stdout.readline().decode():
            raise AssertionError("sensitive mock did not start")
        base_url = f"http://127.0.0.1:{port}/v1"
        for index in range(1, 49):
            run_trial(script, libpath, "respond_permission", index, base_url)
    finally:
        sensitive.terminate()
        sensitive.wait(timeout=5)

    port = free_port()
    slow = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "mock_openai.py"), str(port)],
        env=dict(os.environ, MOCK_EXPECT_WIRE="responses", MOCK_SLOW_MS="5000"),
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        if "ready" not in slow.stdout.readline().decode():
            raise AssertionError("slow mock did not start")
        run_trial(script, libpath, "teardown", 1,
                  f"http://127.0.0.1:{port}/v1")
    finally:
        slow.terminate()
        slow.wait(timeout=5)
    print("test_libtny_faults: allocation scopes and host stdio passed")


if __name__ == "__main__":
    main()
