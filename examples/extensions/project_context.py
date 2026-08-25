"""Add visible workspace context before every provider turn."""

import os
from pathlib import Path
from typing import Optional

from tny_ext import BeforeAgentStartEvent, ContextAction, ExtensionAPI, context


MAX_CONTEXT_CHARS = 16_384


def _context_path() -> Path:
    configured = os.environ.get("TNY_CONTEXT_FILE")
    return Path(configured).expanduser() if configured else Path(".tny-context.md")


def setup(api: ExtensionAPI) -> None:
    @api.on(BeforeAgentStartEvent)
    def add_project_context(event: BeforeAgentStartEvent) -> Optional[ContextAction]:
        del event
        path = _context_path()
        if not path.is_file():
            return None
        content = path.read_text(encoding="utf-8")[:MAX_CONTEXT_CHARS].strip()
        if not content:
            return None
        return context(content, custom_type="project_context", display=True)
