"""Owner-thread-safe synchronous libtny runtime and session adapters."""
from __future__ import annotations

import os
import threading
import time
import warnings
from collections.abc import Callable, Iterator
from dataclasses import dataclass, replace
from enum import IntEnum
from types import MappingProxyType
from urllib.parse import urlsplit

from typing import Any, TypedDict, TYPE_CHECKING

from ._binding import (
    STATUS_DRAINED,
    STATUS_EVENT,
    STATUS_OK,
    STATUS_TIMEOUT,
    Library,
    borrowed,
    copy_bytes,
)
from .errors import BadStateError, InvalidArgumentError
from .events import (
    AnyEvent,
    CustomMessageEvent,
    ErrorEvent,
    EVENT_TYPES_BY_KIND,
    EventStreamError,
    PermissionOptions,
    PermissionRequestEvent,
    PlanEvent,
    StatusEvent,
    SteerRejectedEvent,
    TextDeltaEvent,
    ThinkingEvent,
    ToolEndEvent,
    ToolProgressEvent,
    ToolStartEvent,
    TurnEndEvent,
    UnknownEvent,
    UsageEvent,
    UserMessageEvent,
)

if TYPE_CHECKING:
    from .callbacks import AsyncCustomTool, CustomTool, HostServices, ToolRegistration


class _EventKwargs(TypedDict):
    kind: int
    schema_version: int
    sequence: int
    timestamp_ms: int
    provider: bytes
    session_id: bytes
    turn_id: bytes


class _ToolKwargs(TypedDict):
    tool_name: bytes
    tool_id: bytes
    tool_detail: bytes


class PermissionMode(IntEnum):
    ASK = 0
    AUTO = 1
    YOLO = 2


class PermissionDecision(IntEnum):
    ALLOW = 0
    ALLOW_ALWAYS = 1
    DENY = 2


@dataclass(frozen=True, slots=True, repr=False)
class RuntimeConfig:
    workspace: str | os.PathLike[str]
    state_dir: str | os.PathLike[str] | None = None
    provider: str | bytes = "openai"
    model: str | bytes = b""
    base_url: str | bytes = b""
    api_key: str | bytes = b""
    wire_api: str | bytes = "responses"
    permission_mode: PermissionMode = PermissionMode.ASK
    persistence: bool = False
    max_steps: int = 0
    max_tool_result_bytes: int = 0

    def __repr__(self) -> str:
        # base_url and api_key may contain credentials and are intentionally
        # absent. Paths are useful diagnostics and are not sent to providers.
        return (
            "RuntimeConfig("
            f"workspace={os.fspath(self.workspace)!r}, "
            f"state_dir={os.fspath(self.state_dir) if self.state_dir is not None else None!r}, "
            f"provider={self.provider!r}, model={self.model!r}, "
            f"wire_api={self.wire_api!r}, "
            f"permission_mode={self.permission_mode!r}, "
            f"persistence={self.persistence!r}, max_steps={self.max_steps!r}, "
            f"max_tool_result_bytes={self.max_tool_result_bytes!r})"
        )


class CancellationToken:
    """A thread-safe request flag; native cancellation runs on the owner."""

    def __init__(self) -> None:
        self._requested = threading.Event()

    def cancel(self) -> None:
        self._requested.set()

    @property
    def requested(self) -> bool:
        return self._requested.is_set()


def _validate_config(config: RuntimeConfig) -> None:
    if not 0 <= int(config.max_steps) <= 0x7FFFFFFF:
        raise InvalidArgumentError(-1)
    if not 0 <= int(config.max_tool_result_bytes) <= 0xFFFFFFFFFFFFFFFF:
        raise InvalidArgumentError(-1)
    if config.persistence and config.state_dir is None:
        raise InvalidArgumentError(-1)
    raw_url = config.base_url.decode("utf-8", "strict") if isinstance(config.base_url, bytes) else config.base_url
    if raw_url:
        parsed = urlsplit(raw_url)
        if parsed.username is not None or parsed.password is not None:
            # Userinfo would make ordinary URL diagnostics credential-bearing.
            raise InvalidArgumentError(-1)


