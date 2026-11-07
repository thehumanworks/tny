from __future__ import annotations

import json
import os
from pathlib import Path


def _path() -> Path:
    return Path(os.environ.get("NOTES_FILE", Path.home() / ".tny" / "notes.json"))


def load() -> list[dict[str, str]]:
    try:
        data = json.loads(_path().read_text())
    except FileNotFoundError:
        return []
    return [dict(n) for n in data]


def save(notes: list[dict[str, str]]) -> None:
    path = _path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(notes, indent=2) + "\n")
