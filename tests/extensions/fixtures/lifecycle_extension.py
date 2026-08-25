from tny_ext import (
    AgentEndEvent,
    AgentSettledEvent,
    BeforeAgentStartEvent,
    context,
    continue_with,
    stop,
)


def setup(api):
    # This must go to host stderr, never the JSONL protocol stream.
    print("fixture setup noise")

    @api.on(BeforeAgentStartEvent)
    def add_context(event):
        assert isinstance(event, BeforeAgentStartEvent)
        return context("context for " + event.prompt, custom_type="fixture_context")

    @api.on(AgentEndEvent)
    async def continue_after_end(event):
        assert isinstance(event, AgentEndEvent)
        return continue_with("review the result")

    @api.on(AgentSettledEvent)
    def stop_after_settle(_event):
        return stop("fixture condition reached")

    @api.on("text_delta")
    def observe_text(_event):
        return None

    @api.on("status")
    def fail_without_crashing(_event):
        print("fixture handler noise")
        raise RuntimeError("fixture handler failed")

    @api.on("future_event")
    def future_event(event):
        return {
            "kind": "continue",
            "content": event.payload["instruction"],
            "message_kind": "custom",
            "custom_type": "future_fixture",
        }
