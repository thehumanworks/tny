#!/usr/bin/env python3
"""Run C++ tidy with the selected driver's system headers, including its STL.

A standalone clang-tidy wheel need not share the host C++ driver's installation
or standard library. Ask that driver for its search path rather than guessing
Xcode, GCC, Nix or SDK installation paths. Pass paths as argv, preserving spaces.
"""

from __future__ import annotations

import argparse
import os
import shlex
import subprocess
import sys


def driver_flags(flags: list[str]) -> list[str]:
    """Keep language/target selection, never turn first-party -I into -isystem."""
    result = []
    iterator = iter(flags)
    for flag in iterator:
        if flag in ("-isysroot", "--sysroot", "-target", "--target"):
            result.extend((flag, next(iterator)))
        elif flag.startswith(
            ("-std=", "-stdlib=", "--sysroot=", "--target=")
        ) or flag in (
            "-m32",
            "-m64",
        ):
            result.append(flag)
    return result


def system_includes(output: str) -> list[str]:
    result = []
    active = False
    for line in output.splitlines():
        if line.strip() == "#include <...> search starts here:":
            active = True
        elif active and line.strip() == "End of search list.":
            return result
        elif active:
            path = line.strip()
            suffix = " (framework directory)"
            framework = path.endswith(suffix)
            if framework:
                path = path[: -len(suffix)]
            if path:
                result.extend(("-iframework" if framework else "-isystem", path))
    raise ValueError("C++ driver did not emit a complete include search list")


def main() -> int:
    split = sys.argv.index("--")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--tidy", required=True)
    parser.add_argument("sources", nargs="+")
    args = parser.parse_args(sys.argv[1:split])
    flags = sys.argv[split + 1 :]
    probe = subprocess.run(
        [*shlex.split(args.cxx), *driver_flags(flags), "-E", "-x", "c++", "-v", "-"],
        input="",
        text=True,
        capture_output=True,
        env={**os.environ, "LC_ALL": "C"},
        check=False,
    )
    if probe.returncode:
        sys.stderr.write(probe.stderr)
        return probe.returncode
    includes = system_includes(probe.stderr)
    if not includes:
        raise ValueError("C++ driver reported no system include directories")
    # C-interface .h enums retain their C ABI widths. C tidy still checks those
    # headers in C translation units. C++-specific advice (notably LLVM22's
    # performance-enum-size, which has no extern-C exemption) belongs to private
    # .hpp/.cpp. This filters header advice only: compiler errors in every
    # included header remain fatal, and every configured C++ check stays enabled.
    return subprocess.run(
        [
            *shlex.split(args.tidy),
            "--quiet",
            r"--header-filter=(^|/)(src|include)/.*\.(cpp|hpp)$",
            *args.sources,
            "--",
            *flags,
            *includes,
        ],
        check=False,
    ).returncode


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, StopIteration, OSError) as error:
        print(f"C++ tidy: {error}", file=sys.stderr)
        raise SystemExit(1) from error
