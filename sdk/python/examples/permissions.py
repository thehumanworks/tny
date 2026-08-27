"""Explicitly answer permission requests in ASK mode."""
import os

from tny import (
    PermissionDecision,
    PermissionMode,
    PermissionRequestEvent,
    Runtime,
    RuntimeConfig,
)

config = RuntimeConfig(
    workspace=os.getcwd(), state_dir=os.path.join(os.getcwd(), ".tny-sdk-state"),
    base_url=os.environ["OPENAI_BASE_URL"], api_key=os.environ["OPENAI_API_KEY"],
    permission_mode=PermissionMode.ASK,
)
with Runtime(config) as runtime, runtime.create_session() as session:
    session.send("Inspect the repository")
    for event in session.events():
        if isinstance(event, PermissionRequestEvent):
            summary = event.summary.decode("utf-8", "strict")
            decision = PermissionDecision.ALLOW if input(f"Allow {summary}? [y/N] ").lower() == "y" else PermissionDecision.DENY
            session.respond_permission(event, decision)
