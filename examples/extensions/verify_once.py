"""Ask for one verification pass before allowing a successful run to settle."""

from typing import Optional

from tny_ext import AgentEndEvent, ContinueAction, ExtensionAPI, continue_with


MARKER = "verification complete"


def setup(api: ExtensionAPI) -> None:
    @api.on(AgentEndEvent)
    def verify_once(event: AgentEndEvent) -> Optional[ContinueAction]:
        if event.stop.reason != "done":
            return None
        if event.continuation_count != 0:
            return None
        if MARKER in event.output_text.lower():
            return None
        return continue_with(
            "Run the relevant checks before finishing. Summarize the results "
            "and end with: Verification complete",
            message_kind="user",
            display=True,
        )
