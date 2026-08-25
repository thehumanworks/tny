"""State kept by the ci_guard directory extension."""

from typing import List

from tny_ext import ToolEndEvent


class FailedTools:
    def __init__(self) -> None:
        self.names: List[str] = []

    def observe(self, event: ToolEndEvent) -> None:
        if not event.ok:
            self.names.append(event.tool_name or "tool")

    def take(self) -> List[str]:
        names = self.names
        self.names = []
        return names
