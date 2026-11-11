"""Freeze the pinned CPython standard library into C sources (docs/adr/0180).

Run by the just-built interpreter from scripts/cpython_runtime.sh, never by a
host Python: the marshal format must match the embedded runtime exactly.

    python -E -S cpython_freeze_stdlib.py LIB_DIR OUT_DIR [EXTRA_MODULE.py ...]

Every pure-Python module under LIB_DIR is compiled as CPython compiles its own
frozen modules (filename "<frozen NAME>") and written as one C file per module
plus tny_frozen_stdlib.c, whose NULL-terminated `struct _frozen` table the code
cell installs as PyImport_FrozenModules. Only the regression-test suite, GUI
packages whose native toolkit is never built, and ensurepip/venv (which need a
Python executable and bundled wheels) are left out; see EXCLUDED_TOP.

TNY_SHELL_PATH in the environment names the shell the build owns. When it is
not the default /bin/sh (a Nix build passes a store path, docs/adr/0220), every
"/bin/sh" in the frozen sources becomes that path, so `subprocess` with
shell=True runs the same shell as tny's terminal tool and the binary names no
host shell.
"""

from __future__ import annotations

import marshal
import os
import sys

# Top-level names that cannot work in an embedded, executable-less runtime or
# are CPython's own test suite. Everything else in Lib/ is frozen.
EXCLUDED_TOP = {
    "__phello__",  # import-system test package
    "ensurepip",  # bundled pip wheels, needs sys.executable
    "idlelib",  # Tk GUI
    "site-packages",  # third-party install location, empty in the source tree
    "test",  # CPython regression suite
    "tkinter",  # needs _tkinter/Tcl/Tk
    "turtle.py",  # tkinter
    "turtledemo",  # tkinter
    "venv",  # creates environments around sys.executable
}
# Test-only subpackages inside otherwise shipped packages.
EXCLUDED_DIRS = {"__pycache__", "idle_test", "test", "tests"}
HOST_SHELL = b"/bin/sh"


def build_shell(environ) -> bytes:
    """The shell that replaces HOST_SHELL, or HOST_SHELL itself to change nothing."""
    shell = environ.get("TNY_SHELL_PATH") or HOST_SHELL.decode()
    # The path is spliced into Python string literals of either quote style.
    if not shell.startswith("/") or any(c in shell for c in "'\"\\\n\r\0"):
        raise SystemExit(f"TNY_SHELL_PATH must be a plain absolute path: {shell!r}")
    return shell.encode()


def with_build_shell(source: bytes, shell: bytes) -> bytes:
    """Replace every HOST_SHELL, including the one inside "/system/bin/sh".

    That Android-only fallback is dead on tny's targets; a narrower match
    would leave its "/bin/sh" suffix in the binary for the package check.
    """
    return source if shell == HOST_SHELL else source.replace(HOST_SHELL, shell)


def modules(lib: str):
    for directory, dirnames, filenames in os.walk(lib):
        relative = os.path.relpath(directory, lib)
        parts = [] if relative == "." else relative.split(os.sep)
        if parts and parts[0] in EXCLUDED_TOP:
            dirnames[:] = []
            continue
        dirnames[:] = sorted(
            d for d in dirnames if d not in EXCLUDED_DIRS and not d.startswith(".")
        )
        for filename in sorted(filenames):
            if not filename.endswith(".py") or (not parts and filename in EXCLUDED_TOP):
                continue
            stem = filename[:-3]
            package = stem == "__init__"
            if not parts and package:
                continue
            name = ".".join(parts if package else [*parts, stem])
            # Only importable dotted names; a directory without __init__.py is
            # a namespace fragment and is still reachable if a parent exists.
            if all(part.isidentifier() for part in name.split(".")):
                yield name, os.path.join(directory, filename), package


def literal(data: bytes) -> str:
    """C string literal lines; 3-digit octal escapes cannot absorb a digit."""
    lines = []
    for start in range(0, len(data), 64):
        chunk = data[start : start + 64]
        text = "".join(
            chr(b) if 0x20 <= b < 0x7F and b not in b'"\\?' else f"\\{b:03o}"
            for b in chunk
        )
        lines.append(f'    "{text}"')
    return "\n".join(lines) or '    ""'


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    lib, out, extras = argv[1], argv[2], argv[3:]
    os.makedirs(out, exist_ok=True)
    entries = list(modules(lib))
    for extra in extras:
        entries.append((os.path.basename(extra)[:-3], extra, False))
    names = [name for name, _, _ in entries]
    if len(names) != len(set(names)):
        raise SystemExit("duplicate frozen module name")
    shell = build_shell(os.environ)
    table = []
    for index, (name, path, package) in enumerate(entries):
        with open(path, "rb") as handle:
            source = with_build_shell(handle.read(), shell)
        code = compile(source, f"<frozen {name}>", "exec", dont_inherit=True)
        data = marshal.dumps(code)
        symbol = f"tny_frozen_{index}"
        with open(os.path.join(out, f"{symbol}.c"), "w", encoding="ascii") as handle:
            handle.write(
                f"/* {name}: generated by scripts/cpython_freeze_stdlib.py */\n"
            )
            handle.write(f"const unsigned char {symbol}[{len(data) + 1}] =\n")
            handle.write(literal(data) + ";\n")
        table.append((name, symbol, len(data), package))
    with open(
        os.path.join(out, "tny_frozen_stdlib.c"), "w", encoding="ascii"
    ) as handle:
        handle.write("/* Generated by scripts/cpython_freeze_stdlib.py. */\n")
        handle.write("#include <stddef.h>\n")
        handle.write(
            "struct _frozen {\n    const char *name;\n    const unsigned char *code;\n"
            "    int size;\n    int is_package;\n};\n"
        )
        for _, symbol, size, _ in table:
            handle.write(f"extern const unsigned char {symbol}[{size + 1}];\n")
        handle.write("const struct _frozen tny_frozen_stdlib[] = {\n")
        for name, symbol, size, package in table:
            handle.write(f'    {{"{name}", {symbol}, {size}, {int(package)}}},\n')
        handle.write("    {NULL, NULL, 0, 0},\n};\n")
        handle.write(f"const int tny_frozen_stdlib_count = {len(table)};\n")
    with open(os.path.join(out, "modules.txt"), "w", encoding="ascii") as handle:
        handle.writelines(f"{name}\n" for name in names)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
