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


Action = Union[NoneAction, ContextAction, ContinueAction, StopAction]


def none() -> NoneAction:
    return NoneAction()


def context(content: str, custom_type: str = "tny_extension", display: bool = True) -> ContextAction:
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


def _require_string(value: Any, field: str) -> str:
    if not isinstance(value, str):
        raise TypeError("action.%s must be a string" % field)
    return value


def coerce_action(value: Any) -> Action:
    """Normalize a typed action, a compatible mapping, or bare ``None``."""

    if value is None:
        return NoneAction()
    if isinstance(value, (NoneAction, ContextAction, ContinueAction, StopAction)):
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
        custom_type = _require_string(value.get("custom_type", "tny_extension"), "custom_type")
        return ContextAction(_require_string(value.get("content"), "content"), custom_type, display)
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
    return {"kind": "stop", "type": "stop", "reason": action.reason}


__all__ = [
    "Action",
    "ContextAction",
    "ContinueAction",
    "NoneAction",
    "StopAction",
    "action_to_dict",
    "coerce_action",
    "context",
    "continue_with",
    "none",
    "stop",
]
