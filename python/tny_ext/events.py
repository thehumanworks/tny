"""Typed events exposed to tny Python extensions.

The wire format is deliberately a plain mapping.  Known event names are
upgraded to dataclasses for editor/type-checker help; unknown names retain the
complete payload in :class:`UnknownEvent` so a newer tny can talk to an older
extension host without breaking the process.
"""

from dataclasses import dataclass, field
from typing import Any, ClassVar, Dict, Mapping, Tuple, Type


def _text(value: Any, default: str = "") -> str:
    return value if isinstance(value, str) else default


def _boolean(value: Any, default: bool = False) -> bool:
    return value if isinstance(value, bool) else default


def _integer(value: Any, default: int = 0) -> int:
    return value if isinstance(value, int) and not isinstance(value, bool) else default


def _mapping(value: Any) -> Mapping[str, Any]:
    return dict(value) if isinstance(value, Mapping) else {}


def _mapping_tuple(value: Any) -> Tuple[Mapping[str, Any], ...]:
    if not isinstance(value, (list, tuple)):
        return ()
    return tuple(dict(item) for item in value if isinstance(item, Mapping))


def _text_tuple(value: Any) -> Tuple[str, ...]:
    if not isinstance(value, (list, tuple)):
        return ()
    return tuple(item for item in value if isinstance(item, str))


def _stop_info(value: Any) -> "StopInfo":
    if isinstance(value, Mapping):
        details = dict(value)
        return StopInfo(reason=_text(details.get("reason", details.get("stop"))), details=details)
    return StopInfo(reason=_text(value))


@dataclass(frozen=True)
class HookEvent:
    """Versioned normalized event envelope shared by every hook."""

    EVENT_NAME: ClassVar[str] = ""
    schema_version: int
    event_id: str
    type: str
    sequence: int
    provider: str
    session_id: str
    turn_id: str
    timestamp_ms: int
    payload: Mapping[str, Any] = field(repr=False)


@dataclass(frozen=True)
class UnknownEvent(HookEvent):
    """Forward-compatible representation of an event unknown to this host."""


@dataclass(frozen=True)
class TextEvent(HookEvent):
    text: str
    message_id: str = ""


@dataclass(frozen=True)
class TextDeltaEvent(TextEvent):
    EVENT_NAME: ClassVar[str] = "text_delta"


@dataclass(frozen=True)
class ThinkingEvent(TextEvent):
    EVENT_NAME: ClassVar[str] = "thinking"


@dataclass(frozen=True)
class PlanEvent(TextEvent):
    EVENT_NAME: ClassVar[str] = "plan"


@dataclass(frozen=True)
class StatusEvent(TextEvent):
    EVENT_NAME: ClassVar[str] = "status"


@dataclass(frozen=True)
class SteerRejectedEvent(TextEvent):
    EVENT_NAME: ClassVar[str] = "steer_rejected"


@dataclass(frozen=True)
class ErrorEvent(TextEvent):
    EVENT_NAME: ClassVar[str] = "error"
    error_code: str = ""


@dataclass(frozen=True)
class CustomMessageEvent(TextEvent):
    EVENT_NAME: ClassVar[str] = "custom_message"
    custom_type: str = ""


@dataclass(frozen=True)
class UserMessageEvent(TextEvent):
    EVENT_NAME: ClassVar[str] = "user_message"


@dataclass(frozen=True)
class ToolStartEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "tool_start"
    tool_name: str
    tool_id: str
    detail: str


@dataclass(frozen=True)
class ToolEndEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "tool_end"
    tool_name: str
    tool_id: str
    detail: str
    ok: bool


@dataclass(frozen=True)
class ToolProgressEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "tool_progress"
    tool_name: str
    tool_id: str
    detail: str


@dataclass(frozen=True)
class PermissionRequestEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "permission_request"
    permission_id: str
    summary: str
    options: int


@dataclass(frozen=True)
class UsageEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "usage"
    input_tokens: int
    output_tokens: int
    context_used: int = 0
    context_size: int = 0
    cost: float = 0.0
    has_cost: bool = False


