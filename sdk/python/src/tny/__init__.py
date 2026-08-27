"""Python SDK for the stable libtny ABI 1 runtime."""
from importlib.metadata import PackageNotFoundError, version as _distribution_version

from ._binding import (
    Capabilities as Capabilities,
    Library as Library,
    discover_library as discover_library,
)
from .aio import AsyncRuntime as AsyncRuntime
from .aio import AsyncSession as AsyncSession
from .aio import AsyncToolRegistration as AsyncToolRegistration
from .callbacks import (
    AsyncCustomTool as AsyncCustomTool,
    CustomTool as CustomTool,
    HostServices as HostServices,
    ToolRegistration as ToolRegistration,
    ToolResult as ToolResult,
    ToolSensitivity as ToolSensitivity,
)
from .conformance import build_conformance_report as build_conformance_report
from .conformance import write_conformance_report as write_conformance_report
from .errors import (
    AuthenticationError,
    BackpressureError,
    BadStateError,
    BusyError,
    CancelledError,
    ConfigurationError,
    InternalError,
    InvalidArgumentError,
    OutOfMemoryError,
    ProtocolError,
    TnyError,
    TnyIOError,
    TnyTimeoutError,
    UnsupportedError,
)
from .events import *
from .events import __all__ as _event_all
from .runtime import CancellationToken as CancellationToken
from .runtime import PermissionDecision as PermissionDecision
from .runtime import PermissionMode as PermissionMode
from .runtime import Runtime as Runtime
from .runtime import RuntimeConfig as RuntimeConfig
from .runtime import Session as Session

try:
    __version__ = _distribution_version("tny")
except PackageNotFoundError:
    # Source-tree imports have no installed distribution metadata. This value
    # is deliberately non-publishable; release wheels always use tag metadata.
    __version__ = "0.0.0.dev0"

__all__ = (
    "AsyncCustomTool", "AsyncRuntime", "AsyncSession", "AsyncToolRegistration",
    "AuthenticationError",
    "BackpressureError", "BadStateError", "BusyError", "CancellationToken",
    "Capabilities", "CustomTool", "HostServices",
    "build_conformance_report",
    "CancelledError", "ConfigurationError", "InternalError",
    "InvalidArgumentError", "Library", "OutOfMemoryError",
    "PermissionDecision", "PermissionMode", "ProtocolError", "Runtime",
    "RuntimeConfig", "Session", "TnyError", "TnyIOError", "TnyTimeoutError",
    "ToolRegistration", "ToolResult", "ToolSensitivity", "UnsupportedError",
    "discover_library",
    "write_conformance_report",
) + _event_all
