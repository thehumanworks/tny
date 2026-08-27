#!/usr/bin/env python3
"""Write a deterministic SHA-256 manifest for one staged artifact tree."""

import hashlib
import os
import sys
from pathlib import Path


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: artifact_manifest.py ROOT OUTPUT")
    root = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    for path in sorted(root.rglob("*")):
        if path == output or path.is_dir():
            continue
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            target = os.readlink(path)
            link_hash = hashlib.sha256(target.encode()).hexdigest()
            rows.append(f"{link_hash}  {relative} -> {target}")
        else:
            rows.append(f"{digest(path)}  {relative}")
    output.write_text("\n".join(rows) + "\n")


if __name__ == "__main__":
    main()
