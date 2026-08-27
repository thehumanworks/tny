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
PERMISSION_KIND = 4
TERMINAL_KIND = 7
ERROR_KIND = 8


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
    lib.tny_tools_test_walk.argtypes = [ctypes.c_char_p]
    lib.tny_tools_test_walk.restype = ctypes.c_int
    lib.tny_session_steer.argtypes = [ctypes.c_void_p, byte_type,
                                      ctypes.POINTER(ctypes.c_void_p)]
    lib.tny_session_steer.restype = ctypes.c_int32
    lib.tny_session_id.argtypes = [ctypes.c_void_p]
    lib.tny_session_id.restype = byte_type


def observe(lib, stats):
    stats[0] = max(stats[0], int(lib.tny_alloc_test_scope_count()))
    injected = bool(lib.tny_alloc_test_scope_injected())
    stats[1] = stats[1] or injected
    return injected


def write_report(path, stats):
    with open(path, "w", encoding="ascii") as report:
        report.write(f"{stats[0]} {int(stats[1])}\n")


def create_pair(lib, base_url, root, persistence=0):
    workspace = os.path.join(root, "workspace")
    state = os.path.join(root, "state")
    os.makedirs(workspace, exist_ok=True)
    os.makedirs(state, exist_ok=True)
    opts, keep = runtime_options(
        lib, base_url, workspace, state, api_key="fault-test-key",
        persistence=persistence)
    runtime = ctypes.c_void_p()
    session = ctypes.c_void_p()
    error = ctypes.c_void_p()
    if lib.tny_runtime_create(
            ctypes.byref(opts), ctypes.byref(runtime),
            ctypes.byref(error)) != 0 or not runtime.value:
        die(20)
    free_error(lib, error)
    if lib.tny_session_create(
            runtime, ctypes.byref(session),
            ctypes.byref(error)) != 0 or not session.value:
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
        session, 5000, ctypes.byref(event), ctypes.byref(error))
    if stats is not None:
        observe(lib, stats)
    return status, event


