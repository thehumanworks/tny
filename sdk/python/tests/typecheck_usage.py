"""Public import surface checked with mypy --strict."""
from __future__ import annotations

import tny


def consume(runtime: tny.Runtime, session: tny.Session, event: tny.AnyEvent) -> bytes:
    config: tny.RuntimeConfig = runtime.config
    if isinstance(event, tny.TextDeltaEvent):
        return event.text
    if isinstance(event, tny.UnknownEvent):
        value = event.payload.get("text", b"")
        return value if isinstance(value, bytes) else b""
    token = tny.CancellationToken()
    token.cancel()
    _ = config
    _ = session
    return b""
