#!/usr/bin/env python3
"""Build a clean C consumer and run a full libtny turn against the mock."""
import os
import ctypes
import socket
import re
import subprocess
import sys
import tempfile
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MOCK = os.path.join(ROOT, "tests", "integration", "mock_openai.py")


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


def as_bytes(value):
    raw = value.encode()
    return raw, TnyBytes(raw, len(raw))


def run_ctypes(libpath, base_url, workspace, state, repeat=False,
               api_key="test-key-not-real", expect_send_error=None):
    lib = ctypes.CDLL(libpath)
    lib.tny_runtime_options_init.argtypes = [ctypes.POINTER(RuntimeOptions)]
    lib.tny_runtime_create.argtypes = [ctypes.POINTER(RuntimeOptions),
                                       ctypes.POINTER(ctypes.c_void_p),
                                       ctypes.POINTER(ctypes.c_void_p)]
    lib.tny_runtime_create.restype = ctypes.c_int32
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

    opts = RuntimeOptions()
    lib.tny_runtime_options_init(ctypes.byref(opts))
    opts.persistence = 1
    keep = []
    fields = [("workspace", workspace), ("state_dir", state),
              ("base_url", base_url)]
    if api_key is not None:
        fields.append(("api_key", api_key))
    for field, value in fields:
        raw, view = as_bytes(value)
        keep.append(raw)
        setattr(opts, field, view)

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

    runtime = ctypes.c_void_p()
    error = ctypes.c_void_p()
    assert lib.tny_runtime_create(ctypes.byref(opts), ctypes.byref(runtime),
                                  ctypes.byref(error)) == 0
    second = ctypes.c_void_p()
    second_error = ctypes.c_void_p()
    assert lib.tny_runtime_create(ctypes.byref(opts), ctypes.byref(second),
                                  ctypes.byref(second_error)) == -3
    lib.tny_error_free(second_error)

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
    cross_thread = []
    def wrong_thread_cancel():
        thread_error = ctypes.c_void_p()
        cross_thread.append(lib.tny_session_cancel(session,
                                                   ctypes.byref(thread_error)))
        lib.tny_error_free(thread_error)
    thread = threading.Thread(target=wrong_thread_cancel)
    thread.start()
    thread.join()
    assert cross_thread == [-2]
    output = bytearray()
    saw_permission = False
    stop_reason = None
    error_codes = []
    prompts = ["list files in .", "again"] if repeat else ["list files in ."]
    for prompt_text in prompts:
        prompt_raw, prompt = as_bytes(prompt_text)
        keep.append(prompt_raw)
        send_status = lib.tny_session_send(session, prompt, ctypes.byref(error))
        if expect_send_error is not None:
            assert send_status == expect_send_error, send_status
            lib.tny_error_free(error)
            lib.tny_session_free(session)
            lib.tny_runtime_free(runtime)
            return b"", False, None, [send_status]
        assert send_status == 0
        while True:
            event = ctypes.c_void_p()
            status = lib.tny_session_next_event(session, 5000, ctypes.byref(event),
                                                ctypes.byref(error))
            if status == 2:
                continue
            if status == 3:
                break
            assert status == 1, status
            kind = lib.tny_event_get_kind(event)
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
    if repeat:
        cancel_raw, cancel_prompt = as_bytes("cancel this turn")
        keep.append(cancel_raw)
        assert lib.tny_session_send(session, cancel_prompt,
                                    ctypes.byref(error)) == 0
        assert lib.tny_session_cancel(session, ctypes.byref(error)) == 0
        cancelled = None
        while True:
            event = ctypes.c_void_p()
            status = lib.tny_session_next_event(session, 5000,
                                                ctypes.byref(event),
                                                ctypes.byref(error))
            if status == 3:
                break
            assert status in (1, 2), status
            if status == 1:
                if lib.tny_event_get_kind(event) == 7:
                    cancelled = lib.tny_event_stop_reason(event)
                lib.tny_event_free(event)
        assert cancelled == 1
    lib.tny_session_free(session)
    lib.tny_runtime_free(runtime)
    return bytes(output), saw_permission, stop_reason, error_codes


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def main():
    if sys.platform not in ("darwin", "linux"):
        print("test_libtny: skip (ABI 0 ships on Darwin/Linux only)")
        return
    subprocess.run(["make", "lib-shared"], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)
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

        cxx = os.environ.get("CXX", "c++")
        subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I" + os.path.join(prefix, "include"), "-x", "c++",
                        "-fsyntax-only", "-"],
                       input=b'#include <tny/tny.h>\nint main() { return TNY_ABI_MAJOR; }\n',
                       check=True)
    exe = os.path.join(ROOT, "build", "libtny-embed-test")
    rpath = libdir if sys.platform == "darwin" else "$ORIGIN/lib"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I" + os.path.join(ROOT, "include"),
        os.path.join(ROOT, "examples", "embed.c"),
        "-L" + libdir, "-ltny", "-Wl,-rpath," + rpath, "-o", exe,
    ], check=True)

    port = free_port()
    mock = subprocess.Popen([sys.executable, MOCK, str(port)],
                            env=dict(os.environ, MOCK_EXPECT_WIRE="responses",
                                     MOCK_REJECT_INSTRUCTIONS="HOME-SECRET"),
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        assert "ready" in mock.stdout.readline().decode()
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
    finally:
        mock.terminate()
        mock.wait(timeout=5)

    # Public ask mode parks a sensitive native call until the embedder answers.
    port = free_port()
    mock = subprocess.Popen([sys.executable, MOCK, str(port)],
                            env=dict(os.environ, MOCK_EXPECT_WIRE="responses",
                                     MOCK_SENSITIVE="1"),
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        assert "ready" in mock.stdout.readline().decode()
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
        mock.terminate()
        mock.wait(timeout=5)

    # Post-start authentication failures are typed ERROR events followed by
    # exactly one error terminal, not an unclassified string or sync failure.
    port = free_port()
    mock = subprocess.Popen([sys.executable, MOCK, str(port)],
                            env=dict(os.environ, MOCK_HTTP_STATUS="401"),
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        assert "ready" in mock.stdout.readline().decode()
        with tempfile.TemporaryDirectory() as root:
            workspace = os.path.join(root, "workspace")
            state = os.path.join(root, "state")
            os.makedirs(workspace)
            output, permission, stop, errors = run_ctypes(
                libpath, f"http://127.0.0.1:{port}/v1", workspace, state)
            assert not output and not permission and stop == 4
            assert errors == [-6], errors
    finally:
        mock.terminate()
        mock.wait(timeout=5)
    print("test_libtny: C and Python ctypes consumers passed")


if __name__ == "__main__":
    main()
