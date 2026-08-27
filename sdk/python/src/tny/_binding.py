"""Private cffi ABI-mode declarations for stable libtny ABI 1."""

from __future__ import annotations

import ctypes.util
import os
import platform
from dataclasses import dataclass
from importlib import resources
from pathlib import Path
from typing import Any

from cffi import FFI  # type: ignore[import-untyped]

from .errors import ConfigurationError, UnsupportedError, error_from_status

STATUS_OK = 0
STATUS_EVENT = 1
STATUS_TIMEOUT = 2
STATUS_DRAINED = 3

SUPPORTED_ABI_MAJOR = 1
SUPPORTED_ABI_MINOR_MIN = 0
SUPPORTED_ABI_MINOR_MAX: int | None = None

FEATURE_TLS = 1 << 0
FEATURE_PERSISTENCE = 1 << 1
FEATURE_SHARED_LIBRARY = 1 << 2
FEATURE_MCP = 1 << 4
FEATURE_CUSTOM_TOOLS = 1 << 5
FEATURE_CROSS_THREAD_CANCEL = 1 << 7
FEATURE_WINDOWS = 1 << 8
FEATURE_HOST_SERVICES = 1 << 11

CDEF = r"""
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef struct tny_runtime tny_runtime;
typedef struct tny_session tny_session;
typedef struct tny_event tny_event;
typedef struct tny_error tny_error;
typedef struct tny_tool_registration tny_tool_registration;
typedef struct tny_tool_call tny_tool_call;
typedef struct { const char *ptr; uint64_t len; } tny_bytes;
typedef struct {
    uint32_t struct_size; uint32_t permission_mode; uint32_t persistence;
    uint32_t max_steps; uint64_t max_tool_result_bytes;
    tny_bytes workspace; tny_bytes state_dir; tny_bytes provider;
    tny_bytes model; tny_bytes base_url; tny_bytes api_key;
    tny_bytes wire_api; uint64_t reserved[8];
} tny_runtime_options_v0;
typedef int32_t (*tny_host_diagnostic_fn)(void *, uint32_t, tny_bytes, tny_bytes);
typedef int32_t (*tny_host_monotonic_ms_fn)(void *, int64_t *);
typedef int32_t (*tny_host_secure_random_fn)(void *, void *, uint64_t);
typedef int32_t (*tny_host_storage_load_fn)(void *, tny_bytes, uint64_t *, void *, uint64_t, uint64_t *);
typedef int32_t (*tny_host_storage_store_fn)(void *, tny_bytes, uint64_t, const void *, uint64_t, uint64_t *);
typedef int32_t (*tny_host_open_url_fn)(void *, tny_bytes);
typedef int32_t (*tny_host_notify_scheduler_fn)(void *);
typedef struct {
    uint32_t abi_version; uint32_t struct_size; void *user_data;
    tny_host_diagnostic_fn diagnostic; tny_host_monotonic_ms_fn monotonic_ms;
    tny_host_secure_random_fn secure_random; tny_host_storage_load_fn storage_load;
    tny_host_storage_store_fn storage_store; tny_host_open_url_fn open_url;
    tny_host_notify_scheduler_fn notify_scheduler; uint64_t reserved[8];
} tny_host_services_v1;
typedef struct {
    uint32_t abi_version; uint32_t struct_size; tny_runtime_options_v0 runtime;
    const tny_host_services_v1 *host_services; uint64_t reserved[8];
} tny_runtime_options_v1;
typedef struct {
    uint32_t abi_version; uint32_t struct_size; tny_bytes data;
    uint32_t is_error; uint32_t reserved_scalar; uint64_t reserved[4];
} tny_tool_result_v1;
typedef int32_t (*tny_tool_invoke_fn)(void *, tny_tool_call *, uint64_t,
                                      tny_bytes, tny_tool_result_v1 *);
typedef struct {
    uint32_t abi_version; uint32_t struct_size; void *user_data;
    tny_bytes name; tny_bytes description; tny_bytes input_schema_json;
    uint32_t sensitivity; uint32_t reserved_scalar;
    uint64_t max_argument_bytes; uint64_t max_result_bytes;
    tny_tool_invoke_fn invoke; uint64_t reserved[8];
} tny_tool_spec_v1;
typedef struct {
    uint32_t struct_size; uint32_t kind; uint32_t schema_version;
    uint32_t tool_ok; uint32_t permission_options; uint32_t stop_reason;
    int32_t error_code; uint32_t has_cost; uint64_t sequence;
    int64_t timestamp_ms; int64_t input_tokens; int64_t output_tokens;
    int64_t context_used; int64_t context_size; double cost;
    tny_bytes provider; tny_bytes session_id; tny_bytes turn_id;
    tny_bytes text; tny_bytes message_id; tny_bytes tool_name;
    tny_bytes tool_id; tny_bytes tool_detail; tny_bytes permission_id;
    tny_bytes permission_summary; tny_bytes message_type;
    uint64_t reserved[8];
} tny_event_view_v0;
typedef struct {
    uint32_t struct_size; uint32_t schema_version; uint32_t abi_version;
    uint32_t provider_selected; uint32_t provider_initialized;
    uint32_t endpoint_reachability; uint32_t threading_model;
    uint32_t cancel_model; uint64_t provider_available_mask;
    uint64_t feature_available_mask; uint64_t feature_enabled_mask;
    uint32_t event_queue_max; uint32_t event_reserved;
    uint64_t event_payload_bytes_max; uint64_t event_reserved_bytes;
    tny_bytes library_version; tny_bytes platform_family;
    tny_bytes architecture; tny_bytes transport; tny_bytes tls_implementation;
    tny_bytes linkage; uint64_t reserved[8];
} tny_capabilities_v0;
typedef struct {
    uint32_t abi_version; uint32_t struct_size; tny_capabilities_v0 base;
    uint32_t custom_tool_max_count; uint32_t custom_tool_name_max;
    uint64_t custom_tool_schema_max; uint64_t custom_tool_arguments_max;
    uint64_t custom_tool_result_max; uint64_t reserved[8];
} tny_capabilities_v1;
uint32_t tny_abi_version(void);
tny_bytes tny_library_version(void);
int32_t tny_runtime_options_init(tny_runtime_options_v0 *, uint64_t);
int32_t tny_runtime_options_v1_init(tny_runtime_options_v1 *, uint64_t);
int32_t tny_host_services_v1_init(tny_host_services_v1 *, uint64_t);
int32_t tny_tool_spec_v1_init(tny_tool_spec_v1 *, uint64_t);
int32_t tny_tool_result_v1_init(tny_tool_result_v1 *, uint64_t);
int32_t tny_capabilities_init(tny_capabilities_v0 *, uint64_t);
int32_t tny_capabilities_v1_init(tny_capabilities_v1 *, uint64_t);
int32_t tny_runtime_create(const tny_runtime_options_v0 *, uint64_t,
                           tny_runtime **, tny_error **);
int32_t tny_runtime_create_v1(const tny_runtime_options_v1 *, uint64_t,
                              tny_runtime **, tny_error **);
void tny_runtime_free(tny_runtime *);
int32_t tny_runtime_destroy(tny_runtime **);
int32_t tny_runtime_get_capabilities(const tny_runtime *, tny_capabilities_v0 *, uint64_t);
int32_t tny_runtime_get_capabilities_v1(const tny_runtime *, tny_capabilities_v1 *, uint64_t);
int32_t tny_runtime_register_tool(tny_runtime *, const tny_tool_spec_v1 *,
                                  tny_tool_registration **, tny_error **);
int32_t tny_tool_registration_unregister(tny_tool_registration *, tny_error **);
uint64_t tny_tool_call_generation(const tny_tool_call *);
int32_t tny_tool_call_complete(tny_tool_call *, uint64_t,
                               const tny_tool_result_v1 *, tny_error **);
void tny_tool_call_release(tny_tool_call *);
int32_t tny_runtime_host_monotonic_ms(tny_runtime *, int64_t *, tny_error **);
int32_t tny_runtime_host_secure_random(tny_runtime *, void *, uint64_t, tny_error **);
int32_t tny_runtime_host_storage_load(tny_runtime *, tny_bytes, uint64_t *,
                                      void *, uint64_t, uint64_t *, tny_error **);
int32_t tny_runtime_host_storage_store(tny_runtime *, tny_bytes, uint64_t,
                                       const void *, uint64_t, uint64_t *, tny_error **);
int32_t tny_runtime_host_open_url(tny_runtime *, tny_bytes, tny_error **);
int32_t tny_runtime_host_notify_scheduler(tny_runtime *, tny_error **);
int32_t tny_session_create(tny_runtime *, tny_session **, tny_error **);
int32_t tny_session_open(tny_runtime *, tny_bytes, tny_session **, tny_error **);
tny_bytes tny_session_id(const tny_session *);
int32_t tny_session_send(tny_session *, tny_bytes, tny_error **);
int32_t tny_session_next_event(tny_session *, uint32_t, tny_event **, tny_error **);
int32_t tny_session_steer(tny_session *, tny_bytes, tny_error **);
int32_t tny_session_respond_permission(tny_session *, tny_bytes, uint32_t, tny_error **);
int32_t tny_session_cancel(tny_session *, tny_error **);
void tny_session_free(tny_session *);
int32_t tny_session_destroy(tny_session **);
int32_t tny_event_view_init(tny_event_view_v0 *, uint64_t);
int32_t tny_event_read(const tny_event *, tny_event_view_v0 *, uint64_t);
void tny_event_free(tny_event *);
int32_t tny_error_code(const tny_error *);
tny_bytes tny_error_message(const tny_error *);
void tny_error_free(tny_error *);
"""


