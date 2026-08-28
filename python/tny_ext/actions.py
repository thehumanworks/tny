"""Declarative results returned by tny extension event handlers."""

from dataclasses import dataclass
from typing import Any, ClassVar, Dict, Mapping, Optional, Union


@dataclass(frozen=True)
class NoneAction:
    kind: ClassVar[str] = "none"


@dataclass(frozen=True)
class ContextAction:
    content: str
    custom_type: str = "tny_extension"
    display: bool = True
    kind: ClassVar[str] = "context"


@dataclass(frozen=True)
class ContinueAction:
    content: str
    message_kind: str = "user"
    custom_type: Optional[str] = None
    display: bool = True
    kind: ClassVar[str] = "continue"

    def __post_init__(self) -> None:
        if self.message_kind not in ("user", "custom"):
            raise ValueError("message_kind must be 'user' or 'custom'")
        if self.message_kind == "custom" and not self.custom_type:
            object.__setattr__(self, "custom_type", "tny_extension")


@dataclass(frozen=True)
class StopAction:
    reason: str
    kind: ClassVar[str] = "stop"


@dataclass(frozen=True)
class PromptTransformAction:
    prompt: str
    kind: ClassVar[str] = "prompt_transform"


@dataclass(frozen=True)
class PromptBlockAction:
    reason: str
    kind: ClassVar[str] = "prompt_block"


@dataclass(frozen=True)
class ToolRewriteAction:
    arguments: Mapping[str, Any]
    kind: ClassVar[str] = "tool_rewrite"


@dataclass(frozen=True)
class ToolDenyAction:
    reason: str
    kind: ClassVar[str] = "tool_deny"


@dataclass(frozen=True)
class PermissionDecisionAction:
    decision: str
    reason: Optional[str] = None
    kind: ClassVar[str] = "permission_decision"

    def __post_init__(self) -> None:
        if self.decision not in ("allow_once", "deny", "abstain"):
            raise ValueError("decision must be 'allow_once', 'deny', or 'abstain'")


@dataclass(frozen=True)
class ToolAnnotateAction:
    content: str
    display: bool = True
    kind: ClassVar[str] = "tool_annotate"


@dataclass(frozen=True)
class ToolResultReplaceAction:
    content: str
    is_error: bool = False
    kind: ClassVar[str] = "tool_result_replace"


Action = Union[
    NoneAction,
    ContextAction,
    ContinueAction,
    StopAction,
    PromptTransformAction,
    PromptBlockAction,
    ToolRewriteAction,
    ToolDenyAction,
    PermissionDecisionAction,
    ToolAnnotateAction,
    ToolResultReplaceAction,
]


def none() -> NoneAction:
    return NoneAction()


def context(
    content: str, custom_type: str = "tny_extension", display: bool = True
) -> ContextAction:
    return ContextAction(content=content, custom_type=custom_type, display=display)


def continue_with(
    content: str,
    message_kind: str = "user",
    custom_type: Optional[str] = None,
    display: bool = True,
) -> ContinueAction:
    return ContinueAction(
        content=content,
        message_kind=message_kind,
        custom_type=custom_type,
        display=display,
    )


def stop(reason: str) -> StopAction:
    return StopAction(reason=reason)


def transform_prompt(prompt: str) -> PromptTransformAction:
    return PromptTransformAction(prompt=prompt)


def block_prompt(reason: str) -> PromptBlockAction:
    return PromptBlockAction(reason=reason)


def rewrite_tool(arguments: Mapping[str, Any]) -> ToolRewriteAction:
    if not isinstance(arguments, Mapping):
        raise TypeError("arguments must be a mapping")
    return ToolRewriteAction(arguments=dict(arguments))


def deny_tool(reason: str) -> ToolDenyAction:
    return ToolDenyAction(reason=reason)


def decide_permission(
    decision: str, reason: Optional[str] = None
) -> PermissionDecisionAction:
    return PermissionDecisionAction(decision=decision, reason=reason)


def annotate_tool(content: str, display: bool = True) -> ToolAnnotateAction:
    return ToolAnnotateAction(content=content, display=display)


def replace_tool_result(
    content: str, is_error: bool = False
) -> ToolResultReplaceAction:
    return ToolResultReplaceAction(content=content, is_error=is_error)


def _require_string(value: Any, field: str) -> str:
    if not isinstance(value, str):
        raise TypeError("action.%s must be a string" % field)
    return value


