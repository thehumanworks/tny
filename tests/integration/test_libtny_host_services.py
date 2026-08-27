#!/usr/bin/env python3
"""Compile C/C++ host consumers and exercise Python ctypes callbacks."""

from __future__ import annotations

import ctypes
import os
from pathlib import Path
import platform
import socket
import subprocess
import sys
import tempfile


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


DIAGNOSTIC = ctypes.CFUNCTYPE(
    ctypes.c_int32, ctypes.c_void_p, ctypes.c_uint32, TnyBytes, TnyBytes
)
MONOTONIC = ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.c_void_p,
                            ctypes.POINTER(ctypes.c_int64))
RANDOM = ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.c_void_p, ctypes.c_void_p,
                         ctypes.c_uint64)
STORAGE_LOAD = ctypes.CFUNCTYPE(
    ctypes.c_int32, ctypes.c_void_p, TnyBytes,
    ctypes.POINTER(ctypes.c_uint64), ctypes.c_void_p, ctypes.c_uint64,
    ctypes.POINTER(ctypes.c_uint64),
)
STORAGE_STORE = ctypes.CFUNCTYPE(
    ctypes.c_int32, ctypes.c_void_p, TnyBytes, ctypes.c_uint64,
    ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
)
OPEN_URL = ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.c_void_p, TnyBytes)
NOTIFY = ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.c_void_p)


class HostServicesV1(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("user_data", ctypes.c_void_p),
        ("diagnostic", DIAGNOSTIC),
        ("monotonic_ms", MONOTONIC),
        ("secure_random", RANDOM),
        ("storage_load", STORAGE_LOAD),
        ("storage_store", STORAGE_STORE),
        ("open_url", OPEN_URL),
        ("notify_scheduler", NOTIFY),
        ("reserved", ctypes.c_uint64 * 8),
    ]


class RuntimeOptionsV1(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("runtime", RuntimeOptionsV0),
        ("host_services", ctypes.POINTER(HostServicesV1)),
        ("reserved", ctypes.c_uint64 * 8),
    ]


def run(command: list[str]) -> None:
    subprocess.run(command, cwd=ROOT, check=True, timeout=180)


def as_bytes(value: str) -> tuple[bytes, TnyBytes]:
    raw = value.encode("utf-8")
    return raw, TnyBytes(raw, len(raw))