def drain(lib, session, error, stats=None, expect_oom=None,
          require_oom_if_injected=False):
    kinds = []
    errors = []
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
    runtime, session, error, keep = create_pair(
        lib, base_url, root, persistence=1)
    raw, prompt = as_bytes("persist before session_open fault sweep")
    keep.append(raw)
    if lib.tny_session_send(
            session, prompt, ctypes.byref(error)) != 0:
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
        if scenario == "tools_fs_walk":
            seed_walk_workspace(root)
            workspace = os.path.join(root, "workspace").encode()
            rc = lib.tny_tools_test_walk(workspace)
            injected = observe(lib, stats)
            if (injected and rc != -1) or (not injected and rc != 2):
                die(53)

        elif scenario == "runtime_create":
            workspace = os.path.join(root, "workspace")
            state = os.path.join(root, "state")
            os.makedirs(workspace)
            opts, keep = runtime_options(
                lib, base_url, workspace, state, api_key="fault-test-key")
            runtime = ctypes.c_void_p()
            error = ctypes.c_void_p()
            rc = lib.tny_runtime_create(
                ctypes.byref(opts), ctypes.byref(runtime),
                ctypes.byref(error))
            injected = observe(lib, stats)
            if ((injected and (rc != OOM or runtime.value)) or
                    (not injected and (rc != 0 or not runtime.value))):
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
                lib, base_url, workspace, state, api_key="fault-test-key")
            runtime = ctypes.c_void_p()
            session = ctypes.c_void_p()
            error = ctypes.c_void_p()
            if lib.tny_runtime_create(
                    ctypes.byref(opts), ctypes.byref(runtime),
                    ctypes.byref(error)) != 0:
                die(11)
            rc = lib.tny_session_create(
                runtime, ctypes.byref(session), ctypes.byref(error))
            injected = observe(lib, stats)
            if ((injected and (rc != OOM or session.value)) or
                    (not injected and (rc != 0 or not session.value))):
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
                lib, base_url, workspace, state, api_key="fault-test-key")
            keep.extend(more_keep)
            runtime = ctypes.c_void_p()
            session = ctypes.c_void_p()
            error = ctypes.c_void_p()
            if lib.tny_runtime_create(
                    ctypes.byref(opts), ctypes.byref(runtime),
                    ctypes.byref(error)) != 0:
                die(13)
            raw_id, session_view = as_bytes(session_id)
            keep.append(raw_id)
            rc = lib.tny_session_open(
                runtime, session_view, ctypes.byref(session),
                ctypes.byref(error))
            injected = observe(lib, stats)
            if ((injected and (rc != OOM or session.value)) or
                    (not injected and (rc != 0 or not session.value))):
                die(14)
            free_error(lib, error)
            if session.value:
                lib.tny_session_free(session)
            lib.tny_runtime_free(runtime)

        elif scenario in ("session_send", "session_send_rearm", "next_event",
                          "session_steer", "respond_permission"):
            runtime, session, error, keep = create_pair(
                lib, base_url, root)
            if scenario == "session_send_rearm":
                target_scope = os.environ.get("TNY_TEST_ALLOC_SCOPE", "")
                target_index = os.environ.get("TNY_TEST_ALLOC_FAIL_AT")
                os.environ["TNY_TEST_ALLOC_SCOPE"] = "next_event"
                os.environ["TNY_TEST_ALLOC_FAIL_AT"] = "1"
                raw, prompt = as_bytes("consume oom reserves before rearm")
                keep.append(raw)
                if lib.tny_session_send(
                        session, prompt, ctypes.byref(error)) != 0:
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
                rc = lib.tny_session_send(
                    session, prompt, ctypes.byref(error))
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
                if lib.tny_session_send(
                        session, prompt, ctypes.byref(error)) != 0:
                    die(16)
                drain(lib, session, error, stats=stats,
                      require_oom_if_injected=True)

            elif scenario == "session_steer":
                raw, prompt = as_bytes("slow active turn")
                keep.append(raw)
                if lib.tny_session_send(
                        session, prompt, ctypes.byref(error)) != 0:
                    die(17)
                raw_steer, steer = as_bytes("follow-up steering")
                keep.append(raw_steer)
                rc = lib.tny_session_steer(
                    session, steer, ctypes.byref(error))
                injected = observe(lib, stats)
                if rc not in (0, OOM):
                    die(18)
                free_error(lib, error)
                if rc == OOM:
                    os.environ["TNY_TEST_ALLOC_SCOPE"] = "disabled"
                    if lib.tny_session_cancel(
                            session, ctypes.byref(error)) != 0:
                        die(19)
                drain(lib, session, error,
                      expect_oom=injected if rc == 0 else False)

            else:
                raw, prompt = as_bytes("request a sensitive operation")
                keep.append(raw)
                if lib.tny_session_send(
                        session, prompt, ctypes.byref(error)) != 0:
                    die(22)
                permission = wait_permission(lib, session, error)
                raw_id, permission_id = as_bytes(permission)
                keep.append(raw_id)
                rc = lib.tny_session_respond_permission(
                    session, permission_id, 2, ctypes.byref(error))
                injected = observe(lib, stats)
                if rc not in (0, OOM):
                    die(23)
                free_error(lib, error)
                if rc == OOM:
                    os.environ["TNY_TEST_ALLOC_SCOPE"] = "disabled"
                    if lib.tny_session_respond_permission(
                            session, permission_id, 2,
                            ctypes.byref(error)) != 0:
                        die(24)
                drain(lib, session, error,
                      expect_oom=injected if rc == 0 else False)
            lib.tny_session_free(session)
            lib.tny_runtime_free(runtime)

        elif scenario in ("session_free_active", "runtime_free_active",
                          "session_free_cancelled", "runtime_free_cancelled"):
            target_session = scenario.startswith("session_")
            for cycle in range(3):
                cycle_root = os.path.join(root, str(cycle))
                os.makedirs(cycle_root)
                runtime, session, error, keep = create_pair(
                    lib, base_url, cycle_root)
                raw, prompt = as_bytes("slow active teardown")
                keep.append(raw)
                if lib.tny_session_send(
                        session, prompt, ctypes.byref(error)) != 0:
                    die(25)
                if scenario.endswith("_cancelled"):
                    if lib.tny_session_cancel(
                            session, ctypes.byref(error)) != 0:
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

        elif scenario in ("session_free_permission",
                          "runtime_free_permission"):
            target_session = scenario.startswith("session_")
            runtime, session, error, keep = create_pair(
                lib, base_url, root)
            raw, prompt = as_bytes("request a sensitive operation")
            keep.append(raw)
            if lib.tny_session_send(
                    session, prompt, ctypes.byref(error)) != 0:
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
            runtime, session, error, keep = create_pair(
                lib, base_url, root)
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
            if lib.tny_session_send(
                    session, prompt, ctypes.byref(error)) != 0:
                die(50)
            drain(lib, session, error, expect_oom=True)
        os.environ["TNY_TEST_ALLOC_SCOPE"] = "disabled"
        raw, prompt = as_bytes("successful retry after two oom turns")
        keep.append(raw)
        if lib.tny_session_send(
                session, prompt, ctypes.byref(error)) != 0:
            die(52)
        drain(lib, session, error, expect_oom=False)
        lib.tny_session_free(session)
        lib.tny_runtime_free(runtime)


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
            env["LD_PRELOAD"] = asan_runtime
    executable = env.get("TNY_TEST_PYTHON_EXEC", sys.executable)
    run = subprocess.run(
        [executable, script, *args], env=env,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    if run.returncode != 0 or run.stdout or run.stderr:
        raise AssertionError(
            f"child {args}: rc={run.returncode} "
            f"stdout={run.stdout!r} stderr={run.stderr!r}")


def run_measured(script, libpath, scenario, scope, base_url, fail_at=None):
    # Socket reads may coalesce adjacent flushed HTTP chunks differently in a
    # fresh process. Retry only a not-reached index; an injected run still has
    # exactly one chance and remains release-blocking on any bad outcome.
    attempts = 4 if fail_at is not None else 1
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
            run_child(script,
                      ["--child", libpath, scenario, base_url, report], env)
            with open(report, encoding="ascii") as value:
                count, injected = (int(item) for item in value.read().split())
            maximum = max(maximum, count)
            if fail_at is None or injected:
                return count
    raise AssertionError(
        f"{scenario} allocation {fail_at} was not reached in {attempts} "
        f"fresh processes (maximum observed {maximum})")


def sweep(script, libpath, scenario, scope, base_url):
    count = run_measured(script, libpath, scenario, scope, base_url)
    for index in range(1, count + 1):
        run_measured(script, libpath, scenario, scope, base_url, index)
    return count


def start_mock(**settings):
    port = free_port()
    env = dict(os.environ, MOCK_EXPECT_WIRE="responses", **settings)
    mock = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "mock_openai.py"), str(port)],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    if "ready" not in mock.stdout.readline().decode():
        mock.terminate()
        mock.wait(timeout=5)
        raise AssertionError("mock did not start")
    return mock, f"http://127.0.0.1:{port}/v1"


