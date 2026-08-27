#!/usr/bin/env python3
"""Describe one libtny binary for language-package integrity checks."""

import hashlib
import json
import struct
import sys
from pathlib import Path


def target(path: Path) -> tuple[str, str]:
    header = path.read_bytes()[:64]
    if header.startswith(b"\x7fELF") and header[4:6] == b"\x02\x01":
        machine = int.from_bytes(header[18:20], "little")
        architecture = {62: "x86_64", 183: "aarch64"}.get(machine)
        if architecture and path.name == "libtny.so.0":
            return "linux", architecture
    if header.startswith(b"\xcf\xfa\xed\xfe") and len(header) >= 8:
        architecture = {0x0100000C: "arm64"}.get(
            struct.unpack_from("<I", header, 4)[0]
        )
        if architecture and path.name == "libtny.0.dylib":
            return "darwin", architecture
    raise SystemExit("unsupported libtny artifact target")


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: sdk_artifact_metadata.py LIBRARY OUTPUT")
    library = Path(sys.argv[1]).resolve(strict=True)
    output = Path(sys.argv[2])
    os_name, architecture = target(library)
    metadata = {
        "schema_version": 1,
        "filename": library.name,
        "sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
        "os": os_name,
        "arch": architecture,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
