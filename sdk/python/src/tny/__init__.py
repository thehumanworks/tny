"""Python SDK for the stable libtny ABI 1 runtime."""

from importlib.metadata import PackageNotFoundError
from importlib.metadata import version as _distribution_version

from ._binding import (
    Capabilities as Capabilities,
)
from ._binding import (
    Library as Library,
)
from ._binding import (
    discover_library as discover_library,
)
from .aio import AsyncRuntime as AsyncRuntime
from .aio import AsyncSession as AsyncSession
from .aio import AsyncToolRegistration as AsyncToolRegistration
from .callbacks import (
    AsyncCustomTool as AsyncCustomTool,
)
from .callbacks import (
    CustomTool as CustomTool,
)
from .callbacks import (
    HostServices as HostServices,
)
from .callbacks import (
    ToolRegistration as ToolRegistration,
)
from .callbacks import (
    ToolResult as ToolResult,
)
from .callbacks import (
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
from .workflow import EventHandler as EventHandler
from .workflow import PermissionHandler as PermissionHandler
from .workflow import Workflow as Workflow
from .workflow import WorkflowContextError as WorkflowContextError
from .workflow import WorkflowDefinitionError as WorkflowDefinitionError
from .workflow import WorkflowError as WorkflowError
from .workflow import WorkflowResult as WorkflowResult
from .workflow import WorkflowRunError as WorkflowRunError
from .workflow import WorkflowTask as WorkflowTask
from .workflow import WorkflowTaskExecution as WorkflowTaskExecution
from .workflow import WorkflowTaskResult as WorkflowTaskResult
from .workflow import WorkflowTaskRunner as WorkflowTaskRunner
from .workflow import WorkflowTaskStatus as WorkflowTaskStatus

try:
    __version__ = _distribution_version("tny")
except PackageNotFoundError:
    # Source-tree imports have no installed distribution metadata. This value
    # is deliberately non-publishable; release wheels always use tag metadata.
    __version__ = "0.0.0.dev0"

__all__ = (
    "AsyncCustomTool",
    "AsyncRuntime",
    "AsyncSession",
    "AsyncToolRegistration",
    "AuthenticationError",
    "BackpressureError",
    "BadStateError",
    "BusyError",
    "CancellationToken",
    "CancelledError",
    "Capabilities",
    "ConfigurationError",
    "CustomTool",
    "EventHandler",
    "HostServices",
    "InternalError",
    "InvalidArgumentError",
    "Library",
    "OutOfMemoryError",
    "PermissionDecision",
    "PermissionHandler",
    "PermissionMode",
    "ProtocolError",
    "Runtime",
    "RuntimeConfig",
    "Session",
    "TnyError",
    "TnyIOError",
    "TnyTimeoutError",
    "ToolRegistration",
    "ToolResult",
    "ToolSensitivity",
    "UnsupportedError",
    "Workflow",
    "WorkflowContextError",
    "WorkflowDefinitionError",
    "WorkflowError",
    "WorkflowResult",
    "WorkflowRunError",
    "WorkflowTask",
    "WorkflowTaskExecution",
    "WorkflowTaskResult",
    "WorkflowTaskRunner",
    "WorkflowTaskStatus",
    "build_conformance_report",
    "discover_library",
    "write_conformance_report",
) + _event_all
