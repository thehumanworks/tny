"""Stateful directory extension that asks the agent to recover failed tools."""

from typing import Optional

from tny_ext import (
    AgentEndEvent,
    ContinueAction,
    ExtensionAPI,
    ToolEndEvent,
    continue_with,
)

from .rules import FailedTools


def setup(api: ExtensionAPI) -> None:
    failed = FailedTools()

    @api.on(ToolEndEvent)
    def remember_failure(event: ToolEndEvent) -> None:
        failed.observe(event)

    @api.on(AgentEndEvent)
    def recover_once(event: AgentEndEvent) -> Optional[ContinueAction]:
        names = failed.take()
        if not names or event.continuation_count != 0:
            return None
        return continue_with(
            "Review and recover the failed tool calls: %s" % ", ".join(names),
            message_kind="custom",
            custom_type="ci_guard",
            display=True,
        )
