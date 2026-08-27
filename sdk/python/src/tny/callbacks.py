"""Typed, lifetime-safe host-service and custom-tool callback adapters."""
from __future__ import annotations

import asyncio
import inspect
import threading
import time
from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from enum import IntEnum
from typing import Any, TYPE_CHECKING

from ._binding import STATUS_OK, copy_bytes
from .errors import BadStateError

if TYPE_CHECKING:
    from .runtime import Runtime

STATUS_INVALID_ARGUMENT = -1
STATUS_BAD_STATE = -2
STATUS_BACKPRESSURE = -11
STATUS_INTERNAL = -13
TOOL_INVOKE_SYNC = 0
TOOL_INVOKE_ASYNC = 1


class ToolSensitivity(IntEnum):
    SAFE = 0
    SENSITIVE = 1


@dataclass(frozen=True, slots=True, repr=False)
class ToolResult:
    data: bytes
    is_error: bool = False

    def __repr__(self) -> str:
        return f"ToolResult(bytes={len(self.data)}, is_error={self.is_error!r})"


SyncToolHandler = Callable[[bytes], bytes | ToolResult]
AsyncToolHandler = Callable[[bytes], Awaitable[bytes | ToolResult]]


@dataclass(frozen=True, slots=True, repr=False)
class CustomTool:
    name: str | bytes
    description: str | bytes
    input_schema_json: str | bytes
    handler: SyncToolHandler
    sensitivity: ToolSensitivity = ToolSensitivity.SAFE
    max_argument_bytes: int = 0
    max_result_bytes: int = 0

    def __repr__(self) -> str:
        return f"CustomTool(name={self.name!r}, sensitivity={self.sensitivity!r})"


@dataclass(frozen=True, slots=True, repr=False)
class AsyncCustomTool:
    name: str | bytes
    description: str | bytes
    input_schema_json: str | bytes
    handler: AsyncToolHandler
    sensitivity: ToolSensitivity = ToolSensitivity.SAFE
    max_argument_bytes: int = 0
    max_result_bytes: int = 0

    def __repr__(self) -> str:
        return f"AsyncCustomTool(name={self.name!r}, sensitivity={self.sensitivity!r})"


@dataclass(frozen=True, slots=True, repr=False)
class HostServices:
    diagnostic: Callable[[int, bytes, bytes], None] | None = None
    monotonic_ms: Callable[[], int] | None = None
    secure_random: Callable[[int], bytes] | None = None
    storage_load: Callable[[bytes], tuple[int, bytes]] | None = None
    storage_store: Callable[[bytes, int, bytes], int] | None = None
    open_url: Callable[[bytes], None] | None = None
    notify_scheduler: Callable[[], None] | None = None

    def __repr__(self) -> str:
        enabled = tuple(
            name for name in (
                "diagnostic", "monotonic_ms", "secure_random", "storage_load",
                "storage_store", "open_url", "notify_scheduler",
            ) if getattr(self, name) is not None
        )
        return f"HostServices(enabled={enabled!r})"


def _result(value: bytes | ToolResult) -> ToolResult:
    if isinstance(value, ToolResult):
        return value
    if isinstance(value, bytes):
        return ToolResult(value)
    raise TypeError("custom tool handlers must return bytes or ToolResult")


async def _await_tool_result(
    value: Awaitable[bytes | ToolResult],
) -> bytes | ToolResult:
    return await value


class _PendingCall:
    def __init__(self, call: Any, generation: int,
                 awaitable: Awaitable[bytes | ToolResult]) -> None:
        self.call = call
        self.generation = generation
        self.awaitable = awaitable
        self.future: Any | None = None
        self.armed = threading.Event()
        self.active = False
        self.started = threading.Event()
        self.handler_done = threading.Event()
        self.finished = threading.Event()
        self.value: bytes | ToolResult | None = None
        self.failed = False