class Runtime:
    """One explicit libtny runtime, owned by its creating thread."""

    def __init__(self, config: RuntimeConfig, *, library: Library | None = None,
                 library_path: str | os.PathLike[str] | None = None,
                 host_services: HostServices | None = None) -> None:
        _validate_config(config)
        # Retain only the non-secret configuration snapshot. The caller still
        # owns its input object and credential; the Runtime must not keep a
        # second Python reference after native creation copies it.
        self.config = replace(config, api_key=b"")
        self.library = library or Library(library_path)
        ffi = self.library.ffi
        native = self.library.native
        self._owner = threading.get_ident()
        self._callback_depth = 0
        self._handle = ffi.NULL
        self._session: Session | None = None
        self._registrations: list[ToolRegistration] = []
        self._host_binding: Any | None = None
        if host_services is not None and self.library.abi_minor < 6:
            from .errors import UnsupportedError
            raise UnsupportedError(-9)
        opts = ffi.new("tny_runtime_options_v0 *")
        native.tny_runtime_options_init(opts)
        opts.permission_mode = int(config.permission_mode)
        opts.persistence = int(config.persistence)
        opts.max_steps = int(config.max_steps)
        if config.max_tool_result_bytes:
            opts.max_tool_result_bytes = int(config.max_tool_result_bytes)
        retained: list[Any] = []
        values = {
            "workspace": config.workspace,
            "state_dir": config.state_dir or b"",
            "provider": config.provider, "model": config.model,
            "base_url": config.base_url, "api_key": config.api_key,
            "wire_api": config.wire_api,
        }
        api_key_buffer: Any | None = None
        try:
            for name, value in values.items():
                buffer, view = borrowed(ffi, value)
                retained.extend((buffer, view))
                setattr(opts[0], name, view[0])
                if name == "api_key":
                    api_key_buffer = buffer
            out = ffi.new("tny_runtime **")
            error = ffi.new("tny_error **")
            if host_services is None:
                status = native.tny_runtime_create(opts, out, error)
            else:
                from .callbacks import _HostBinding
                self._host_binding = _HostBinding(self, host_services)
                options_v1 = ffi.new("tny_runtime_options_v1 *")
                native.tny_runtime_options_v1_init(options_v1)
                options_v1.runtime = opts[0]
                options_v1.host_services = self._host_binding.table
                status = native.tny_runtime_create_v1(options_v1, out, error)
        finally:
            if api_key_buffer is not None:
                size = ffi.sizeof(api_key_buffer)
                ffi.buffer(api_key_buffer, size)[:] = b"\0" * size
        if status != STATUS_OK:
            self.library.raise_status(status, error[0])
        self._handle = out[0]
        self.capabilities = self.library.read_capabilities(
            self._handle, extended=self.library.abi_minor >= 7
        )

    def _enter_callback(self) -> None:
        self._callback_depth += 1

    def _leave_callback(self) -> None:
        self._callback_depth = max(0, self._callback_depth - 1)

    def _check_owner(self) -> None:
        if threading.get_ident() != self._owner:
            raise BadStateError(-2)

    def _check_open(self) -> None:
        self._check_owner()
        if self._callback_depth:
            raise BadStateError(-2)
        if self._handle == self.library.ffi.NULL:
            raise BadStateError(-2)

    @property
    def closed(self) -> bool:
        return bool(self._handle == self.library.ffi.NULL)

    def create_session(self) -> Session:
        self._check_open()
        if self._session is not None and not self._session.closed:
            raise BadStateError(-2)
        ffi = self.library.ffi
        handle = ffi.new("tny_session **")
        error = ffi.new("tny_error **")
        status = self.library.native.tny_session_create(self._handle, handle, error)
        if status != STATUS_OK:
            self.library.raise_status(status, error[0])
        self._session = Session(self, handle[0])
        return self._session

    def open_session(self, session_id: str | bytes) -> Session:
        self._check_open()
        if self._session is not None and not self._session.closed:
            raise BadStateError(-2)
        ffi = self.library.ffi
        _buffer, view = borrowed(ffi, session_id)
        handle = ffi.new("tny_session **")
        error = ffi.new("tny_error **")
        status = self.library.native.tny_session_open(
            self._handle, view[0], handle, error
        )
        if status != STATUS_OK:
            self.library.raise_status(status, error[0])
        self._session = Session(self, handle[0])
        return self._session

    def register_tool(self, tool: CustomTool | AsyncCustomTool, *,
                      _loop: Any | None = None) -> ToolRegistration:
        self._check_open()
        from .callbacks import ToolRegistration
        registration = ToolRegistration(self, tool, _loop)
        try:
            self._registrations.append(registration)
            self.capabilities = self.library.read_capabilities(
                self._handle, extended=True
            )
        except BaseException:
            if registration in self._registrations:
                self._registrations.remove(registration)
            registration._close(refresh=False)
            raise
        return registration

    def _forget_registration(self, registration: ToolRegistration, *,
                             refresh: bool) -> None:
        if registration in self._registrations:
            self._registrations.remove(registration)
        if refresh and self._handle != self.library.ffi.NULL:
            self.capabilities = self.library.read_capabilities(
                self._handle, extended=True
            )

    def host_monotonic_ms(self) -> int:
        self._check_open()
        out = self.library.ffi.new("int64_t *")
        error = self.library.ffi.new("tny_error **")
        status = self.library.native.tny_runtime_host_monotonic_ms(
            self._handle, out, error
        )
        if status != STATUS_OK: self.library.raise_status(status, error[0])
        return int(out[0])

    def host_secure_random(self, size: int) -> bytes:
        self._check_open()
        if size < 0 or size > 1024 * 1024: raise InvalidArgumentError(-1)
        buffer = self.library.ffi.new("unsigned char[]", size)
        error = self.library.ffi.new("tny_error **")
        status = self.library.native.tny_runtime_host_secure_random(
            self._handle, buffer, size, error
        )
        if status != STATUS_OK: self.library.raise_status(status, error[0])
        return bytes(self.library.ffi.buffer(buffer, size))

    def host_storage_load(self, key: str | bytes, *,
                          capacity: int = 1024 * 1024) -> tuple[int, bytes]:
        self._check_open()
        if capacity < 0 or capacity > 1024 * 1024: raise InvalidArgumentError(-1)
        _key_buffer, key_view = borrowed(self.library.ffi, key)
        revision = self.library.ffi.new("uint64_t *")
        out_size = self.library.ffi.new("uint64_t *")
        buffer = self.library.ffi.new("unsigned char[]", capacity)
        error = self.library.ffi.new("tny_error **")
        status = self.library.native.tny_runtime_host_storage_load(
            self._handle, key_view[0], revision, buffer, capacity, out_size, error
        )
        if status != STATUS_OK: self.library.raise_status(status, error[0])
        if int(out_size[0]) > capacity: raise BadStateError(-2)
        return int(revision[0]), bytes(self.library.ffi.buffer(buffer, int(out_size[0])))

    def host_storage_store(self, key: str | bytes, revision: int,
                           data: bytes) -> int:
        self._check_open()
        if revision < 0 or not isinstance(data, bytes): raise InvalidArgumentError(-1)
        _key_buffer, key_view = borrowed(self.library.ffi, key)
        data_buffer = self.library.ffi.new("char[]", data)
        out_revision = self.library.ffi.new("uint64_t *")
        error = self.library.ffi.new("tny_error **")
        status = self.library.native.tny_runtime_host_storage_store(
            self._handle, key_view[0], revision, data_buffer, len(data),
            out_revision, error
        )
        if status != STATUS_OK: self.library.raise_status(status, error[0])
        return int(out_revision[0])

    def host_open_url(self, url: str | bytes) -> None:
        self._check_open()
        _buffer, view = borrowed(self.library.ffi, url)
        error = self.library.ffi.new("tny_error **")
        status = self.library.native.tny_runtime_host_open_url(
            self._handle, view[0], error
        )
        if status != STATUS_OK: self.library.raise_status(status, error[0])

    def host_notify_scheduler(self) -> None:
        self._check_open()
        error = self.library.ffi.new("tny_error **")
        status = self.library.native.tny_runtime_host_notify_scheduler(
            self._handle, error
        )
        if status != STATUS_OK: self.library.raise_status(status, error[0])

    def close(self) -> None:
        self._check_owner()
        if self._handle == self.library.ffi.NULL:
            return
        if self._session is not None:
            self._session.close()
        for registration in reversed(tuple(self._registrations)):
            registration._close(refresh=False)
        self._registrations.clear()
        self.library.native.tny_runtime_free(self._handle)
        self._handle = self.library.ffi.NULL
        self._host_binding = None

    def __enter__(self) -> Runtime:
        self._check_open()
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        if (getattr(self, "_handle", None) is not None and
                getattr(self, "library", None) is not None and
                self._handle != self.library.ffi.NULL):
            warnings.warn("unclosed tny.Runtime; GC cleanup is a fallback", ResourceWarning)
            if threading.get_ident() == getattr(self, "_owner", None):
                try:
                    self.close()
                except Exception:  # noqa: BLE001, S110
                    pass

    def __repr__(self) -> str:
        return f"Runtime(closed={self.closed})"


