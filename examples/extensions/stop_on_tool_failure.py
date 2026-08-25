"""Request agent cancellation when a normalized tool result is unsuccessful."""

from typing import Optional

from tny_ext import ExtensionAPI, StopAction, ToolEndEvent, stop


def setup(api: ExtensionAPI) -> None:
    @api.on(ToolEndEvent)
    def stop_after_failed_tool(event: ToolEndEvent) -> Optional[StopAction]:
        if event.ok:
            return None
        name = event.tool_name or "tool"
        return stop("%s failed; stopping this agent run" % name)
