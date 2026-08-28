#!/usr/bin/env python3
"""Build and execute the strict native custom-tool callback fixture."""

from __future__ import annotations

import ctypes
import os
import platform
import shutil
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MOCK = ROOT / "tests/integration/mock_openai.py"


class TnyBytes(ctypes.Structure):
    _fields_ = [("ptr", ctypes.c_char_p), ("len", ctypes.c_uint64)]


class RuntimeOptionsV0(ctypes.Structure):
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


class ToolResult(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("data", TnyBytes),
        ("is_error", ctypes.c_uint32),
        ("reserved_scalar", ctypes.c_uint32),
        ("reserved", ctypes.c_uint64 * 4),
    ]


INVOKE = ctypes.CFUNCTYPE(
    ctypes.c_int32,
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_uint64,
    TnyBytes,
    ctypes.POINTER(ToolResult),
)


class ToolSpec(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("user_data", ctypes.c_void_p),
        ("name", TnyBytes),
        ("description", TnyBytes),
        ("input_schema_json", TnyBytes),
        ("sensitivity", ctypes.c_uint32),
        ("reserved_scalar", ctypes.c_uint32),
        ("max_argument_bytes", ctypes.c_uint64),
        ("max_result_bytes", ctypes.c_uint64),
        ("invoke", INVOKE),
        ("reserved", ctypes.c_uint64 * 8),
    ]


def byte_view(value: str) -> tuple[bytes, TnyBytes]:
    raw = value.encode()
    return raw, TnyBytes(raw, len(raw))