class _HostBinding:
    def __init__(self, runtime: Runtime, services: HostServices) -> None:
        self.runtime = runtime
        self.services = services
        self.ffi = runtime.library.ffi
        self.table = self.ffi.new("tny_host_services_v1 *")
        size = self.ffi.sizeof("tny_host_services_v1")
        status = runtime.library.native.tny_host_services_v1_init(
            self.table, size
        )
        if status != STATUS_OK:
            runtime.library.raise_status(status, self.ffi.NULL)
        self.handle = self.ffi.new_handle(self)
        self.table.user_data = self.handle
        self.callbacks: list[Any] = []
        self._install()

    def _guard(self, function: Callable[..., int], *args: Any) -> int:
        try:
            self.runtime._enter_callback()
            return function(*args)
        except BaseException:
            return STATUS_INTERNAL
        finally:
            self.runtime._leave_callback()

    def _callback(self, declaration: str, function: Callable[..., int]) -> Any:
        def callback(user_data: Any, *args: Any) -> int:
            binding = self.ffi.from_handle(user_data)
            return int(binding._guard(function, *args))
        wrapped = self.ffi.callback(
            declaration, callback, error=STATUS_INTERNAL
        )
        self.callbacks.append(wrapped)
        return wrapped

    def _install(self) -> None:
        ffi = self.ffi
        services = self.services
        if services.diagnostic is not None:
            def diagnostic(level: Any, component: Any, message: Any) -> int:
                assert services.diagnostic is not None
                services.diagnostic(int(level), copy_bytes(ffi, component), copy_bytes(ffi, message))
                return STATUS_OK
            self.table.diagnostic = self._callback(
                "int32_t(void *, uint32_t, tny_bytes, tny_bytes)", diagnostic
            )
        if services.monotonic_ms is not None:
            def monotonic(out: Any) -> int:
                assert services.monotonic_ms is not None
                out[0] = int(services.monotonic_ms())
                return STATUS_OK
            self.table.monotonic_ms = self._callback("int32_t(void *, int64_t *)", monotonic)
        if services.secure_random is not None:
            def random_bytes(buffer: Any, size: Any) -> int:
                assert services.secure_random is not None
                value = services.secure_random(int(size))
                if not isinstance(value, bytes) or len(value) != int(size):
                    if buffer != ffi.NULL and int(size): ffi.buffer(buffer, int(size))[:] = b"\0" * int(size)
                    return STATUS_INTERNAL
                if value: ffi.buffer(buffer, len(value))[:] = value
                return STATUS_OK
            self.table.secure_random = self._callback(
                "int32_t(void *, void *, uint64_t)", random_bytes
            )
        if services.storage_load is not None:
            def storage_load(key: Any, out_revision: Any, buffer: Any,
                             capacity: Any, out_size: Any) -> int:
                assert services.storage_load is not None
                revision, data = services.storage_load(copy_bytes(ffi, key))
                if not isinstance(data, bytes) or revision < 0: return STATUS_INTERNAL
                out_revision[0] = int(revision)
                out_size[0] = len(data)
                if len(data) > int(capacity): return STATUS_BACKPRESSURE
                if data: ffi.buffer(buffer, len(data))[:] = data
                return STATUS_OK
            self.table.storage_load = self._callback(
                "int32_t(void *, tny_bytes, uint64_t *, void *, uint64_t, uint64_t *)",
                storage_load,
            )
        if services.storage_store is not None:
            def storage_store(key: Any, revision: Any, data: Any, size: Any,
                              out_revision: Any) -> int:
                assert services.storage_store is not None
                raw = b"" if data == ffi.NULL else bytes(ffi.buffer(data, int(size)))
                updated = int(services.storage_store(copy_bytes(ffi, key), int(revision), raw))
                if updated <= int(revision): return STATUS_BAD_STATE
                out_revision[0] = updated
                return STATUS_OK
            self.table.storage_store = self._callback(
                "int32_t(void *, tny_bytes, uint64_t, const void *, uint64_t, uint64_t *)",
                storage_store,
            )
        if services.open_url is not None:
            def open_url(url: Any) -> int:
                assert services.open_url is not None
                services.open_url(copy_bytes(ffi, url))
                return STATUS_OK
            self.table.open_url = self._callback("int32_t(void *, tny_bytes)", open_url)
        if services.notify_scheduler is not None:
            def notify() -> int:
                assert services.notify_scheduler is not None
                services.notify_scheduler()
                return STATUS_OK
            self.table.notify_scheduler = self._callback("int32_t(void *)", notify)


