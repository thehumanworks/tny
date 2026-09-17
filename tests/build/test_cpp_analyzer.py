#!/usr/bin/env python3
"""Check real owner factories and prove GCC's lifetime diagnostics remain active."""

from __future__ import annotations

import argparse
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = '#include "json/ownership.hpp"\n#include <cstdlib>\n'
CASES = (
    (
        "owners",
        "",
        """
struct value { int number; explicit value(int n) noexcept : number(n) {} };
int exercise(const char *data, std::size_t size) {
    auto owned = tny::make_owned<value>(42);
    auto parsed = tny::parse(data, size);
    auto mutable_doc = tny::make_document();
    return owned->number + (parsed ? 1 : 0) + (mutable_doc ? 1 : 0);
}
""",
    ),
    (
        "uninitialized",
        "analyzer-use-of-uninitialized-value",
        """
int exercise() {
    auto *p = static_cast<int *>(std::malloc(sizeof(int)));
    if (!p) return 0;
    volatile int value = *p;
    std::free(p);
    return value;
}
""",
    ),
    (
        "use-after-free",
        "analyzer-use-after-free",
        """
int exercise() {
    auto *p = static_cast<int *>(std::malloc(sizeof(int)));
    if (!p) return 0;
    *p = 42;
    std::free(p);
    return *p;
}
""",
    ),
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--flags", required=True)
    args = parser.parse_args()
    compiler = shlex.split(args.cxx)
    flags = shlex.split(args.flags)
    with tempfile.TemporaryDirectory(prefix="tny-cpp-analyzer-") as directory:
        root = Path(directory)
        for name, diagnostic, body in CASES:
            source = root / (name + ".cpp")
            source.write_text(HEADER + body)
            result = subprocess.run(
                [
                    *compiler,
                    *flags,
                    "-fanalyzer",
                    "-O1",
                    "-c",
                    str(source),
                    "-o",
                    str(root / (name + ".o")),
                ],
                cwd=ROOT,
                text=True,
                capture_output=True,
                timeout=120,
            )
            output = result.stdout + result.stderr
            if diagnostic:
                if result.returncode == 0 or diagnostic not in output:
                    raise AssertionError(
                        f"{name}: expected the enabled {diagnostic} rejection\n{output}"
                    )
            elif result.returncode != 0 or output:
                raise AssertionError(f"owner factories must analyze cleanly\n{output}")
    print("C++ analyzer: owner factories pass; both lifetime defects are rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
