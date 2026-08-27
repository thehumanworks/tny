"""Append provider-neutral event metadata to a JSONL audit file."""

import json
import os
from pathlib import Path

from tny_ext import ExtensionAPI, HookEvent


def _log_path() -> Path:
    configured = os.environ.get("TNY_EVENT_LOG")
    return (
        Path(configured).expanduser()
        if configured
        else Path.home() / ".tny" / "events.jsonl"
    )


def setup(api: ExtensionAPI) -> None:
    @api.on("*")
    def log_event(event: HookEvent) -> None:
        path = _log_path()
        path.parent.mkdir(parents=True, exist_ok=True)
        record = {
            "event_id": event.event_id,
            "type": event.type,
            "sequence": event.sequence,
            "provider": event.provider,
            "session_id": event.session_id,
            "turn_id": event.turn_id,
            "timestamp_ms": event.timestamp_ms,
        }
        with path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(record, separators=(",", ":")) + "\n")