def free_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def start_mock() -> tuple[subprocess.Popen[bytes], str]:
    port = free_port()
    process = subprocess.Popen(
        [sys.executable, os.fspath(MOCK), str(port)],
        cwd=ROOT,
        env=dict(os.environ, MOCK_EXPECT_WIRE="responses", MOCK_CONNECTION_CLOSE="1",
                 MOCK_CHUNK_WIDTH="1048576"),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert process.stdout is not None
    if b"ready" not in process.stdout.readline():
        assert process.stderr is not None
        raise RuntimeError(process.stderr.read().decode("utf-8", "replace"))
    return process, f"http://127.0.0.1:{port}/v1"


def stop_mock(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is None:
        process.terminate()
    process.wait(timeout=5)
    if process.stdout is not None:
        process.stdout.close()
    if process.stderr is not None:
        process.stderr.close()


def compile_consumers(library: Path, workspace: Path, output: Path,
                      base_url: str) -> None:
    rpath = f"-Wl,-rpath,{library.parent}"
    c_exe = output / "host-services-c"
    cpp_exe = output / "host-services-cpp"
    run([
        os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-Iinclude", "tests/integration/libtny_host_services.c",
        os.fspath(library), "-pthread", rpath, "-o", os.fspath(c_exe),
    ])
    run([
        os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
        "-Werror", "-Iinclude",
        "tests/integration/libtny_host_services_cpp.cpp", os.fspath(library),
        "-pthread", rpath, "-o", os.fspath(cpp_exe),
    ])
    run([os.fspath(c_exe), os.fspath(workspace), base_url])
    run([os.fspath(cpp_exe), os.fspath(workspace)])


def python_callback_consumer(library: Path, workspace: Path) -> None:
    assert ctypes.sizeof(RuntimeOptionsV0) == 200
    lib = ctypes.CDLL(os.fspath(library))
    lib.tny_runtime_options_v1_init.argtypes = [ctypes.POINTER(RuntimeOptionsV1)]
    lib.tny_host_services_v1_init.argtypes = [ctypes.POINTER(HostServicesV1)]
    lib.tny_runtime_create_v1.argtypes = [
        ctypes.POINTER(RuntimeOptionsV1), ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_runtime_create_v1.restype = ctypes.c_int32
    lib.tny_runtime_host_monotonic_ms.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_int64),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_runtime_host_monotonic_ms.restype = ctypes.c_int32
    lib.tny_runtime_host_notify_scheduler.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_runtime_host_notify_scheduler.restype = ctypes.c_int32
    lib.tny_runtime_host_secure_random.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint64,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_runtime_host_secure_random.restype = ctypes.c_int32
    lib.tny_error_code.argtypes = [ctypes.c_void_p]
    lib.tny_error_code.restype = ctypes.c_int32
    lib.tny_error_free.argtypes = [ctypes.c_void_p]
    lib.tny_runtime_free.argtypes = [ctypes.c_void_p]

    # Every byte boundary through the callback table is safe: a pointer is
    # copied only when the declared prefix contains that entire field.
    prefix_workspace_raw, prefix_workspace = as_bytes(os.fspath(workspace))
    prefix_url_raw, prefix_url = as_bytes("http://127.0.0.1:1/v1")
    assert prefix_workspace_raw and prefix_url_raw
    for declared in range(HostServicesV1.user_data.offset +
                          ctypes.sizeof(ctypes.c_void_p),
                          ctypes.sizeof(HostServicesV1) + 1):
        raw = (ctypes.c_ubyte * ctypes.sizeof(HostServicesV1))()
        table = ctypes.cast(raw, ctypes.POINTER(HostServicesV1))
        table.contents.abi_version = 1
        table.contents.struct_size = declared
        for index in range(declared, len(raw)):
            raw[index] = 0xA5
        prefix_options = RuntimeOptionsV1()
        lib.tny_runtime_options_v1_init(ctypes.byref(prefix_options))
        prefix_options.runtime.workspace = prefix_workspace
        prefix_options.runtime.base_url = prefix_url
        prefix_options.host_services = table
        prefix_runtime = ctypes.c_void_p()
        prefix_error = ctypes.c_void_p()
        assert lib.tny_runtime_create_v1(
            ctypes.byref(prefix_options), ctypes.byref(prefix_runtime),
            ctypes.byref(prefix_error)
        ) == 0
        lib.tny_runtime_free(prefix_runtime)

    runtime = ctypes.c_void_p()
    calls: list[str] = []
    reentrant: list[int] = []

    @DIAGNOSTIC
    def diagnostic(_user: int, _level: int, component: TnyBytes,
                   message: TnyBytes) -> int:
        component_text = ctypes.string_at(component.ptr, component.len)
        message_text = ctypes.string_at(message.ptr, message.len)
        assert component_text == b"libtny"
        assert b"secret" not in message_text.lower()
        calls.append(f"diagnostic:{message_text.decode()}")
        return 0

    @MONOTONIC
    def clock(_user: int, out: ctypes.POINTER(ctypes.c_int64)) -> int:
        nested_error = ctypes.c_void_p()
        reentrant.append(lib.tny_runtime_host_notify_scheduler(
            runtime, ctypes.byref(nested_error)
        ))
        if nested_error.value:
            lib.tny_error_free(nested_error)
        out[0] = 7000 + calls.count("clock")
        calls.append("clock")
        return 0

    @RANDOM
    def failing_random(_user: int, buffer: int, size: int) -> int:
        ctypes.memset(buffer, 0xCC, size)
        calls.append("random")
        return -7

    services = HostServicesV1()
    lib.tny_host_services_v1_init(ctypes.byref(services))
    services.diagnostic = diagnostic
    services.monotonic_ms = clock
    services.secure_random = failing_random
    options = RuntimeOptionsV1()
    lib.tny_runtime_options_v1_init(ctypes.byref(options))
    workspace_raw, options.runtime.workspace = as_bytes(os.fspath(workspace))
    url_raw, options.runtime.base_url = as_bytes("http://127.0.0.1:1/v1")
    key_raw, options.runtime.api_key = as_bytes("python-fixture-not-real")
    assert workspace_raw and url_raw and key_raw
    options.host_services = ctypes.pointer(services)
    error = ctypes.c_void_p()
    assert lib.tny_runtime_create_v1(
        ctypes.byref(options), ctypes.byref(runtime), ctypes.byref(error)
    ) == 0

    now = ctypes.c_int64()
    assert lib.tny_runtime_host_monotonic_ms(
        runtime, ctypes.byref(now), ctypes.byref(error)
    ) == 0
    assert now.value >= 7000 and reentrant == [-2]
    random = (ctypes.c_ubyte * 8)(*([0xAA] * 8))
    assert lib.tny_runtime_host_secure_random(
        runtime, random, len(random), ctypes.byref(error)
    ) == -7
    assert list(random) == [0] * 8
    assert error.value and lib.tny_error_code(error) == -7
    lib.tny_error_free(error)
    callbacks_before_free = len(calls)
    lib.tny_runtime_free(runtime)
    assert len(calls) == callbacks_before_free + 1
    assert calls[0] == "diagnostic:runtime created"
    assert calls[-1] == "diagnostic:runtime destroying"


def main() -> None:
    run(["make", "lib-shared"])
    suffix = "libtny.0.dylib" if platform.system() == "Darwin" else "libtny.so.0"
    library = (ROOT / "build/lib" / suffix).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="tny-host-services-") as temporary:
        root = Path(temporary)
        workspace = root / "workspace"
        workspace.mkdir()
        process, base_url = start_mock()
        try:
            compile_consumers(library, workspace, root, base_url)
            python_callback_consumer(library, workspace)
        finally:
            stop_mock(process)
    print("libtny-host-services: Python ctypes callback passed")


if __name__ == "__main__":
    main()