@dataclass(frozen=True, slots=True)
class Capabilities:
    schema_version: int
    abi_version: int
    provider_selected: int
    provider_initialized: bool
    endpoint_reachability: int
    threading_model: int
    cancel_model: int
    provider_available_mask: int
    feature_available_mask: int
    feature_enabled_mask: int
    event_queue_max: int
    event_reserved: int
    event_payload_bytes_max: int
    event_reserved_bytes: int
    library_version: bytes
    platform_family: bytes
    architecture: bytes
    transport: bytes
    tls_implementation: bytes
    linkage: bytes
    custom_tool_max_count: int = 0
    custom_tool_name_max: int = 0
    custom_tool_schema_max: int = 0
    custom_tool_arguments_max: int = 0
    custom_tool_result_max: int = 0

    @property
    def owner_thread_affine(self) -> bool:
        return self.threading_model == 1

    @property
    def cross_thread_native_cancel(self) -> bool:
        return bool(self.feature_enabled_mask & FEATURE_CROSS_THREAD_CANCEL)

    @property
    def custom_tool_callbacks(self) -> bool:
        return bool(self.feature_enabled_mask & FEATURE_CUSTOM_TOOLS)

    @property
    def host_service_callbacks(self) -> bool:
        return bool(self.feature_enabled_mask & FEATURE_HOST_SERVICES)

    @property
    def provider(self) -> str:
        return {0: "none", 1: "openai", 2: "cursor", 3: "codex", 4: "acp"}.get(
            self.provider_selected, "unknown"
        )

    @property
    def mcp(self) -> bool:
        return bool(self.feature_enabled_mask & FEATURE_MCP)

    @property
    def windows(self) -> bool:
        return bool(self.feature_available_mask & FEATURE_WINDOWS)