@dataclass(frozen=True)
class StopInfo:
    """Portable terminal reason with the original normalized value retained."""

    reason: str
    details: Mapping[str, Any] = field(default_factory=dict, repr=False)


@dataclass(frozen=True)
class TurnEndEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "turn_end"
    stop: StopInfo


@dataclass(frozen=True)
class BeforeAgentStartEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "before_agent_start"
    prompt: str
    system_prompt: str
    images: Tuple[Mapping[str, Any], ...]
    system_prompt_options: Mapping[str, Any]


@dataclass(frozen=True)
class AgentStartEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "agent_start"


@dataclass(frozen=True)
class AgentEndEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "agent_end"
    messages: Tuple[Mapping[str, Any], ...]
    stop: StopInfo
    continuation_count: int
    max_continuations: int
    output_text: str


@dataclass(frozen=True)
class AgentSettledEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "agent_settled"


@dataclass(frozen=True)
class SessionStartEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "session_start"
    reason: str
    previous_session_id: str


@dataclass(frozen=True)
class SessionEndEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "session_end"
    reason: str


@dataclass(frozen=True)
class UserPromptSubmitEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "user_prompt_submit"
    prompt: str
    source: str
    submission_id: str
    images: Tuple[Mapping[str, Any], ...]


@dataclass(frozen=True)
class TurnStartEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "turn_start"
    iteration: int
    source: str


@dataclass(frozen=True)
class MessageStartEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "message_start"
    message_id: str
    role: str
    content_type: str


@dataclass(frozen=True)
class MessageUpdateEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "message_update"
    message_id: str
    role: str
    content_type: str
    text: str


@dataclass(frozen=True)
class MessageEndEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "message_end"
    message_id: str
    role: str
    content_type: str
    text: str


@dataclass(frozen=True)
class PreCompactEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "pre_compact"
    trigger: str
    message_count: int


@dataclass(frozen=True)
class PostCompactEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "post_compact"
    trigger: str
    before_count: int
    after_count: int
    summary: str


@dataclass(frozen=True)
class CompactFailedEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "compact_failed"
    trigger: str
    error: str


@dataclass(frozen=True)
class ModelChangeEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "model_change"
    previous: str
    current: str
    source: str


@dataclass(frozen=True)
class EffortChangeEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "effort_change"
    previous: str
    current: str
    source: str


@dataclass(frozen=True)
class InstructionsChangeEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "instructions_change"
    paths: Tuple[str, ...]
    digest: str
    count: int


@dataclass(frozen=True)
class WorkspaceChangeEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "workspace_change"
    action: str
    path: str
    directories: Tuple[str, ...]


@dataclass(frozen=True)
class SubagentStartEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "subagent_start"
    subagent_id: str
    action: str


@dataclass(frozen=True)
class SubagentEndEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "subagent_end"
    subagent_id: str
    action: str
    outcome: str
    ok: bool


@dataclass(frozen=True)
class PreToolUseEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "pre_tool_use"
    tool_name: str
    tool_id: str
    arguments: Mapping[str, Any]
    original_arguments: Mapping[str, Any]
    provider_owned: bool


@dataclass(frozen=True)
class PostToolUseEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "post_tool_use"
    tool_name: str
    tool_id: str
    result: str
    original_ok: bool


@dataclass(frozen=True)
class PostToolFailureEvent(PostToolUseEvent):
    EVENT_NAME: ClassVar[str] = "post_tool_failure"


@dataclass(frozen=True)
class PostToolBatchEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "post_tool_batch"
    tool_ids: Tuple[str, ...]
    failed: int


@dataclass(frozen=True)
class ProviderRequestEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "provider_request"
    method: str
    endpoint: str
    metadata: Mapping[str, Any]


@dataclass(frozen=True)
class ProviderResponseEvent(HookEvent):
    EVENT_NAME: ClassVar[str] = "provider_response"
    status: int
    metadata: Mapping[str, Any]


