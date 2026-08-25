"""Registration API passed to an extension's ``setup(api)`` function."""

from dataclasses import dataclass
from typing import Any, Callable, List, Optional, Tuple, Union

from .capabilities import CapabilityView
from .events import HookEvent, event_name


Handler = Callable[[HookEvent], Any]
EventSelector = Union[str, type]


@dataclass(frozen=True)
class HandlerRegistration:
    event: str
    handler: Handler
    index: int


class ExtensionAPI:
    """Collect handlers for one extension during setup.

    ``on`` supports both decorator and direct-registration styles::

        @api.on("agent_end")
        def continue_if_needed(event): ...

        api.on(AgentSettledEvent, settled_handler)
    """

    def __init__(self, name: str, capabilities: Optional[CapabilityView] = None) -> None:
        self.name = name
        self.capabilities = capabilities or CapabilityView.empty()
        self._handlers: List[HandlerRegistration] = []

    def on(self, event: EventSelector, handler: Optional[Handler] = None) -> Any:
        name = event_name(event)

        def register(callback: Handler) -> Handler:
            if not callable(callback):
                raise TypeError("event handler must be callable")
            self._handlers.append(HandlerRegistration(name, callback, len(self._handlers)))
            return callback

        if handler is None:
            return register
        return register(handler)

    @property
    def registrations(self) -> Tuple[HandlerRegistration, ...]:
        return tuple(self._handlers)


__all__ = ["ExtensionAPI", "Handler", "HandlerRegistration"]
