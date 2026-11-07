"""A folder extension with a group: `notes.add`, `notes.list`, `notes.tags.list`.

    tny notes add "buy milk" --tag home
    tny notes tags list
    tools.notes.add(text="buy milk", tag="home")   # in code mode

The entry point is __main__.py; `from . import tags` nests the tools in
tags.py under `notes.tags`. Files starting with `_` are never loaded as
extensions, so _store.py is a safe place for shared code.
"""

from __future__ import annotations

import tny

from . import tags  # noqa: F401  (registers notes.tags.*)
from ._store import load, save


@tny.tool
def add(text: str, tag: str = "inbox") -> dict[str, str]:
    """Add a note."""
    notes = load()
    note = {"text": text, "tag": tag}
    notes.append(note)
    save(notes)
    return note


@tny.tool(name="list")
def list_notes(tag: str | None = None) -> list[dict[str, str]]:
    """List notes, optionally only one tag."""
    return [n for n in load() if tag is None or n["tag"] == tag]