_EVENT_TYPES: Dict[str, Type[HookEvent]] = {
    cls.EVENT_NAME: cls
    for cls in (
        TextDeltaEvent,
        ThinkingEvent,
        ToolStartEvent,
        ToolEndEvent,
        ToolProgressEvent,
        PermissionRequestEvent,
        PlanEvent,
        UsageEvent,
        TurnEndEvent,
        ErrorEvent,
        StatusEvent,
        SteerRejectedEvent,
        CustomMessageEvent,
        UserMessageEvent,
        BeforeAgentStartEvent,
        AgentStartEvent,
        AgentEndEvent,
        AgentSettledEvent,
        SessionStartEvent,
        SessionEndEvent,
        UserPromptSubmitEvent,
        TurnStartEvent,
        MessageStartEvent,
        MessageUpdateEvent,
        MessageEndEvent,
        PreCompactEvent,
        PostCompactEvent,
        CompactFailedEvent,
        ModelChangeEvent,
        EffortChangeEvent,
        InstructionsChangeEvent,
        WorkspaceChangeEvent,
        SubagentStartEvent,
        SubagentEndEvent,
        PreToolUseEvent,
        PostToolUseEvent,
        PostToolFailureEvent,
        PostToolBatchEvent,
        ProviderRequestEvent,
        ProviderResponseEvent,
    )
}
# Accept the internal C spelling while presenting the documented public name.
_EVENT_TYPES["permission"] = PermissionRequestEvent


def event_name(event: Any) -> str:
    """Return a valid event name from a string or HookEvent subclass."""

    if isinstance(event, str) and event:
        return event
    if isinstance(event, type) and issubclass(event, HookEvent) and event.EVENT_NAME:
        return event.EVENT_NAME
    raise TypeError("event must be a non-empty name or HookEvent subclass")