class Session:
    """A single session whose native handle is explicitly closed."""

    def __init__(self, runtime: Runtime, handle: Any) -> None:
        self._runtime = runtime
        self._handle = handle
        self._lifetime_lock = threading.RLock()
        self._turn_active = False
        self._native_cancel_requested = False

    @property
    def closed(self) -> bool:
        return bool(self._handle == self._runtime.library.ffi.NULL)

    def _check_open(self) -> None:
        self._runtime._check_open()
        if self._handle == self._runtime.library.ffi.NULL:
            raise BadStateError(-2)

    @property
    def id(self) -> bytes:
        self._check_open()
        library = self._runtime.library
        return copy_bytes(library.ffi, library.native.tny_session_id(self._handle))

    def send(self, prompt: str | bytes) -> None:
        self._check_open()
        library = self._runtime.library
        _buffer, value = borrowed(library.ffi, prompt)
        error = library.ffi.new("tny_error **")
        status = library.native.tny_session_send(self._handle, value[0], error)
        if status != STATUS_OK:
            library.raise_status(status, error[0])
        self._turn_active = True
        self._native_cancel_requested = False

    def steer(self, text: str | bytes) -> None:
        self._check_open()
        library = self._runtime.library
        _buffer, value = borrowed(library.ffi, text)
        error = library.ffi.new("tny_error **")
        status = library.native.tny_session_steer(self._handle, value[0], error)
        if status != STATUS_OK:
            library.raise_status(status, error[0])

    def respond_permission(self, request: PermissionRequestEvent | str | bytes,
                           decision: PermissionDecision) -> None:
        self._check_open()
        request_id = request.permission_id if isinstance(request, PermissionRequestEvent) else request
        library = self._runtime.library
        _buffer, value = borrowed(library.ffi, request_id)
        error = library.ffi.new("tny_error **")
        status = library.native.tny_session_respond_permission(
            self._handle, value[0], int(decision), error
        )
        if status != STATUS_OK:
            library.raise_status(status, error[0])

    def cancel(self) -> None:
        # ABI 0.5 makes cancel the sole cross-thread-safe session operation.
        # Serialize it with close so the native handle cannot be freed while a
        # Python-initiated cancel is entering or returning from libtny.
        with self._lifetime_lock:
            library = self._runtime.library
            if self._handle == library.ffi.NULL:
                raise BadStateError(-2)
            error = library.ffi.new("tny_error **")
            status = library.native.tny_session_cancel(self._handle, error)
            if status != STATUS_OK:
                library.raise_status(status, error[0])
            self._native_cancel_requested = True

    def _service_cancellation(self, token: CancellationToken | None) -> None:
        if token is not None and token.requested and not self._native_cancel_requested:
            self.cancel()

    def next_event(self, timeout: float | None = None, *,
                   cancellation: CancellationToken | None = None) -> AnyEvent | None:
        """Return an event, ``None`` on timeout, or raise StopIteration drained.

        cffi releases the GIL during the blocking native call. When a
        cancellation token is supplied, waits are sliced to at most 50 ms so a
        request from any Python thread is executed by this owning thread.
        """
        self._check_open()
        if timeout is not None and timeout < 0:
            raise InvalidArgumentError(-1)
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            self._service_cancellation(cancellation)
            if deadline is None:
                wait_ms = 50 if cancellation is not None else 1000
            else:
                remaining = max(0.0, deadline - time.monotonic())
                wait_ms = min(600000, max(0, int(remaining * 1000)))
                if cancellation is not None:
                    wait_ms = min(wait_ms, 50)
            library = self._runtime.library
            event = library.ffi.new("tny_event **")
            error = library.ffi.new("tny_error **")
            status = library.native.tny_session_next_event(
                self._handle, wait_ms, event, error
            )
            if status == STATUS_EVENT:
                try:
                    result = self._copy_event(event[0])
                finally:
                    library.native.tny_event_free(event[0])
                return result
            if status == STATUS_DRAINED:
                self._turn_active = False
                raise StopIteration
            if status != STATUS_TIMEOUT:
                library.raise_status(status, error[0])
            self._service_cancellation(cancellation)
            if deadline is not None and time.monotonic() >= deadline:
                return None

    def events(self, *, cancellation: CancellationToken | None = None,
               raise_on_error: bool = False) -> Iterator[AnyEvent]:
        drained = False
        try:
            while True:
                try:
                    event = self.next_event(cancellation=cancellation)
                except StopIteration:
                    drained = True
                    return
                if event is None:
                    continue
                if raise_on_error and isinstance(event, ErrorEvent):
                    raise EventStreamError(event)
                yield event
        finally:
            if not drained and self._turn_active and not self.closed:
                self._cancel_and_drain()

    def run(self, prompt: str | bytes, *, cancellation: CancellationToken | None = None,
            raise_on_error: bool = False) -> Iterator[AnyEvent]:
        self.send(prompt)
        yield from self.events(cancellation=cancellation, raise_on_error=raise_on_error)

    def _cancel_and_drain(self) -> None:
        """Resolve an abandoned iterator; close only if drain misses its bound."""
        try:
            self.cancel()
        except BadStateError:
            pass
        deadline = time.monotonic() + 5.0
        while not self.closed:
            try:
                self.next_event(0.1)
            except StopIteration:
                return
            except BadStateError:
                return
            if time.monotonic() >= deadline:
                self.close()
                return

    def close(self) -> None:
        self._runtime._check_owner()
        with self._lifetime_lock:
            if self._handle == self._runtime.library.ffi.NULL:
                return
            self._runtime.library.native.tny_session_free(self._handle)
            self._handle = self._runtime.library.ffi.NULL
            self._turn_active = False
            if self._runtime._session is self:
                self._runtime._session = None

    def __enter__(self) -> Session:
        self._check_open()
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        if (getattr(self, "_handle", None) is not None and
                self._handle != self._runtime.library.ffi.NULL):
            warnings.warn("unclosed tny.Session; GC cleanup is a fallback", ResourceWarning)
            if threading.get_ident() == getattr(self._runtime, "_owner", None):
                try:
                    self.close()
                except Exception:  # noqa: BLE001, S110
                    pass

    def __repr__(self) -> str:
        return f"Session(closed={self.closed}, turn_active={self._turn_active})"

    def _copy_event(self, handle: Any) -> AnyEvent:
        library = self._runtime.library
        view_ptr = library.ffi.new("tny_event_view_v0 *")
        library.native.tny_event_view_init(view_ptr)
        status = library.native.tny_event_read(handle, view_ptr)
        if status != STATUS_OK:
            raise InvalidArgumentError(status)
        view = view_ptr[0]
        common: _EventKwargs = {
            "kind": int(view.kind),
            "schema_version": int(view.schema_version),
            "sequence": int(view.sequence),
            "timestamp_ms": int(view.timestamp_ms),
            "provider": copy_bytes(library.ffi, view.provider),
            "session_id": copy_bytes(library.ffi, view.session_id),
            "turn_id": copy_bytes(library.ffi, view.turn_id),
        }
        text = copy_bytes(library.ffi, view.text)
        message_id = copy_bytes(library.ffi, view.message_id)
        tool: _ToolKwargs = {
            "tool_name": copy_bytes(library.ffi, view.tool_name),
            "tool_id": copy_bytes(library.ffi, view.tool_id),
            "tool_detail": copy_bytes(library.ffi, view.tool_detail),
        }
        constructors: dict[int, Callable[[], AnyEvent]] = {
            0: lambda: TextDeltaEvent(type="text_delta", text=text, message_id=message_id, **common),
            1: lambda: ThinkingEvent(type="thinking", text=text, message_id=message_id, **common),
            2: lambda: ToolStartEvent(type="tool_start", **tool, **common),
            3: lambda: ToolEndEvent(type="tool_end", ok=bool(view.tool_ok), **tool, **common),
            4: lambda: PermissionRequestEvent(type="permission_request", permission_id=copy_bytes(library.ffi, view.permission_id), summary=copy_bytes(library.ffi, view.permission_summary), options=PermissionOptions(view.permission_options), **common),
            5: lambda: PlanEvent(type="plan", text=text, message_id=message_id, **common),
            6: lambda: UsageEvent(type="usage", input_tokens=int(view.input_tokens), output_tokens=int(view.output_tokens), context_used=int(view.context_used), context_size=int(view.context_size), cost=float(view.cost) if view.has_cost else None, **common),
            7: lambda: TurnEndEvent(type="turn_end", stop_reason=int(view.stop_reason), **common),
            8: lambda: ErrorEvent(type="error", text=text, error_code=int(view.error_code), **common),
            9: lambda: StatusEvent(type="status", text=text, message_id=message_id, **common),
            10: lambda: SteerRejectedEvent(type="steer_rejected", text=text, message_id=message_id, **common),
            11: lambda: CustomMessageEvent(type="custom_message", text=text, message_id=message_id, message_type=copy_bytes(library.ffi, view.message_type), **common),
            12: lambda: UserMessageEvent(type="user_message", text=text, message_id=message_id, **common),
            13: lambda: ToolProgressEvent(type="tool_progress", **tool, **common),
        }
        if set(constructors) != set(EVENT_TYPES_BY_KIND):
            raise RuntimeError("SDK event constructor registry is inconsistent")
        constructor = constructors.get(int(view.kind))
        if constructor:
            return constructor()
        payload = MappingProxyType({
            "text": text,
            "message_id": message_id,
            "tool_name": tool["tool_name"],
            "tool_id": tool["tool_id"],
            "tool_detail": tool["tool_detail"],
            "tool_ok": bool(view.tool_ok),
            "permission_id": copy_bytes(library.ffi, view.permission_id),
            "permission_summary": copy_bytes(
                library.ffi, view.permission_summary
            ),
            "permission_options": int(view.permission_options),
            "stop_reason": int(view.stop_reason),
            "error_code": int(view.error_code),
            "input_tokens": int(view.input_tokens),
            "output_tokens": int(view.output_tokens),
            "context_used": int(view.context_used),
            "context_size": int(view.context_size),
            "cost": float(view.cost) if view.has_cost else None,
            "message_type": copy_bytes(library.ffi, view.message_type),
        })
        return UnknownEvent(type="unknown", payload=payload, **common)
