"""Offline authoring helpers for the existing Python extension API.

The catalog inspects only bundled API types. Checking explicitly selected
extensions executes their imports and setup as trusted, unsandboxed code.
Neither helper starts inference or invokes registered event handlers itself.
"""

import inspect
from dataclasses import asdict, dataclass, fields
from typing import Any, Dict, Iterable, Optional, Tuple, get_args

from .actions import Action
from .capabilities import CapabilityView
from .events import HookEvent, UnknownEvent, event_from_dict, event_types
from .loader import load_extension


@dataclass(frozen=True)
class CheckDiagnostic:
    severity: str
    code: str
    entry: str
    message: str
    event: str = ""
    handler_index: Optional[int] = None


@dataclass(frozen=True)
class ExtensionSummary:
    name: str
    entry: str
    events: Tuple[str, ...]
    handlers: int


@dataclass(frozen=True)
class CheckReport:
    extensions: Tuple[ExtensionSummary, ...]
    diagnostics: Tuple[CheckDiagnostic, ...]

    @property
    def ok(self) -> bool:
        return not any(item.severity == "error" for item in self.diagnostics)

    def to_dict(self) -> Dict[str, Any]:
        return {"schema_version": 1, "ok": self.ok, **asdict(self)}


def check_extensions(
    entries: Iterable[str],
    *,
    cwd: Optional[str] = None,
    capabilities: Optional[CapabilityView] = None,
) -> CheckReport:
    """Load explicit trusted entries and diagnose setup and registrations.

    Uses the runtime's loader, including package-relative imports, asynchronous
    setup, and stdout isolation. The caller chooses every entry; no directory
    discovery, installation, provider probing, or handler invocation occurs.
    An omitted capability view is empty, not a claim of provider support.
    """

    summaries = []
    diagnostics = []
    for ordinal, entry in enumerate(entries):
        try:
            extension = load_extension(entry, ordinal, cwd, capabilities)
        except KeyboardInterrupt:
            raise
        except BaseException as error:
            diagnostics.append(
                CheckDiagnostic(
                    "error", "load_error", entry, str(error) or type(error).__name__
                )
            )
            continue
        summaries.append(
            ExtensionSummary(
                extension.name,
                extension.entry,
                tuple(item.event for item in extension.registrations),
                len(extension.registrations),
            )
        )
        if not extension.registrations:
            diagnostics.append(
                CheckDiagnostic(
                    "warning",
                    "no_handlers",
                    extension.entry,
                    "setup registered no handlers; check capability-dependent branches",
                )
            )
        for registration in extension.registrations:
            event = registration.event
            if event != "*" and isinstance(
                event_from_dict({"type": event}), UnknownEvent
            ):
                diagnostics.append(
                    CheckDiagnostic(
                        "warning",
                        "unknown_event",
                        extension.entry,
                        "event is not in this API's catalog; check spelling or a future event",
                        event,
                        registration.index,
                    )
                )
            try:
                signature = inspect.signature(registration.handler)
            except (TypeError, ValueError):
                diagnostics.append(
                    CheckDiagnostic(
                        "warning",
                        "unknown_signature",
                        extension.entry,
                        "callable signature is unavailable; verify it accepts one event",
                        event,
                        registration.index,
                    )
                )
                continue
            try:
                signature.bind(object())
            except TypeError as error:
                diagnostics.append(
                    CheckDiagnostic(
                        "error",
                        "invalid_handler_signature",
                        extension.entry,
                        "handler must accept one positional event: %s" % error,
                        event,
                        registration.index,
                    )
                )
    return CheckReport(tuple(summaries), tuple(diagnostics))


def _field_catalog(cls: type) -> list:
    return [
        {
            "name": item.name,
            "type": (
                item.type.__name__
                if isinstance(item.type, type)
                else str(item.type).replace("typing.", "")
            ),
        }
        for item in fields(cls)
    ]


def extension_catalog() -> Dict[str, Any]:
    """Describe bundled event/action dataclasses without loading extensions.

    Field lists describe Python values, not provider availability or JSON
    validation rules. The runtime capability view remains authoritative.
    """

    envelope = _field_catalog(HookEvent)
    envelope_names = {item["name"] for item in envelope}
    return {
        "schema_version": 1,
        "envelope": envelope,
        "events": [
            {
                "name": cls.EVENT_NAME,
                "python_type": cls.__name__,
                "fields": [
                    item
                    for item in _field_catalog(cls)
                    if item["name"] not in envelope_names
                ],
            }
            for cls in event_types()
        ],
        "actions": [
            {
                "kind": cls.kind,
                "python_type": cls.__name__,
                "fields": _field_catalog(cls),
            }
            for cls in sorted(get_args(Action), key=lambda cls: cls.kind)
        ],
    }


__all__ = [
    "CheckDiagnostic",
    "CheckReport",
    "ExtensionSummary",
    "check_extensions",
    "extension_catalog",
]
