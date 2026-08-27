"""Immutable, Python-owned representations of the public event schema."""
from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from enum import IntEnum, IntFlag


class PermissionOptions(IntFlag):
    ALLOW = 1
    ALLOW_ALWAYS = 2
    DENY = 4


class StopReason(IntEnum):
    DONE = 0
    INTERRUPTED = 1
    DENIED = 2
    STEP_LIMIT = 3
    ERROR = 4


@dataclass(frozen=True, slots=True, repr=False)
class Event:
    kind: int
    schema_version: int
    sequence: int
    timestamp_ms: int
    provider: bytes
    session_id: bytes
    turn_id: bytes
    type: str

    def __repr__(self) -> str:
        return f"{type(self).__name__}(kind={self.kind}, sequence={self.sequence})"


@dataclass(frozen=True, slots=True, repr=False)
class TextDeltaEvent(Event):
    text: bytes
    message_id: bytes


@dataclass(frozen=True, slots=True, repr=False)
class ThinkingEvent(Event):
    text: bytes
    message_id: bytes


@dataclass(frozen=True, slots=True, repr=False)
class ToolStartEvent(Event):
    tool_name: bytes
    tool_id: bytes
    tool_detail: bytes


@dataclass(frozen=True, slots=True, repr=False)
class ToolEndEvent(Event):
    tool_name: bytes
    tool_id: bytes
    tool_detail: bytes
    ok: bool


@dataclass(frozen=True, slots=True, repr=False)
class PermissionRequestEvent(Event):
    permission_id: bytes
    summary: bytes
    options: PermissionOptions


@dataclass(frozen=True, slots=True, repr=False)
class PlanEvent(Event):
    text: bytes
    message_id: bytes


@dataclass(frozen=True, slots=True, repr=False)
class UsageEvent(Event):
    input_tokens: int
    output_tokens: int
    context_used: int
    context_size: int
    cost: float | None


@dataclass(frozen=True, slots=True, repr=False)
class TurnEndEvent(Event):
    stop_reason: int


@dataclass(frozen=True, slots=True, repr=False)
class ErrorEvent(Event):
    text: bytes
    error_code: int


@dataclass(frozen=True, slots=True, repr=False)
class StatusEvent(Event):
    text: bytes
    message_id: bytes


@dataclass(frozen=True, slots=True, repr=False)
class SteerRejectedEvent(Event):
    text: bytes
    message_id: bytes


@dataclass(frozen=True, slots=True, repr=False)
class CustomMessageEvent(Event):
    text: bytes
    message_id: bytes
    message_type: bytes


@dataclass(frozen=True, slots=True, repr=False)
class UserMessageEvent(Event):
    text: bytes
    message_id: bytes


@dataclass(frozen=True, slots=True, repr=False)
class ToolProgressEvent(Event):
    tool_name: bytes
    tool_id: bytes
    tool_detail: bytes


@dataclass(frozen=True, slots=True, repr=False)
class UnknownEvent(Event):
    """Forward-compatible envelope for an event kind unknown to this SDK."""

    payload: Mapping[str, object]


EVENT_TYPES_BY_KIND = {
    0: ("text_delta", TextDeltaEvent),
    1: ("thinking", ThinkingEvent),
    2: ("tool_start", ToolStartEvent),
    3: ("tool_end", ToolEndEvent),
    4: ("permission_request", PermissionRequestEvent),
    5: ("plan", PlanEvent),
    6: ("usage", UsageEvent),
    7: ("turn_end", TurnEndEvent),
    8: ("error", ErrorEvent),
    9: ("status", StatusEvent),
    10: ("steer_rejected", SteerRejectedEvent),
    11: ("custom_message", CustomMessageEvent),
    12: ("user_message", UserMessageEvent),
    13: ("tool_progress", ToolProgressEvent),
}


def decode_unknown_event_fixture(
    *,
    kind: int,
    schema_version: int,
    sequence: int,
    timestamp_ms: int,
    provider: bytes,
    session_id: bytes,
    turn_id: bytes,
    payload: Mapping[str, object],
) -> UnknownEvent:
    """Exercise the SDK's forward-event decoder without a native injector.

    The shared ABI has no unknown-event injection symbol. This fixture seam is
    intentionally strict: known numeric kinds are rejected, byte fields are
    copied, and the future payload is retained under an immutable mapping.
    """
    if kind in EVENT_TYPES_BY_KIND:
        raise ValueError("fixture kind is already known")
    copied_payload = {
        str(key): bytes(value) if isinstance(value, bytes) else value
        for key, value in payload.items()
    }
    from types import MappingProxyType

    return UnknownEvent(
        kind=int(kind),
        schema_version=int(schema_version),
        sequence=int(sequence),
        timestamp_ms=int(timestamp_ms),
        provider=bytes(provider),
        session_id=bytes(session_id),
        turn_id=bytes(turn_id),
        type="unknown",
        payload=MappingProxyType(copied_payload),
    )


KnownEvent = (
    TextDeltaEvent | ThinkingEvent | ToolStartEvent | ToolEndEvent |
    PermissionRequestEvent | PlanEvent | UsageEvent | TurnEndEvent |
    ErrorEvent | StatusEvent | SteerRejectedEvent | CustomMessageEvent |
    UserMessageEvent | ToolProgressEvent
)
AnyEvent = KnownEvent | UnknownEvent


class EventStreamError(RuntimeError):
    """Optional exception representation of a post-start ERROR event."""

    def __init__(self, event: ErrorEvent) -> None:
        self.event = event
        super().__init__(event.error_code)

    def __str__(self) -> str:
        return f"tny turn failed asynchronously (code {self.event.error_code})"

    def __repr__(self) -> str:
        return f"EventStreamError(code={self.event.error_code})"


__all__ = (
    "AnyEvent", "CustomMessageEvent", "ErrorEvent", "Event",
    "EVENT_TYPES_BY_KIND",
    "decode_unknown_event_fixture",
    "EventStreamError", "KnownEvent", "PermissionOptions",
    "PermissionRequestEvent", "PlanEvent", "StatusEvent", "SteerRejectedEvent",
    "StopReason", "TextDeltaEvent", "ThinkingEvent", "ToolEndEvent",
    "ToolProgressEvent", "ToolStartEvent", "TurnEndEvent", "UnknownEvent",
    "UsageEvent", "UserMessageEvent",
)
