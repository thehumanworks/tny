"""Build size-comparable native embedding probes; no shipped dependency changes."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import sysconfig
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
BUILD = ROOT / "build/code-mode-bench"
VERSION = "2026-06-04"
SHA256 = "b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a"
FLAGS = [
    "-std=c11",
    "-D_GNU_SOURCE",
    "-Os",
    "-flto=auto",
    "-ffunction-sections",
    "-fdata-sections",
]


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--download",
        action="store_true",
        help="Allow the pinned official QuickJS download",
    )
    args = parser.parse_args()
    if sys.platform != "linux" or sys.version_info[:2] != (3, 14):
        raise SystemExit(
            "These native probes require Linux and CPython 3.14 embedding development files"
        )
    BUILD.mkdir(parents=True, exist_ok=True)
    archive = BUILD / f"quickjs-{VERSION}.tar.xz"
    if not archive.exists():
        if not args.download:
            raise SystemExit("Missing pinned archive; explicitly enable --download")
        with urllib.request.urlopen(
            f"https://bellard.org/quickjs/{archive.name}", timeout=60
        ) as response:
            archive.write_bytes(response.read(2 * 1024 * 1024))
    if digest(archive) != SHA256:
        raise SystemExit("QuickJS archive SHA256 mismatch")
    vendor = BUILD / f"quickjs-{VERSION}"
    with tarfile.open(archive) as tar:
        tar.extractall(BUILD, filter="data")
    common = [HERE / "host.c", ROOT / "third_party/yyjson/yyjson.c"]
    excluded = {
        "lua.c",
        "luac.c",
        "linit.c",
        "liolib.c",
        "loslib.c",
        "loadlib.c",
        "ldblib.c",
        "lcorolib.c",
    }
    # The measured Lua runtime moved out of production (ADR 0179); these
    # benchmark-only copies are byte-identical to the PR #197 sources.
    lua_root = HERE / "lua_runtime"
    lua = sorted(p for p in (lua_root / "lua").glob("*.c") if p.name not in excluded)
    sources = {
        "empty": common + [HERE / "empty.c"],
        "lua": common
        + [HERE / "lua.c", lua_root / "core/code_runtime.c", ROOT / "src/util/util.c"]
        + lua,
        "javascript": common
        + [HERE / "javascript.c"]
        + [
            vendor / f"{name}.c"
            for name in ("quickjs", "dtoa", "libregexp", "libunicode", "cutils")
        ],
        "python": common + [HERE / "python.c"],
    }
    python_home = Path(sysconfig.get_config_var("LIBDIR")).parent.resolve()
    compiler = os.environ.get("CC", "cc")
    receipt = {
        "quickjs_version": VERSION,
        "quickjs_sha256": SHA256,
        "python_version": sys.version,
        "python_home": str(python_home),
        "compiler": subprocess.check_output(
            [compiler, "--version"], text=True
        ).splitlines()[0],
        "binaries": {},
        "commands": {},
        "sources": {},
    }
    includes = [
        f"-I{HERE / 'lua_runtime'}",
        f"-I{ROOT / 'src'}",
        f"-I{ROOT / 'include'}",
        f"-I{ROOT / 'third_party'}",
        f"-I{ROOT / 'third_party/yyjson'}",
        f"-I{vendor}",
        f"-I{sysconfig.get_config_var('INCLUDEPY')}",
    ]
    for language, inputs in sources.items():
        target = BUILD / language
        command = [
            compiler,
            *FLAGS,
            *includes,
            '-DCONFIG_VERSION="' + VERSION + '"',
            '-DPYHOME="' + str(python_home) + '"',
            *(str(p) for p in inputs),
            "-Wl,--gc-sections",
            "-o",
            str(target),
            "-lm",
            "-ldl",
            "-pthread",
        ]
        if language == "javascript":
            command += ["-Dasm=__asm__"]  # upstream uses GNU asm in its C11 source
        if language == "python":
            command += [
                f"-L{python_home / 'lib'}",
                f"-Wl,-rpath,{python_home / 'lib'}",
                "-lpython3.14",
                "-lutil",
            ]
        print(f"Building {language}", flush=True)
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run(["strip", "--strip-unneeded", str(target)], check=True)
        receipt["commands"][language] = command
        receipt["binaries"][language] = {
            "bytes": target.stat().st_size,
            "sha256": digest(target),
            "dependencies": subprocess.check_output(["ldd", str(target)], text=True),
        }
        for source in inputs:
            receipt["sources"][str(source.relative_to(ROOT))] = digest(source)
    library = python_home / "lib" / sysconfig.get_config_var("LDLIBRARY")
    stdlib = Path(sysconfig.get_path("stdlib")).resolve()
    stdlib_files = [
        p
        for p in stdlib.rglob("*")
        if p.is_file()
        and "site-packages" not in p.parts
        and "__pycache__" not in p.parts
    ]
    receipt["python_closure"] = {
        "libpython_bytes": library.stat().st_size,
        "stdlib_without_site_packages_or_pycache_bytes": sum(
            p.stat().st_size for p in stdlib_files
        ),
        "note": "Installed dynamic distribution, not a minimized static CPython bundle",
    }
    (BUILD / "build.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({k: v["bytes"] for k, v in receipt["binaries"].items()}))


if __name__ == "__main__":
    main()
