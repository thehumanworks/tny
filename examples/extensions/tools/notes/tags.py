from __future__ import annotations

import tny

from ._store import load


@tny.tool(name="list")
def list_tags() -> list[str]:
    """Every tag in use."""
    return sorted({n["tag"] for n in load()})
