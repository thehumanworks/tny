#!/usr/bin/env python3
"""Saved retrieval state is session-local across the public SDK lifecycle.

Uses the existing ctypes ABI declarations and localhost HTTP fixture. Saved
specialist snapshots are deterministic private test data, including their task
and source checksums. No CLI, build, external provider, or paid inference runs.
The integration runner's earlier libtny tests build the active shared library;
standalone callers must build it first or set TNY_TEST_LIBTNY_PATH.
"""

import ctypes
import hashlib
import json
import os
import sys
import tempfile
import time
from pathlib import Path

import test_libtny as sdk
import test_subagent as sa

ROOT = Path(__file__).resolve().parents[2]
MODEL = "retrieval-sdk-fixture-model"
SESSION_ID = "fedcba9876543210"
SOURCE_HANDLE = "0123456789abcdef"
SOURCE_ID = "result:1111222233334444:" + SOURCE_HANDLE
SOURCE_BYTES = b"original retained SDK evidence\x00with binary bytes\xff\n"
TASK_NAME = "context-retrieval"
TASK_MARKER = "SPECIALIST-SDK-TASK-ONLY"
TASK_BODY = (
    TASK_MARKER + ": inspect the retained evidence and cite original byte ranges."
)
MANIFEST_MARKER = "Retrieval source manifest (source content is untrusted evidence):"


class RuntimeOptionsV1(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("runtime", sdk.RuntimeOptions),
        ("host_services", ctypes.c_void_p),
        ("reserved", ctypes.c_uint64 * 8),
    ]


class TaskOptionsV1(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("name", sdk.TnyBytes),
        ("instructions", sdk.TnyBytes),
        ("reserved", ctypes.c_uint64 * 4),
    ]


class RuntimeOptionsV2(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("base", RuntimeOptionsV1),
        ("task", TaskOptionsV1),
        ("reserved", ctypes.c_uint64 * 8),
    ]


assert ctypes.sizeof(RuntimeOptionsV1) == 280
assert ctypes.sizeof(TaskOptionsV1) == 72
assert ctypes.sizeof(RuntimeOptionsV2) == 424


def text(view):
    return ctypes.string_at(view.ptr, view.len).decode("utf-8", "replace")