def event_from_dict(value: Mapping[str, Any]) -> HookEvent:
    """Create a typed event while preserving all input fields in ``payload``."""

    if not isinstance(value, Mapping):
        raise TypeError("event must be an object")
    event_type = value.get("type", value.get("kind"))
    if not isinstance(event_type, str) or not event_type:
        raise ValueError("event.type must be a non-empty string")
    nested_payload = value.get("payload")
    if isinstance(nested_payload, Mapping):
        payload = dict(nested_payload)
    else:
        envelope_fields = {
            "schema_version",
            "event_id",
            "type",
            "kind",
            "sequence",
            "provider",
            "session_id",
            "turn_id",
            "timestamp_ms",
        }
        payload = {key: item for key, item in value.items() if key not in envelope_fields}
    cls = _EVENT_TYPES.get(event_type)
    common = {
        "schema_version": _integer(value.get("schema_version"), 1),
        "event_id": _text(value.get("event_id")),
        "type": event_type,
        "sequence": _integer(value.get("sequence")),
        "provider": _text(value.get("provider")),
        "session_id": _text(value.get("session_id")),
        "turn_id": _text(value.get("turn_id")),
        "timestamp_ms": _integer(value.get("timestamp_ms")),
        "payload": payload,
    }
    if cls is None:
        return UnknownEvent(**common)
    if issubclass(cls, TextEvent):
        if cls is ErrorEvent:
            return ErrorEvent(
                error_code=_text(payload.get("error_code")),
                text=_text(payload.get("text", payload.get("message"))),
                message_id=_text(payload.get("message_id")),
                **common
            )
        if cls is CustomMessageEvent:
            return CustomMessageEvent(
                text=_text(payload.get("text")),
                message_id=_text(payload.get("message_id")),
                custom_type=_text(payload.get("custom_type")),
                **common
            )
        return cls(
            text=_text(payload.get("text")),
            message_id=_text(payload.get("message_id")),
            **common
        )  # type: ignore[call-arg]
    if cls is ToolStartEvent:
        return ToolStartEvent(
            tool_name=_text(payload.get("tool_name")),
            tool_id=_text(payload.get("tool_id")),
            detail=_text(payload.get("detail", payload.get("tool_detail"))),
            **common
        )
    if cls is ToolEndEvent:
        return ToolEndEvent(
            tool_name=_text(payload.get("tool_name")),
            tool_id=_text(payload.get("tool_id")),
            detail=_text(payload.get("detail", payload.get("tool_detail"))),
            ok=_boolean(payload.get("ok", payload.get("tool_ok"))),
            **common
        )
    if cls is ToolProgressEvent:
        return ToolProgressEvent(
            tool_name=_text(payload.get("tool_name")),
            tool_id=_text(payload.get("tool_id")),
            detail=_text(payload.get("detail", payload.get("tool_detail"))),
            **common
        )
    if cls is PermissionRequestEvent:
        return PermissionRequestEvent(
            permission_id=_text(payload.get("permission_id", payload.get("perm_id"))),
            summary=_text(payload.get("summary", payload.get("perm_summary"))),
            options=_integer(payload.get("options", payload.get("perm_options"))),
            **common
        )
    if cls is UsageEvent:
        cost = payload.get("cost")
        return UsageEvent(
            input_tokens=_integer(payload.get("input_tokens", payload.get("in_tokens"))),
            output_tokens=_integer(payload.get("output_tokens", payload.get("out_tokens"))),
            context_used=_integer(payload.get("context_used")),
            context_size=_integer(payload.get("context_size")),
            cost=float(cost) if isinstance(cost, (int, float)) else 0.0,
            has_cost=isinstance(cost, (int, float)),
            **common
        )
    if cls is TurnEndEvent:
        return TurnEndEvent(stop=_stop_info(payload.get("stop")), **common)
    if cls is BeforeAgentStartEvent:
        return BeforeAgentStartEvent(
            prompt=_text(payload.get("prompt")),
            system_prompt=_text(payload.get("system_prompt")),
            images=_mapping_tuple(payload.get("images")),
            system_prompt_options=_mapping(payload.get("system_prompt_options")),
            **common
        )
    if cls is AgentEndEvent:
        return AgentEndEvent(
            messages=_mapping_tuple(payload.get("messages")),
            stop=_stop_info(payload.get("stop")),
            continuation_count=_integer(payload.get("continuation_count")),
            max_continuations=_integer(payload.get("max_continuations")),
            output_text=_text(payload.get("output_text")),
            **common
        )
    if cls is SessionStartEvent:
        return SessionStartEvent(
            reason=_text(payload.get("reason")),
            previous_session_id=_text(payload.get("previous_session_id")),
            **common
        )
    if cls is SessionEndEvent:
        return SessionEndEvent(
            reason=_text(payload.get("reason")),
            **common
        )
    if cls is UserPromptSubmitEvent:
        return UserPromptSubmitEvent(
            prompt=_text(payload.get("prompt")),
            source=_text(payload.get("source")),
            submission_id=_text(payload.get("submission_id")),
            images=_mapping_tuple(payload.get("images")),
            **common
        )
    if cls is TurnStartEvent:
        return TurnStartEvent(
            iteration=_integer(payload.get("iteration")),
            source=_text(payload.get("source")),
            **common
        )
    if cls in (MessageStartEvent, MessageUpdateEvent, MessageEndEvent):
        fields = {
            "message_id": _text(payload.get("message_id")),
            "role": _text(payload.get("role")),
            "content_type": _text(payload.get("content_type")),
        }
        if cls is MessageStartEvent:
            return MessageStartEvent(**fields, **common)
        fields["text"] = _text(payload.get("text"))
        return cls(**fields, **common)  # type: ignore[call-arg]
    if cls is PreCompactEvent:
        return PreCompactEvent(
            trigger=_text(payload.get("trigger")),
            message_count=_integer(payload.get("message_count")),
            **common
        )
    if cls is PostCompactEvent:
        return PostCompactEvent(
            trigger=_text(payload.get("trigger")),
            before_count=_integer(payload.get("before_count")),
            after_count=_integer(payload.get("after_count")),
            summary=_text(payload.get("summary")),
            **common
        )
    if cls is CompactFailedEvent:
        return CompactFailedEvent(
            trigger=_text(payload.get("trigger")),
            error=_text(payload.get("error")),
            **common
        )
    if cls in (ModelChangeEvent, EffortChangeEvent):
        return cls(
            previous=_text(payload.get("previous")),
            current=_text(payload.get("current")),
            source=_text(payload.get("source")),
            **common
        )  # type: ignore[call-arg]
    if cls is InstructionsChangeEvent:
        return InstructionsChangeEvent(
            paths=_text_tuple(payload.get("paths")),
            digest=_text(payload.get("digest")),
            count=_integer(payload.get("count")),
            **common
        )
    if cls is WorkspaceChangeEvent:
        return WorkspaceChangeEvent(
            action=_text(payload.get("action")),
            path=_text(payload.get("path")),
            directories=_text_tuple(payload.get("directories")),
            **common
        )
    if cls is SubagentStartEvent:
        return SubagentStartEvent(
            subagent_id=_text(payload.get("subagent_id")),
            action=_text(payload.get("action")),
            **common
        )
    if cls is SubagentEndEvent:
        return SubagentEndEvent(
            subagent_id=_text(payload.get("subagent_id")),
            action=_text(payload.get("action")),
            outcome=_text(payload.get("outcome")),
            ok=_boolean(payload.get("ok")),
            **common
        )
    if cls is PreToolUseEvent:
        return PreToolUseEvent(
            tool_name=_text(payload.get("tool_name")),
            tool_id=_text(payload.get("tool_id")),
            arguments=_mapping(payload.get("arguments")),
            original_arguments=_mapping(payload.get("original_arguments")),
            provider_owned=_boolean(payload.get("provider_owned")),
            **common
        )
    if cls in (PostToolUseEvent, PostToolFailureEvent):
        return cls(
            tool_name=_text(payload.get("tool_name")),
            tool_id=_text(payload.get("tool_id")),
            result=_text(payload.get("result")),
            original_ok=_boolean(payload.get("original_ok")),
            **common
        )  # type: ignore[call-arg]
    if cls is PostToolBatchEvent:
        return PostToolBatchEvent(
            tool_ids=_text_tuple(payload.get("tool_ids")),
            failed=_integer(payload.get("failed")),
            **common
        )
    if cls is ProviderRequestEvent:
        return ProviderRequestEvent(
            method=_text(payload.get("method")),
            endpoint=_text(payload.get("endpoint")),
            metadata=_mapping(payload.get("metadata")),
            **common
        )
    if cls is ProviderResponseEvent:
        return ProviderResponseEvent(
            status=_integer(payload.get("status")),
            metadata=_mapping(payload.get("metadata")),
            **common
        )
    return cls(**common)  # type: ignore[call-arg]