def borrowed(ffi: FFI, value: str | bytes | os.PathLike[str]) -> tuple[Any, Any]:
    raw = value if isinstance(value, bytes) else os.fspath(value).encode("utf-8")
    buffer = ffi.new("char[]", raw)
    view = ffi.new("tny_bytes *")
    view.ptr = buffer
    view.len = len(raw)
    return buffer, view


def copy_bytes(ffi: FFI, value: Any) -> bytes:
    if value.ptr == ffi.NULL or value.len == 0:
        return b""
    return bytes(ffi.buffer(value.ptr, value.len))


def _packaged_candidates() -> list[Path]:
    names = ("libtny.1.dylib",) if platform.system() == "Darwin" else ("libtny.so.1",)
    try:
        root = resources.files("tny").joinpath(".libs")
        return [Path(str(root.joinpath(name))) for name in names]
    except (FileNotFoundError, TypeError):
        return []


def discover_library(explicit: str | os.PathLike[str] | None = None) -> Path | str:
    """Resolve libtny without searching the current working directory."""
    if explicit is not None:
        path = Path(explicit).expanduser().resolve(strict=True)
        if not path.is_file():
            raise ConfigurationError(-5)
        return path
    env_path = os.environ.get("TNY_LIBRARY_PATH")
    if env_path:
        path = Path(env_path).expanduser().resolve(strict=True)
        if not path.is_file():
            raise ConfigurationError(-5)
        return path
    for path in _packaged_candidates():
        if path.is_file():
            return path.resolve()
    system = ctypes.util.find_library("tny")
    if system:
        return system
    raise ConfigurationError(-5)