class ToolRegistration:
    def __init__(self, runtime: Runtime, tool: CustomTool | AsyncCustomTool,
                 loop: asyncio.AbstractEventLoop | None = None) -> None:
        runtime._check_open()
        self._runtime = runtime
        self._tool: CustomTool | AsyncCustomTool | None = tool
        self._name = tool.name
        self._loop = loop
        self._lock = threading.RLock()
        self._pending: dict[int, _PendingCall] = {}
        self._closed = False
        self._native_unregistered = False
        self._invalidated = threading.Event()
        self._sync_result_buffer: Any | None = None
        ffi = runtime.library.ffi
        self._handle_ref = ffi.new_handle(self)
        self._callback_ref = ffi.callback(
            "int32_t(void *, tny_tool_call *, uint64_t, tny_bytes, tny_tool_result_v1 *)",
            self._invoke, error=STATUS_INTERNAL,
        )
        spec = ffi.new("tny_tool_spec_v1 *")
        spec_size = ffi.sizeof("tny_tool_spec_v1")
        status = runtime.library.native.tny_tool_spec_v1_init(spec, spec_size)
        if status != STATUS_OK:
            runtime.library.raise_status(status, ffi.NULL)
        spec.user_data = self._handle_ref
        spec.invoke = self._callback_ref
        spec.sensitivity = int(tool.sensitivity)
        spec.max_argument_bytes = int(tool.max_argument_bytes)
        spec.max_result_bytes = int(tool.max_result_bytes)
        retained: list[Any] = []
        for field, value in (
            ("name", tool.name), ("description", tool.description),
            ("input_schema_json", tool.input_schema_json),
        ):
            from ._binding import borrowed
            buffer, view = borrowed(ffi, value)
            retained.extend((buffer, view))
            setattr(spec[0], field, view[0])
        out = ffi.new("tny_tool_registration **")
        error = ffi.new("tny_error **")
        status = runtime.library.native.tny_runtime_register_tool(
            runtime._handle, spec, out, error
        )
        if status != STATUS_OK:
            runtime.library.raise_status(status, error[0])
        self._handle = out[0]

    @property
    def closed(self) -> bool:
        return self._closed

    def _invoke(self, _user: Any, call: Any, generation: Any,
                arguments: Any, out_result: Any) -> int:
        try:
            self._runtime._enter_callback()
            raw = copy_bytes(self._runtime.library.ffi, arguments)
            tool = self._tool
            if tool is None: return STATUS_BAD_STATE
            value = tool.handler(raw)
            if inspect.isawaitable(value):
                if self._loop is None: return STATUS_INTERNAL
                pending = _PendingCall(call, int(generation), value)
                key = id(pending)
                waiter = threading.Thread(
                    target=self._finish_pending, args=(key, pending), daemon=True,
                    name="libtny-python-tool-completion",
                )
                try:
                    waiter.start()
                except BaseException:
                    if inspect.iscoroutine(value): value.close()
                    return STATUS_INTERNAL
                runner = self._run_async_handler(pending)
                try:
                    future = asyncio.run_coroutine_threadsafe(runner, self._loop)
                except BaseException:
                    runner.close()
                    if inspect.iscoroutine(value): value.close()
                    pending.armed.set()
                    return STATUS_INTERNAL
                pending.future = future
                published = False
                try:
                    self._publication_hook("before_insert")
                    with self._lock: self._pending[key] = pending
                    self._publication_hook("after_insert")
                    future.add_done_callback(
                        lambda completed: self._future_terminal(pending, completed)
                    )
                    self._publication_hook("after_done_callback")
                    pending.active = True
                    self._publication_hook("before_arm")
                    published = True
                except BaseException:
                    pending.active = False
                    with self._lock: self._pending.pop(key, None)
                    future.cancel()
                    self._future_terminal(pending, future)
                finally:
                    pending.armed.set()
                if not published:
                    pending.finished.wait(5.0)
                    return STATUS_INTERNAL
                return TOOL_INVOKE_ASYNC
            result = self._validate_result(_result(value))
            self._sync_result_buffer = self._write_result(out_result, result)
            return TOOL_INVOKE_SYNC
        except (TypeError, UnicodeError, ValueError):
            return STATUS_INVALID_ARGUMENT
        except BaseException:
            return STATUS_INTERNAL
        finally:
            self._runtime._leave_callback()

    def _write_result(self, out_result: Any, result: ToolResult) -> Any:
        ffi = self._runtime.library.ffi
        size = ffi.sizeof("tny_tool_result_v1")
        status = self._runtime.library.native.tny_tool_result_v1_init(
            out_result, size
        )
        if status != STATUS_OK:
            self._runtime.library.raise_status(status, ffi.NULL)
        buffer = ffi.new("char[]", result.data)
        out_result.data.ptr = buffer
        out_result.data.len = len(result.data)
        out_result.is_error = int(result.is_error)
        return buffer

    def _validate_result(self, result: ToolResult) -> ToolResult:
        data = result.data
        if not isinstance(data, bytes) or b"\0" in data:
            raise ValueError("tool result must be NUL-free UTF-8 bytes")
        data.decode("utf-8", "strict")
        tool = self._tool
        configured = tool.max_result_bytes if tool is not None else 0
        maximum = configured or self._runtime.capabilities.custom_tool_result_max or 1048576
        if len(data) > maximum:
            raise ValueError("tool result exceeds its declared bound")
        return result

    async def _run_async_handler(self, pending: _PendingCall) -> None:
        pending.started.set()
        try:
            pending.value = await _await_tool_result(pending.awaitable)
        except BaseException:
            pending.failed = True
        finally:
            pending.handler_done.set()

    def _future_terminal(self, pending: _PendingCall, future: Any) -> None:
        if future.cancelled() and not pending.started.is_set():
            if inspect.iscoroutine(pending.awaitable): pending.awaitable.close()
            pending.failed = True
            pending.handler_done.set()

    def _publication_hook(self, _stage: str) -> None:
        """Deterministic fault-injection seam for transactional publication."""

    def _complete_native(self, call: Any, generation: int,
                         result: ToolResult) -> int:
        library = self._runtime.library
        native_result = library.ffi.new("tny_tool_result_v1 *")
        buffer = self._write_result(native_result, result)
        error = library.ffi.new("tny_error **")
        status = int(library.native.tny_tool_call_complete(
            call, generation, native_result, error
        ))
        if error[0] != library.ffi.NULL:
            library.native.tny_error_free(error[0])
        _ = buffer
        return status

    def _release_native(self, call: Any) -> None:
        self._runtime.library.native.tny_tool_call_release(call)

    def _request_cancel(self) -> bool:
        session = self._runtime._session
        if session is not None and not session.closed:
            try:
                session.cancel()
                return True
            except BaseException:
                return False
        return False

    def _finish_pending(self, key: int, pending: _PendingCall) -> None:
        library = self._runtime.library
        release_authority = True
        try:
            pending.armed.wait()
            if not pending.active:
                release_authority = False
                pending.handler_done.wait()
                pending.finished.set()
                return
            try:
                pending.handler_done.wait()
                if pending.failed or pending.value is None:
                    raise ValueError("handler failed")
                result = self._validate_result(_result(pending.value))
            except BaseException:
                result = ToolResult(b"custom tool handler failed", is_error=True)
            try:
                status = self._complete_native(pending.call, pending.generation, result)
            except BaseException:
                status = STATUS_INTERNAL
            if status not in (STATUS_OK, STATUS_BAD_STATE):
                fallback = ToolResult(b"custom tool completion failed", is_error=True)
                try: fallback_status = self._complete_native(
                    pending.call, pending.generation, fallback
                )
                except BaseException: fallback_status = STATUS_INTERNAL
                if fallback_status not in (STATUS_OK, STATUS_BAD_STATE):
                    release_authority = self._request_cancel()
                    if not release_authority:
                        session = self._runtime._session
                        if session is None or session.closed:
                            release_authority = True
                        else:
                            while not self._invalidated.wait(0.05):
                                if session.closed:
                                    break
                            release_authority = True
        finally:
            if release_authority:
                self._release_native(pending.call)
                with self._lock: self._pending.pop(key, None)
                pending.finished.set()

    def _close(self, *, refresh: bool) -> None:
        self._runtime._check_owner()
        with self._lock:
            if self._closed: return
        if not self._native_unregistered:
            error = self._runtime.library.ffi.new("tny_error **")
            status = self._runtime.library.native.tny_tool_registration_unregister(
                self._handle, error
            )
            if status != STATUS_OK:
                self._runtime.library.raise_status(status, error[0])
            self._native_unregistered = True
            self._invalidated.set()
        with self._lock:
            pending = list(self._pending.values())
            for item in pending:
                if item.future is not None: item.future.cancel()
        deadline = time.monotonic() + 5.0
        for item in pending:
            remaining = max(0.0, deadline - time.monotonic())
            item.finished.wait(remaining)
        with self._lock:
            if self._pending: raise BadStateError(-2)
            self._closed = True
            self._handle = self._runtime.library.ffi.NULL
            self._sync_result_buffer = None
            self._tool = None
            self._callback_ref = None
            self._handle_ref = None
        self._runtime._forget_registration(self, refresh=refresh)

    def close(self) -> None:
        self._close(refresh=True)

    def __enter__(self) -> ToolRegistration:
        if self._closed: raise BadStateError(-2)
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def __repr__(self) -> str:
        return f"ToolRegistration(closed={self._closed}, name={self._name!r})"