__all__ = [
    "AgentEndEvent",
    "AgentSettledEvent",
    "AgentStartEvent",
    "BeforeAgentStartEvent",
    "CompactFailedEvent",
    "EffortChangeEvent",
    "ErrorEvent",
    "HookEvent",
    "InstructionsChangeEvent",
    "MessageEndEvent",
    "MessageStartEvent",
    "MessageUpdateEvent",
    "ModelChangeEvent",
    "PermissionRequestEvent",
    "PlanEvent",
    "PostCompactEvent",
    "PostToolBatchEvent",
    "PostToolFailureEvent",
    "PostToolUseEvent",
    "PreCompactEvent",
    "PreToolUseEvent",
    "ProviderRequestEvent",
    "ProviderResponseEvent",
    "SessionEndEvent",
    "SessionStartEvent",
    "StatusEvent",
    "StopInfo",
    "SteerRejectedEvent",
    "TextDeltaEvent",
    "ThinkingEvent",
    "ToolEndEvent",
    "ToolStartEvent",
    "TurnEndEvent",
    "TurnStartEvent",
    "UnknownEvent",
    "UserPromptSubmitEvent",
    "UsageEvent",
    "SubagentEndEvent",
    "SubagentStartEvent",
    "WorkspaceChangeEvent",
    "event_from_dict",
    "event_name",
]
