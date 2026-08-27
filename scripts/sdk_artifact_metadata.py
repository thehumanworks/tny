#!/usr/bin/env python3
"""Describe one libtny binary for language-package integrity checks."""

import ctypes
import hashlib
import json
import re
import struct
import subprocess
import sys
from pathlib import Path


class TnyBytes(ctypes.Structure):
    _fields_ = [("ptr", ctypes.c_void_p), ("len", ctypes.c_uint64)]


def target(path: Path) -> tuple[str, str]:
    header = path.read_bytes()[:64]
    if header.startswith(b"\x7fELF") and header[4:6] == b"\x02\x01":
        machine = int.from_bytes(header[18:20], "little")
        architecture = {62: "x86_64", 183: "aarch64"}.get(machine)
        if architecture and path.name == "libtny.so.1":
            return "linux", architecture
    if header.startswith(b"\xcf\xfa\xed\xfe") and len(header) >= 8:
        architecture = {0x0100000C: "arm64"}.get(struct.unpack_from("<I", header, 4)[0])
        if architecture and path.name == "libtny.1.dylib":
            return "darwin", architecture
    raise SystemExit("unsupported libtny artifact target")


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: sdk_artifact_metadata.py LIBRARY OUTPUT")
    library = Path(sys.argv[1]).resolve(strict=True)
    output = Path(sys.argv[2])
    os_name, architecture = target(library)
    native = ctypes.CDLL(str(library))
    native.tny_abi_version.restype = ctypes.c_uint32
    native.tny_library_version.restype = TnyBytes
    abi = int(native.tny_abi_version())
    if abi >> 16 != 1:
        raise SystemExit("SDK artifact must be libtny ABI major 1")
    version_view = native.tny_library_version()
    library_version = (
        ctypes.string_at(version_view.ptr, version_view.len).decode("utf-8", "strict")
        if version_view.ptr and version_view.len
        else ""
    )
    if not library_version:
        raise SystemExit("SDK artifact has no library version")
    if os_name == "linux":
        dynamic = subprocess.check_output(["readelf", "-d", library], text=True)
        match = re.search(r"\(SONAME\).*?\[([^]]+)\]", dynamic)
        identity = {"kind": "soname", "value": match.group(1) if match else ""}
        if identity["value"] != "libtny.so.1":
            raise SystemExit("SDK artifact has the wrong ELF SONAME")
    else:
        lines = subprocess.check_output(
            ["otool", "-D", library], text=True
        ).splitlines()
        identity = {
            "kind": "install_name",
            "value": lines[1].strip() if len(lines) > 1 else "",
        }
        if identity["value"] != "@rpath/libtny.1.dylib":
            raise SystemExit("SDK artifact has the wrong Mach-O install name")
    metadata = {
        "schema_version": 2,
        "filename": library.name,
        "sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
        "os": os_name,
        "arch": architecture,
        "abi_major": abi >> 16,
        "abi_minor": abi & 0xFFFF,
        "library_version": library_version,
        "dynamic_identity": identity,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