class Library:
    """A validated stable libtny ABI 1 library loaded with cffi."""

    def __init__(self, path: str | os.PathLike[str] | None = None) -> None:
        resolved = discover_library(path)
        ffi = FFI()
        ffi.cdef(CDEF)
        try:
            native = ffi.dlopen(os.fspath(resolved))
        except OSError:
            raise ConfigurationError(-5) from None
        self.path = resolved
        self.ffi = ffi
        self.native = native
        version = int(native.tny_abi_version())
        self.abi_major, self.abi_minor = version >> 16, version & 0xFFFF
        too_new = (
            SUPPORTED_ABI_MINOR_MAX is not None
            and self.abi_minor > SUPPORTED_ABI_MINOR_MAX
        )
        if (
            self.abi_major != SUPPORTED_ABI_MAJOR
            or self.abi_minor < SUPPORTED_ABI_MINOR_MIN
            or too_new
        ):
            raise UnsupportedError(-9)
        self.version = copy_bytes(ffi, native.tny_library_version())
        self.capabilities: Capabilities | None = None

    def __repr__(self) -> str:
        return f"Library(abi={self.abi_major}.{self.abi_minor})"

    def raise_status(self, status: int, error: Any) -> None:
        message = b""
        code = int(status)
        if error != self.ffi.NULL:
            try:
                code = int(self.native.tny_error_code(error))
                message = copy_bytes(self.ffi, self.native.tny_error_message(error))
            finally:
                self.native.tny_error_free(error)
        raise error_from_status(code, message)

    def read_capabilities(
        self, runtime: Any, *, extended: bool = False
    ) -> Capabilities:
        """Copy the complete borrowed capability snapshot from a live runtime."""
        view = self.ffi.new("tny_capabilities_v0 *")
        size = self.ffi.sizeof("tny_capabilities_v0")
        status = int(self.native.tny_capabilities_init(view, size))
        if status != STATUS_OK:
            self.raise_status(status, self.ffi.NULL)
        status = int(self.native.tny_runtime_get_capabilities(runtime, view, size))
        if status != STATUS_OK:
            self.raise_status(status, self.ffi.NULL)
        limits = (0, 0, 0, 0, 0)
        if extended:
            extended_view = self.ffi.new("tny_capabilities_v1 *")
            extended_size = self.ffi.sizeof("tny_capabilities_v1")
            extended_status = int(
                self.native.tny_capabilities_v1_init(extended_view, extended_size)
            )
            if extended_status != STATUS_OK:
                self.raise_status(extended_status, self.ffi.NULL)
            extended_status = int(
                self.native.tny_runtime_get_capabilities_v1(
                    runtime, extended_view, extended_size
                )
            )
            if extended_status != STATUS_OK:
                self.raise_status(extended_status, self.ffi.NULL)
            view = self.ffi.addressof(extended_view[0], "base")
            limits = (
                int(extended_view.custom_tool_max_count),
                int(extended_view.custom_tool_name_max),
                int(extended_view.custom_tool_schema_max),
                int(extended_view.custom_tool_arguments_max),
                int(extended_view.custom_tool_result_max),
            )
        snapshot = Capabilities(
            schema_version=int(view.schema_version),
            abi_version=int(view.abi_version),
            provider_selected=int(view.provider_selected),
            provider_initialized=bool(view.provider_initialized),
            endpoint_reachability=int(view.endpoint_reachability),
            threading_model=int(view.threading_model),
            cancel_model=int(view.cancel_model),
            provider_available_mask=int(view.provider_available_mask),
            feature_available_mask=int(view.feature_available_mask),
            feature_enabled_mask=int(view.feature_enabled_mask),
            event_queue_max=int(view.event_queue_max),
            event_reserved=int(view.event_reserved),
            event_payload_bytes_max=int(view.event_payload_bytes_max),
            event_reserved_bytes=int(view.event_reserved_bytes),
            library_version=copy_bytes(self.ffi, view.library_version),
            platform_family=copy_bytes(self.ffi, view.platform_family),
            architecture=copy_bytes(self.ffi, view.architecture),
            transport=copy_bytes(self.ffi, view.transport),
            tls_implementation=copy_bytes(self.ffi, view.tls_implementation),
            linkage=copy_bytes(self.ffi, view.linkage),
            custom_tool_max_count=limits[0],
            custom_tool_name_max=limits[1],
            custom_tool_schema_max=limits[2],
            custom_tool_arguments_max=limits[3],
            custom_tool_result_max=limits[4],
        )
        self.capabilities = snapshot
        return snapshot