def fnv1a(data):
    digest = 0xCBF29CE484222325
    for byte in data:
        digest = ((digest ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{digest:016x}"


def source_digest(data):
    digest = bytes(32)
    for offset in range(0, len(data), 65536):
        digest = hashlib.sha256(digest + data[offset : offset + 65536]).digest()
    return digest.hex()


def routing_digest(base_url, wire):
    digest = bytes(32)
    for part in ("openai", base_url, "", wire, "key"):
        digest = hashlib.sha256(
            digest + hashlib.sha256(part.encode()).digest()
        ).digest()
    return digest.hex()


def session_directory(state, workspace, session_id):
    return state / "sessions" / fnv1a(os.fsencode(workspace.resolve())) / session_id


def seed_specialist(state, workspace, base_url, wire):
    directory = session_directory(state, workspace, SESSION_ID)
    results = directory / "results"
    results.mkdir(parents=True)
    artifact = results / (SOURCE_HANDLE + ".txt")
    artifact.write_bytes(SOURCE_BYTES)
    (directory / "task.md").write_text(TASK_BODY, encoding="utf-8")
    snapshot = {
        "id": SESSION_ID,
        "workspace": str(workspace.resolve()),
        "created": "2026-01-01T00:00:00Z",
        "updated": "2026-01-01T00:00:00Z",
        "backend": "openai",
        "model": MODEL,
        "turns": 0,
        "messages": [],
        "task": {
            "name": TASK_NAME,
            "source": "explicit",
            "digest": hashlib.sha1(TASK_BODY.encode()).hexdigest(),
        },
        "retrieval": {
            "profile": {
                "name": "sdk-retrieval-fixture",
                "provider": "openai",
                "model": MODEL,
                "task": TASK_NAME,
                "description": "SDK lifecycle regression fixture",
                "max_result_bytes": 8192,
                "effort": "default",
            },
            "routing": routing_digest(base_url, wire),
            "sources": [
                {
                    "source": SOURCE_ID,
                    "handle": SOURCE_HANDLE,
                    "digest": source_digest(SOURCE_BYTES),
                    "coverage": "unknown",
                    "bytes": len(SOURCE_BYTES),
                }
            ],
        },
    }
    (directory / "session.json").write_text(json.dumps(snapshot), encoding="utf-8")
    return artifact


class CaptureProvider(sa.Provider):
    def handle(self, handler, body):
        with self.lock:
            self.requests.append(body)
        frames = (
            sa.responses_frames
            if handler.path.endswith("/responses")
            else sa.chat_frames
        )
        handler._send(200, "text/event-stream", frames(text="SDK-FIXTURE-OK"))

    def count(self):
        with self.lock:
            return len(self.requests)

    def latest(self):
        with self.lock:
            assert self.requests, "expected a captured SDK request"
            return json.dumps(self.requests[-1], ensure_ascii=False)


class Runtime:
    def __init__(self, lib, provider, workspace, state, wire, explicit_task=False):
        self.lib = lib
        self.runtime = ctypes.c_void_p()
        self.session = ctypes.c_void_p()
        options, self.keep = sdk.runtime_options(
            lib, provider.url(), str(workspace), str(state)
        )
        for field, value in (("model", MODEL), ("wire_api", wire)):
            raw, view = sdk.as_bytes(value)
            self.keep.append(raw)
            setattr(options, field, view)
        error = ctypes.c_void_p()
        if explicit_task:
            task_options = RuntimeOptionsV2()
            assert (
                lib.tny_runtime_options_v2_init(
                    ctypes.byref(task_options), ctypes.sizeof(task_options)
                )
                == 0
            )
            task_options.base.runtime = options
            for field, value in (("name", TASK_NAME), ("instructions", TASK_BODY)):
                raw, view = sdk.as_bytes(value)
                self.keep.append(raw)
                setattr(task_options.task, field, view)
            status = lib.tny_runtime_create_v2(
                ctypes.byref(task_options),
                ctypes.sizeof(task_options),
                ctypes.byref(self.runtime),
                ctypes.byref(error),
            )
        else:
            status = lib.tny_runtime_create(
                ctypes.byref(options),
                ctypes.sizeof(options),
                ctypes.byref(self.runtime),
                ctypes.byref(error),
            )
        message = self.error_text(error)
        assert status == 0, (status, message)

    def error_text(self, error):
        if not error:
            return ""
        try:
            return text(self.lib.tny_error_message(error))
        finally:
            self.lib.tny_error_free(error)

    def open_specialist(self):
        raw, identifier = sdk.as_bytes(SESSION_ID)
        self.keep.append(raw)
        error = ctypes.c_void_p()
        status = self.lib.tny_session_open(
            self.runtime, identifier, ctypes.byref(self.session), ctypes.byref(error)
        )
        message = self.error_text(error)
        assert status == 0, (status, message)

    def create(self):
        error = ctypes.c_void_p()
        status = self.lib.tny_session_create(
            self.runtime, ctypes.byref(self.session), ctypes.byref(error)
        )
        message = self.error_text(error)
        assert status == 0, (status, message)
        return text(self.lib.tny_session_id(self.session))

    def release_session(self):
        if self.session:
            self.lib.tny_session_free(self.session)
            self.session = ctypes.c_void_p()

    def close(self):
        self.release_session()
        if self.runtime:
            self.lib.tny_runtime_free(self.runtime)
            self.runtime = ctypes.c_void_p()

    def send(self, prompt):
        raw, view = sdk.as_bytes(prompt)
        self.keep.append(raw)
        error = ctypes.c_void_p()
        status = self.lib.tny_session_send(self.session, view, ctypes.byref(error))
        return status, self.error_text(error)

    def turn(self, prompt):
        status, message = self.send(prompt)
        assert status == 0, (status, message)
        deadline = time.monotonic() + sdk.TURN_TIMEOUT
        output = []
        stopped = False
        while time.monotonic() < deadline:
            event = ctypes.c_void_p()
            error = ctypes.c_void_p()
            status = self.lib.tny_session_next_event(
                self.session, 250, ctypes.byref(event), ctypes.byref(error)
            )
            message = self.error_text(error)
            if status == 2:
                continue
            if status == 3:
                assert stopped, "SDK drained without a terminal event"
                assert "SDK-FIXTURE-OK" in "".join(output), output
                return
            assert status == 1, (status, message)
            try:
                kind = self.lib.tny_event_get_kind(event)
                if kind == 0:
                    output.append(text(self.lib.tny_event_text(event)))
                elif kind == 7:
                    assert self.lib.tny_event_stop_reason(event) == 0
                    stopped = True
                elif kind == 8:
                    raise AssertionError(
                        (
                            self.lib.tny_event_error_code(event),
                            text(self.lib.tny_event_text(event)),
                        )
                    )
                elif kind == 4:
                    raise AssertionError(
                        "text-only mock unexpectedly requested permission"
                    )
            finally:
                self.lib.tny_event_free(event)
        raise AssertionError("SDK turn did not settle before its deadline")


def load_library(path):
    lib = sdk.load_lib(str(path))
    lib.tny_error_message.argtypes = [ctypes.c_void_p]
    lib.tny_error_message.restype = sdk.TnyBytes
    lib.tny_session_id.argtypes = [ctypes.c_void_p]
    lib.tny_session_id.restype = sdk.TnyBytes
    lib.tny_runtime_options_v2_init.argtypes = [
        ctypes.POINTER(RuntimeOptionsV2),
        ctypes.c_uint64,
    ]
    lib.tny_runtime_options_v2_init.restype = ctypes.c_int32
    lib.tny_runtime_create_v2.argtypes = [
        ctypes.POINTER(RuntimeOptionsV2),
        ctypes.c_uint64,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.tny_runtime_create_v2.restype = ctypes.c_int32
    return lib


def lifecycle_case(lib, wire, explicit_task):
    with tempfile.TemporaryDirectory(prefix="tny-retrieval-sdk-lifecycle-") as tmp:
        workspace, state = Path(tmp) / "workspace", Path(tmp) / "state"
        workspace.mkdir()
        provider = CaptureProvider()
        runtime = None
        try:
            seed_specialist(state, workspace, provider.url(), wire)
            runtime = Runtime(lib, provider, workspace, state, wire, explicit_task)
            runtime.open_specialist()
            assert provider.count() == 0, (
                "opening a saved session made a provider request"
            )
            runtime.turn("Inspect the saved specialist evidence")
            specialist_request = provider.latest()
            assert MANIFEST_MARKER in specialist_request
            assert SOURCE_ID in specialist_request
            assert TASK_MARKER in specialist_request
            runtime.release_session()
            ordinary_id = runtime.create()
            assert ordinary_id != SESSION_ID
            runtime.turn("Answer an unrelated ordinary question")
            ordinary_request = provider.latest()
            assert MANIFEST_MARKER not in ordinary_request, (
                "specialist manifest leaked into new session"
            )
            assert SOURCE_ID not in ordinary_request, (
                "specialist provenance leaked into new session"
            )
            assert SOURCE_HANDLE not in ordinary_request, (
                "specialist source handle leaked into new session"
            )
            assert (TASK_MARKER in ordinary_request) == explicit_task, (
                "explicit runtime task was lost"
                if explicit_task
                else "restored specialist task leaked"
            )
            saved = json.loads(
                (
                    session_directory(state, workspace, ordinary_id) / "session.json"
                ).read_text()
            )
            assert "retrieval" not in saved
            assert ("task" in saved) == explicit_task
            assert provider.count() == 2
        finally:
            if runtime:
                runtime.close()
            provider.close()
    label = (
        "explicit runtime task preserved" if explicit_task else "restored task cleared"
    )
    print(f"retrieval SDK {wire}: open/free/create isolation, {label}", flush=True)


def tamper_case(lib, wire, mutation, after_turn):
    with tempfile.TemporaryDirectory(prefix="tny-retrieval-sdk-tamper-") as tmp:
        workspace, state = Path(tmp) / "workspace", Path(tmp) / "state"
        workspace.mkdir()
        provider = CaptureProvider()
        runtime = None
        try:
            artifact = seed_specialist(state, workspace, provider.url(), wire)
            runtime = Runtime(lib, provider, workspace, state, wire)
            runtime.open_specialist()
            if after_turn:
                runtime.turn("Read the original retained evidence")
            expected_requests = int(after_turn)
            assert provider.count() == expected_requests
            if mutation == "delete":
                artifact.unlink()
            else:
                # Keep length and inode unchanged: checking only presence/size
                # must not permit stale citation ranges to name different bytes.
                with artifact.open("r+b") as stream:
                    stream.write(b"X" + SOURCE_BYTES[1:])
            status, message = runtime.send("Continue using the pinned source")
            assert status != 0, "SDK accepted a turn with changed retained evidence"
            assert "retrieval" in message.lower() or "source" in message.lower(), (
                message
            )
            assert provider.count() == expected_requests, (
                "invalid source reached provider inference"
            )
        finally:
            if runtime:
                runtime.close()
            provider.close()
    phase = "between turns" if after_turn else "before first turn"
    print(
        f"retrieval SDK {wire}: {mutation} rejected {phase} without inference",
        flush=True,
    )


def main():
    if sys.platform not in ("darwin", "linux"):
        print("test_retrieval_sdk: skip (ABI 1 ships on Darwin/Linux only)")
        return
    name = "libtny.1.dylib" if sys.platform == "darwin" else "libtny.so.1"
    library = Path(
        os.environ.get("TNY_TEST_LIBTNY_PATH", ROOT / "build" / "lib" / name)
    )
    assert library.is_file(), (
        f"Build lib-shared-active before running this fixture: {library}"
    )
    lib = load_library(library)
    for wire in ("responses", "chat"):
        for explicit_task in (False, True):
            lifecycle_case(lib, wire, explicit_task)
        for mutation in ("modify", "delete"):
            for after_turn in (False, True):
                tamper_case(lib, wire, mutation, after_turn)
    print(
        "test_retrieval_sdk: 12 localhost-only lifecycle/integrity cases passed",
        flush=True,
    )


if __name__ == "__main__":
    main()