def main():
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

    results["tools_fs_walk"] = sweep(
        script, libpath, "tools_fs_walk", "tools_fs_walk", "unused")

    mock, base_url = start_mock(MOCK_SLOW_MS="150")
    try:
        for scenario, scope in (
                ("runtime_create", "runtime_create"),
                ("session_create", "session_create"),
                ("session_open", "session_open"),
                ("session_send", "session_send"),
                ("session_send_rearm", "session_send"),
                ("next_event", "next_event"),
                ("session_steer", "session_steer")):
            results[scenario] = sweep(
                script, libpath, scenario, scope, base_url)
        env = dict(os.environ, TNY_TEST_ALLOC_SCOPE="next_event",
                   TNY_TEST_ALLOC_FAIL_AT="1")
        run_child(script, ["--repeat-oom", libpath, base_url], env)
    finally:
        mock.terminate()
        mock.wait(timeout=5)

    sensitive, sensitive_url = start_mock(MOCK_SENSITIVE="1")
    try:
        results["respond_permission"] = sweep(
            script, libpath, "respond_permission",
            "respond_permission", sensitive_url)
        for scenario, scope in (
                ("session_free_permission", "session_free"),
                ("runtime_free_permission", "runtime_free")):
            results[scenario] = sweep(
                script, libpath, scenario, scope, sensitive_url)
    finally:
        sensitive.terminate()
        sensitive.wait(timeout=5)

    slow, slow_url = start_mock(MOCK_SLOW_MS="5000")
    try:
        for scenario, scope in (
                ("session_free_active", "session_free"),
                ("runtime_free_active", "runtime_free"),
                ("session_free_cancelled", "session_free"),
                ("runtime_free_cancelled", "runtime_free")):
            results[scenario] = sweep(
                script, libpath, scenario, scope, slow_url)
    finally:
        slow.terminate()
        slow.wait(timeout=5)

    unreachable_url = f"http://127.0.0.1:{free_port()}/v1"
    for scenario, scope in (
            ("session_free_failed", "session_free"),
            ("runtime_free_failed", "runtime_free")):
        results[scenario] = sweep(
            script, libpath, scenario, scope, unreachable_url)

    counts = ", ".join(f"{name}={count}" for name, count in results.items())
    print("test_libtny_faults: exhaustive allocation sweeps passed; " + counts)


if __name__ == "__main__":
    main()