def python_consumer(library: Path, workspace: Path, url: str) -> None:
    lib = ctypes.CDLL(os.fspath(library))
    lib.tny_runtime_options_init.argtypes = [
        ctypes.POINTER(RuntimeOptionsV0),
        ctypes.c_uint64,
    ]
    lib.tny_tool_spec_v1_init.argtypes = [ctypes.POINTER(ToolSpec), ctypes.c_uint64]
    lib.tny_tool_result_v1_init.argtypes = [ctypes.POINTER(ToolResult), ctypes.c_uint64]
    lib.tny_runtime_create.argtypes = [
        ctypes.POINTER(RuntimeOptionsV0),
        ctypes.c_uint64,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_runtime_create.restype = ctypes.c_int32
    lib.tny_runtime_register_tool.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ToolSpec),
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_runtime_register_tool.restype = ctypes.c_int32
    lib.tny_session_create.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_session_send.argtypes = [
        ctypes.c_void_p,
        TnyBytes,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_session_next_event.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_event_get_kind.argtypes = [ctypes.c_void_p]
    lib.tny_event_get_kind.restype = ctypes.c_uint32
    lib.tny_event_stop_reason.argtypes = [ctypes.c_void_p]
    lib.tny_event_stop_reason.restype = ctypes.c_uint32
    lib.tny_event_free.argtypes = [ctypes.c_void_p]
    lib.tny_session_free.argtypes = [ctypes.c_void_p]
    lib.tny_tool_registration_unregister.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_runtime_free.argtypes = [ctypes.c_void_p]

    invocations = 0
    result_raw = b"python-result"

    @INVOKE
    def invoke(
        _user: int,
        _call: int,
        _generation: int,
        arguments: TnyBytes,
        result: ctypes.POINTER(ToolResult),
    ) -> int:
        nonlocal invocations
        invocations += 1
        assert b"hello" in ctypes.string_at(arguments.ptr, arguments.len)
        lib.tny_tool_result_v1_init(result, ctypes.sizeof(result.contents))
        result.contents.data = TnyBytes(result_raw, len(result_raw))
        return 0

    options = RuntimeOptionsV0()
    lib.tny_runtime_options_init(ctypes.byref(options), ctypes.sizeof(options))
    workspace_raw, options.workspace = byte_view(os.fspath(workspace))
    url_raw, options.base_url = byte_view(url)
    key_raw, options.api_key = byte_view("python-custom-not-real")
    options.permission_mode = 2
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
    spec = ToolSpec()
    lib.tny_tool_spec_v1_init(ctypes.byref(spec), ctypes.sizeof(spec))
    name_raw, spec.name = byte_view("host_echo")
    desc_raw, spec.description = byte_view("Python callback fixture")
    schema_raw, spec.input_schema_json = byte_view(
        '{"type":"object","properties":{"value":{"type":"string"}}}'
    )
    spec.invoke = invoke
    registration = ctypes.c_void_p()
    assert all((workspace_raw, url_raw, key_raw, name_raw, desc_raw, schema_raw))
    assert (
        lib.tny_runtime_register_tool(
            runtime, ctypes.byref(spec), ctypes.byref(registration), ctypes.byref(error)
        )
        == 0
    )
    session = ctypes.c_void_p()
    assert (
        lib.tny_session_create(runtime, ctypes.byref(session), ctypes.byref(error)) == 0
    )
    prompt_raw, prompt = byte_view("call Python host tool")
    assert (
        prompt_raw and lib.tny_session_send(session, prompt, ctypes.byref(error)) == 0
    )
    terminals = 0
    while True:
        event = ctypes.c_void_p()
        status = lib.tny_session_next_event(
            session, 5000, ctypes.byref(event), ctypes.byref(error)
        )
        if status == 3:
            break
        assert status == 1 and event.value
        if lib.tny_event_get_kind(event) == 7:
            terminals += 1
            assert lib.tny_event_stop_reason(event) == 0
        lib.tny_event_free(event)
    assert invocations == 1 and terminals == 1
    lib.tny_session_free(session)
    assert lib.tny_tool_registration_unregister(registration, ctypes.byref(error)) == 0
    lib.tny_runtime_free(runtime)


def free_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def main() -> None:
    prebuilt_host = os.environ.get("TNY_CUSTOM_TOOL_HOST")
    library: Path | None = None
    if not prebuilt_host:
        subprocess.run(["make", "lib-shared-active"], cwd=ROOT, check=True, timeout=180)
        suffix = "libtny.1.dylib" if platform.system() == "Darwin" else "libtny.so.1"
        library = (ROOT / "build/lib" / suffix).resolve(strict=True)
    port = free_port()
    mock = subprocess.Popen(
        [sys.executable, os.fspath(MOCK), str(port)],
        cwd=ROOT,
        env=dict(
            os.environ,
            MOCK_EXPECT_WIRE="responses",
            MOCK_CUSTOM_TOOL="host_echo",
            MOCK_CONNECTION_CLOSE="1",
            MOCK_CHUNK_WIDTH="1048576",
        ),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        assert mock.stdout is not None
        if b"ready" not in mock.stdout.readline():
            assert mock.stderr is not None
            raise RuntimeError(mock.stderr.read().decode("utf-8", "replace"))
        with tempfile.TemporaryDirectory(prefix="tny-custom-tools-") as temporary:
            root = Path(temporary)
            workspace = root / "workspace"
            workspace.mkdir()
            executable = root / "custom-tools"
            cpp_executable = root / "custom-tools-cpp"
            if prebuilt_host:
                executable = Path(prebuilt_host).resolve(strict=True)
            else:
                assert library is not None
                subprocess.run(
                    [
                        os.environ.get("CC", "cc"),
                        "-std=c11",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-Iinclude",
                        "tests/integration/libtny_custom_tools.c",
                        os.fspath(library),
                        "-pthread",
                        f"-Wl,-rpath,{library.parent}",
                        "-o",
                        os.fspath(executable),
                    ],
                    cwd=ROOT,
                    check=True,
                    timeout=180,
                )
                subprocess.run(
                    [
                        os.environ.get("CXX", "c++"),
                        "-std=c++17",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-Iinclude",
                        "tests/integration/libtny_custom_tools_cpp.cpp",
                        os.fspath(library),
                        "-pthread",
                        f"-Wl,-rpath,{library.parent}",
                        "-o",
                        os.fspath(cpp_executable),
                    ],
                    cwd=ROOT,
                    check=True,
                    timeout=180,
                )
            subprocess.run(
                [
                    os.fspath(executable),
                    os.fspath(workspace),
                    f"http://127.0.0.1:{port}/v1",
                ],
                cwd=ROOT,
                check=True,
                timeout=180,
            )
            if prebuilt_host:
                return
            assert library is not None
            subprocess.run(
                [
                    os.fspath(cpp_executable),
                    os.fspath(workspace),
                    f"http://127.0.0.1:{port}/v1",
                ],
                cwd=ROOT,
                check=True,
                timeout=180,
            )
            python_consumer(library, workspace, f"http://127.0.0.1:{port}/v1")
            if shutil.which("go"):
                subprocess.run(
                    [
                        "go",
                        "run",
                        "tests/integration/custom_tools_header_go.go",
                    ],
                    cwd=ROOT,
                    env=dict(os.environ, CGO_CFLAGS=f"-I{ROOT / 'include'}"),
                    check=True,
                    timeout=180,
                )
            if shutil.which("swiftc"):
                subprocess.run(
                    [
                        "swiftc",
                        "-typecheck",
                        "-I",
                        os.fspath(ROOT / "tests/integration/tny_clang_module"),
                        os.fspath(
                            ROOT / "tests/integration/custom_tools_header_swift.swift"
                        ),
                    ],
                    cwd=ROOT,
                    check=True,
                    timeout=180,
                )
    finally:
        if mock.poll() is None:
            mock.terminate()
        mock.wait(timeout=5)
        if mock.stdout is not None:
            mock.stdout.close()
        if mock.stderr is not None:
            mock.stderr.close()


if __name__ == "__main__":
    main()
