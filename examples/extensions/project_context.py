"""Add visible workspace context before the first provider turn of a session."""

import os
from pathlib import Path
from typing import Optional, Set

from tny_ext import (
    BeforeAgentStartEvent,
    ContextAction,
    ExtensionAPI,
    SessionStartEvent,
    context,
)


MAX_CONTEXT_CHARS = 16_384


def _context_path() -> Path:
    configured = os.environ.get("TNY_CONTEXT_FILE")
    return Path(configured).expanduser() if configured else Path(".tny-context.md")


def setup(api: ExtensionAPI) -> None:
    pending_sessions: Set[str] = set()

    @api.on(SessionStartEvent)
    def begin_session(event: SessionStartEvent) -> None:
        pending_sessions.add(event.session_id)

    @api.on(BeforeAgentStartEvent)
    def add_project_context(event: BeforeAgentStartEvent) -> Optional[ContextAction]:
        if event.session_id not in pending_sessions:
            return None
        pending_sessions.remove(event.session_id)
        path = _context_path()
        if not path.is_file():
            return None
        content = path.read_text(encoding="utf-8")[:MAX_CONTEXT_CHARS].strip()
        if not content:
            return None
        return context(content, custom_type="project_context", display=True)
