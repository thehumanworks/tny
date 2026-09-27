"""Build Python-runtime embedding probes over the preserved benchmark host.

Benchmark only: nothing here is linked into shipped tny artifacts. Sources are
pinned by SHA256 and must already exist in --sources (no implicit download).
Uses the preserved benchmark's -Os/LTO/section-GC flags and its host.c/bench.h.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
PRIOR = ROOT / "tests/bench/code_mode"
BUILD = ROOT / "build/python-runtime-bench"
FLAGS = ["-std=c11", "-D_GNU_SOURCE", "-Os", "-flto=auto", "-ffunction-sections", "-fdata-sections"]
PINS = {
    "pocketpy.c": "0f0c19071b4fd0b37cb292e15bea49ca163f151af901b00893d3a2005bb056e6",
    "pocketpy.h": "43864cfa090d65b80915467900430dfae52c769f13d6edb137d0227d1fbe03ad",
    "micropython-1.29.0.tar.xz": "d925a7c664e79a2bdf3dfcb285ba5e2237041cc35a0bd4ee573b6c5711efeca0",
}
MONTY_COMMIT = "85c5d1f6bef038405cfc40a4eed94806e303567e"  # tag v1.0.0


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def link(name: str, inputs: list[Path], includes: list[Path], extra: list[str], receipt: dict) -> None:
    target = BUILD / name
    compiler = os.environ.get("CC", "cc")
    command = [
        compiler,
        *FLAGS,
        *(f"-I{p}" for p in [ROOT / "src", ROOT / "include", ROOT / "third_party", ROOT / "third_party/yyjson", PRIOR, *includes]),
        *(str(p) for p in [PRIOR / "host.c", ROOT / "third_party/yyjson/yyjson.c", *inputs]),
        "-Wl,--gc-sections",
        "-o",
        str(target),
        *extra,
        "-lm",
        "-ldl",
        "-pthread",
    ]
    print(f"Building {name}", flush=True)
    subprocess.run(command, cwd=ROOT, check=True)
    subprocess.run(["strip", "--strip-unneeded", str(target)], check=True)
    receipt["binaries"][name] = {
        "bytes": target.stat().st_size,
        "sha256": digest(target),
        "dependencies": subprocess.check_output(["ldd", str(target)], text=True),
    }
    receipt["commands"][name] = command


def build_micropython(sources: Path) -> tuple[list[Path], list[Path]]:
    top = BUILD / "micropython-1.29.0"
    if not top.exists():
        with tarfile.open(sources / "micropython-1.29.0.tar.xz") as tar:
            tar.extractall(BUILD, filter="data")
    package = BUILD / "micropython_embed"
    work = BUILD / "micropython-work"
    shutil.rmtree(package, ignore_errors=True)
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir()
    shutil.copy(HERE / "micropython/mpconfigport.h", work / "mpconfigport.h")
    subprocess.run(
        ["make", "-f", str(HERE / "micropython/embed.mk"), f"MICROPYTHON_TOP={top}", f"PACKAGE_DIR={package}", "-j3"],
        cwd=work,
        check=True,
        stdout=subprocess.DEVNULL,
    )
    shutil.copy(top / "extmod/modjson.c", package / "extmod/modjson.c")
    # Route print() to the benchmark host and make fatal paths abort (not spin).
    hal = package / "port/mphalport.c"
    hal.write_text(
        '#include <stddef.h>\n#include "py/mphal.h"\nvoid tny_mp_out(const char *text, size_t len);\n'
        "void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) { tny_mp_out(str, len); }\n"
    )
    util = package / "port/embed_util.c"
    text = util.read_text()
    text = text.replace("void nlr_jump_fail(void *val) {\n    for (;;) {\n    }\n}", "#include <stdlib.h>\nvoid nlr_jump_fail(void *val) {\n    abort();\n}")
    text = text.replace(
        "void __assert_func(const char *file, int line, const char *func, const char *expr) {\n    for (;;) {\n    }\n}",
        "void __assert_func(const char *file, int line, const char *func, const char *expr) {\n    abort();\n}",
    )
    util.write_text(text)
    stubs = work / "stubs.c"
    stubs.write_text(
        '#include "py/builtin.h"\n#include "py/mpprint.h"\n#include "py/runtime.h"\n#include <stddef.h>\n'
        "void tny_mp_out(const char *text, size_t len);\n"
        "static void out_strn(void *env, const char *str, size_t len) { (void)env; tny_mp_out(str, len); }\n"
        "const mp_print_t mp_sys_stdout_print = {NULL, out_strn};\n"
        "struct _mp_dummy_t { int unused; } mp_sys_stdout_obj;\n"
        "static mp_obj_t refuse_open(size_t n, const mp_obj_t *a, mp_map_t *kw) { (void)n; (void)a; (void)kw; mp_raise_OSError(1); }\n"
        "MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 0, refuse_open);\n"
    )
    return sorted(package.rglob("*.c")) + [stubs], [work, package, package / "port"]


def build_monty() -> Path:
    source = BUILD / "monty"
    head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if head != MONTY_COMMIT:
        raise SystemExit(f"Monty checkout {head} is not pinned v1.0.0 {MONTY_COMMIT}")
    target = BUILD / "monty-probe-target"
    subprocess.run(
        ["cargo", "build", "--release", "--locked" if (HERE / "monty_probe/Cargo.lock").exists() else "--offline", "-j3"],
        cwd=HERE / "monty_probe",
        env={**os.environ, "CARGO_TARGET_DIR": str(target)},
        check=True,
    )
    return target / "release/libtny_monty_probe.a"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sources", type=Path, required=True)
    parser.add_argument("--only", action="append")
    args = parser.parse_args()
    BUILD.mkdir(parents=True, exist_ok=True)
    for name, expected in PINS.items():
        if digest(args.sources / name) != expected:
            raise SystemExit(f"{name}: SHA256 mismatch")
    receipt_path = BUILD / "build.json"
    receipt = json.loads(receipt_path.read_text()) if receipt_path.exists() else {"binaries": {}, "commands": {}}
    receipt["pins"] = {**PINS, "monty_git": MONTY_COMMIT}
    receipt["compiler"] = subprocess.check_output([os.environ.get("CC", "cc"), "--version"], text=True).splitlines()[0]
    wanted = set(args.only or ["empty", "pocketpy", "micropython", "monty", "cpython"])
    if "empty" in wanted:
        link("empty", [PRIOR / "empty.c"], [], [], receipt)
    if "pocketpy" in wanted:
        link("pocketpy", [HERE / "pocketpy.c", args.sources / "pocketpy.c"], [args.sources], [], receipt)
    if "micropython" in wanted:
        sources, includes = build_micropython(args.sources)
        link("micropython", [HERE / "micropython.c", *sources], includes, [], receipt)
    if "monty" in wanted:
        archive = build_monty()
        link("monty", [HERE / "monty.c", archive], [], ["-lgcc_s"], receipt)
    if "cpython" in wanted:
        # The preserved PR #197 CPython arm (stock shared libpython + stdlib json),
        # used to execute CPython-arm trial programs. Requires Python 3.14 here.
        import sysconfig

        home = Path(sysconfig.get_config_var("LIBDIR")).parent.resolve()
        receipt["python_home"] = str(home)
        flags = [f'-DPYHOME="{home}"', f"-L{home / 'lib'}", f"-Wl,-rpath,{home / 'lib'}", "-lpython3.14", "-lutil"]
        include = [Path(sysconfig.get_config_var("INCLUDEPY"))]
        link("cpython", [PRIOR / "python.c"], include, flags, receipt)
        # Proposed production builtins/print/error policy, for trial execution.
        link("cpython_prod", [HERE / "cpython_prod.c"], include, flags, receipt)
    if "cpython_static" in wanted:
        cpython = args.sources / "Python-3.14.7"
        frozen = BUILD / "cpython-frozen"
        frozen.mkdir(exist_ok=True)
        for name, source in (("encodings", "__init__"), ("encodings.aliases", "aliases"), ("encodings.utf_8", "utf_8")):
            subprocess.run(
                [str(cpython / "Programs/_freeze_module"), name, str(cpython / f"Lib/encodings/{source}.py"),
                 str(frozen / f"frozen_{name.replace('.', '_')}.h")],
                check=True,
            )
        link(
            "cpython_static",
            [HERE / "cpython_static.c", cpython / "libpython3.14.a"],
            [cpython / "Include", cpython, frozen],
            ["-lutil"],
            receipt,
        )
    receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({k: v["bytes"] for k, v in receipt["binaries"].items()}))


if __name__ == "__main__":
    main()