def coerce_action(value: Any) -> Action:
    """Normalize a typed action, a compatible mapping, or bare ``None``."""

    if value is None:
        return NoneAction()
    if isinstance(
        value,
        (
            NoneAction,
            ContextAction,
            ContinueAction,
            StopAction,
            PromptTransformAction,
            PromptBlockAction,
            ToolRewriteAction,
            ToolDenyAction,
            PermissionDecisionAction,
            ToolAnnotateAction,
            ToolResultReplaceAction,
        ),
    ):
        return value
    if not isinstance(value, Mapping):
        raise TypeError("handler must return a tny_ext action, mapping, or None")
    kind = value.get("kind")
    if kind == "none":
        return NoneAction()
    if kind == "context":
        display = value.get("display", True)
        if not isinstance(display, bool):
            raise TypeError("action.display must be a boolean")
        custom_type = _require_string(
            value.get("custom_type", "tny_extension"), "custom_type"
        )
        return ContextAction(
            _require_string(value.get("content"), "content"), custom_type, display
        )
    if kind == "continue":
        display = value.get("display", True)
        if not isinstance(display, bool):
            raise TypeError("action.display must be a boolean")
        custom_type = value.get("custom_type")
        if custom_type is not None:
            custom_type = _require_string(custom_type, "custom_type")
        return ContinueAction(
            _require_string(value.get("content"), "content"),
            _require_string(value.get("message_kind", "user"), "message_kind"),
            custom_type,
            display,
        )
    if kind == "stop":
        return StopAction(_require_string(value.get("reason"), "reason"))
    if kind == "prompt_transform":
        return PromptTransformAction(_require_string(value.get("prompt"), "prompt"))
    if kind == "prompt_block":
        return PromptBlockAction(_require_string(value.get("reason"), "reason"))
    if kind == "tool_rewrite":
        arguments = value.get("arguments")
        if not isinstance(arguments, Mapping):
            raise TypeError("action.arguments must be an object")
        return ToolRewriteAction(dict(arguments))
    if kind == "tool_deny":
        return ToolDenyAction(_require_string(value.get("reason"), "reason"))
    if kind == "permission_decision":
        reason = value.get("reason")
        if reason is not None:
            reason = _require_string(reason, "reason")
        return PermissionDecisionAction(
            _require_string(value.get("decision"), "decision"), reason
        )
    if kind == "tool_annotate":
        display = value.get("display", True)
        if not isinstance(display, bool):
            raise TypeError("action.display must be a boolean")
        return ToolAnnotateAction(
            _require_string(value.get("content"), "content"), display
        )
    if kind == "tool_result_replace":
        is_error = value.get("is_error", False)
        if not isinstance(is_error, bool):
            raise TypeError("action.is_error must be a boolean")
        return ToolResultReplaceAction(
            _require_string(value.get("content"), "content"), is_error
        )
    raise ValueError("unknown action kind: %r" % (kind,))


def action_to_dict(value: Any) -> Dict[str, Any]:
    action = coerce_action(value)
    if isinstance(action, NoneAction):
        return {"kind": "none", "type": "none"}
    if isinstance(action, ContextAction):
        return {
            "kind": "context",
            "type": "context",
            "content": action.content,
            "custom_type": action.custom_type,
            "display": action.display,
        }
    if isinstance(action, ContinueAction):
        result: Dict[str, Any] = {
            "kind": "continue",
            "type": "continue",
            "content": action.content,
            "message_kind": action.message_kind,
            "display": action.display,
        }
        if action.custom_type is not None:
            result["custom_type"] = action.custom_type
        return result
    if isinstance(action, StopAction):
        return {"kind": "stop", "type": "stop", "reason": action.reason}
    if isinstance(action, PromptTransformAction):
        return {"kind": action.kind, "type": action.kind, "prompt": action.prompt}
    if isinstance(action, PromptBlockAction):
        return {"kind": action.kind, "type": action.kind, "reason": action.reason}
    if isinstance(action, ToolRewriteAction):
        return {
            "kind": action.kind,
            "type": action.kind,
            "arguments": dict(action.arguments),
        }
    if isinstance(action, ToolDenyAction):
        return {"kind": action.kind, "type": action.kind, "reason": action.reason}
    if isinstance(action, PermissionDecisionAction):
        result = {"kind": action.kind, "type": action.kind, "decision": action.decision}
        if action.reason is not None:
            result["reason"] = action.reason
        return result
    if isinstance(action, ToolAnnotateAction):
        return {
            "kind": action.kind,
            "type": action.kind,
            "content": action.content,
            "display": action.display,
        }
    return {
        "kind": action.kind,
        "type": action.kind,
        "content": action.content,
        "is_error": action.is_error,
    }


__all__ = [
    "Action",
    "ContextAction",
    "ContinueAction",
    "NoneAction",
    "PermissionDecisionAction",
    "PromptBlockAction",
    "PromptTransformAction",
    "StopAction",
    "ToolAnnotateAction",
    "ToolDenyAction",
    "ToolResultReplaceAction",
    "ToolRewriteAction",
    "action_to_dict",
    "annotate_tool",
    "block_prompt",
    "coerce_action",
    "context",
    "continue_with",
    "decide_permission",
    "deny_tool",
    "none",
    "replace_tool_result",
    "rewrite_tool",
    "stop",
    "transform_prompt",
]
