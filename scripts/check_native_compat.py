#!/usr/bin/env python3
"""Enforce the published macOS and glibc compatibility floors."""

import argparse
import re
import subprocess
from pathlib import Path


def version(value: str) -> tuple[int, ...]:
    return tuple(int(part) for part in value.split("."))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("artifact", type=Path)
    parser.add_argument("--max-macos-min", default="13.0")
    parser.add_argument("--max-glibc", default="2.34")
    args = parser.parse_args()
    artifact = args.artifact.resolve(strict=True)
    header = artifact.read_bytes()[:8]
    if header.startswith(b"\xcf\xfa\xed\xfe"):
        if artifact.name.startswith("libtny"):
            identities = subprocess.check_output(
                ["otool", "-D", artifact], text=True
            ).splitlines()
            if len(identities) < 2 or identities[1].strip() != "@rpath/libtny.1.dylib":
                raise SystemExit("Mach-O libtny artifact has the wrong install name")
        output = subprocess.check_output(["otool", "-l", artifact], text=True)
        values = re.findall(r"^\s*minos\s+([0-9.]+)\s*$", output, re.MULTILINE)
        if not values:
            raise SystemExit("Mach-O artifact has no LC_BUILD_VERSION minos")
        observed = max(values, key=version)
        if version(observed) > version(args.max_macos_min):
            raise SystemExit(f"Mach-O minimum {observed} exceeds {args.max_macos_min}")
        print(f"native compatibility: macOS minimum {observed}")
        return
    if header.startswith(b"\x7fELF"):
        dynamic = subprocess.check_output(["readelf", "-d", artifact], text=True)
        if artifact.name.startswith("libtny") and not re.search(
            r"\(SONAME\).*\[libtny\.so\.1\]", dynamic
        ):
            raise SystemExit("ELF libtny artifact has the wrong SONAME")
        output = subprocess.check_output(
            ["readelf", "--version-info", artifact], text=True
        )
        values = re.findall(r"GLIBC_([0-9]+\.[0-9]+)", output)
        observed = max(values, key=version) if values else "0.0"
        if version(observed) > version(args.max_glibc):
            raise SystemExit(f"ELF requires glibc {observed}, exceeds {args.max_glibc}")
        print(f"native compatibility: glibc >= {observed}")
        return
    raise SystemExit("unsupported native artifact format")


if __name__ == "__main__":
    main()
