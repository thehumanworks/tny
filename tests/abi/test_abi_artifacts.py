#!/usr/bin/env python3
"""Build/run ABI1 and frozen ABI0 consumers and verify artifact identity."""
from __future__ import annotations

import ast
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build"
ABI0_COMMIT = "510a95c2ef89aa9ec02a66d8b0a5cadd953025a8"
ABI0_INCLUDE = BUILD / "compat0" / ABI0_COMMIT / "src" / "include"
IS_MAC = platform.system() == "Darwin"
PRIMARY = BUILD / "lib" / ("libtny.1.dylib" if IS_MAC else "libtny.so.1")
COMPAT0 = BUILD / "lib" / ("libtny.0.dylib" if IS_MAC else "libtny.so.0")


def run(command: list[str], *, expect: int = 0,
        env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False, env=env,
    )
    if completed.returncode != expect:
        raise AssertionError(
            f"command {command!r}: expected {expect}, got {completed.returncode}\n"
            f"stdout={completed.stdout}\nstderr={completed.stderr}"
        )
    return completed


def link(compiler: str, source: Path, output: Path, include: Path,
         library: Path) -> None:
    run([
        compiler, "-std=c++17" if source.suffix == ".cpp" else "-std=c11",
        "-Wall", "-Wextra", "-Werror", "-I", str(include), str(source),
        str(library), f"-Wl,-rpath,{library.parent}", "-o", str(output),
    ])


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def linux_fixture_key(machine: str) -> str:
    normalized = machine.lower()
    if normalized in {"x86_64", "amd64"}:
        return "linux-x86_64"
    if normalized in {"aarch64", "arm64"}:
        return "linux-aarch64"
    raise ValueError(f"unsupported Linux ABI fixture architecture: {machine}")


def minimum_glibc(version_info: str) -> str:
    versions = {
        tuple(int(part) for part in match.split("."))
        for match in re.findall(r"\bGLIBC_(\d+(?:\.\d+)+)\b", version_info)
    }
    if not versions:
        raise ValueError("consumer has no recorded GLIBC version requirement")
    value = max(versions)
    return ".".join(str(part) for part in value)


def linux_seed_fragment(*, key: str, binary: Path, compiler: str,
                        target: str, glibc: str, header: Path,
                        source: Path) -> dict[str, object]:
    return {
        "header_sha256": sha256(header),
        "sources": {source.name: sha256(source)},
        "binaries": {
            key: {
                "path": f"../bin/{key}/abi1-consumer",
                "sha256": sha256(binary),
                "compiler": compiler,
                "target": target,
                "minimum_os": "linux-glibc",
                "minimum_glibc": glibc,
                "header_sha256": sha256(header),
                "source_sha256": sha256(source),
            }
        },
    }


def seed_linux_fixture(compiler: str, key: str) -> Path:
    """Build a review artifact, never a passing-job replacement fixture."""
    minimum = ROOT / "tests/abi/fixtures/abi1-min"
    source = minimum / "consumer.c"
    header = minimum / "include/tny/tny.h"
    output = BUILD / "abi-fixture-seed" / key
    output.mkdir(parents=True, exist_ok=True)
    binary = output / "abi1-consumer"
    # Link the SONAME directly without RPATH/RUNPATH. Compatibility jobs load
    # the committed binary with a sanitized LD_LIBRARY_PATH pointing at lib1.
    run([
        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(minimum / "include"), str(source), str(PRIMARY),
        "-o", str(binary),
    ])
    dynamic = run(["readelf", "-d", "--wide", str(binary)]).stdout
    if "libtny.so.1" not in dynamic:
        raise AssertionError("seed consumer does not require libtny.so.1")
    if "(RPATH)" in dynamic or "(RUNPATH)" in dynamic:
        raise AssertionError("seed consumer contains a checkout-specific runtime path")
    versions = run(["readelf", "--version-info", "--wide", str(binary)]).stdout
    fragment = linux_seed_fragment(
        key=key,
        binary=binary,
        compiler=run([compiler, "--version"]).stdout.splitlines()[0],
        target=run([compiler, "-dumpmachine"]).stdout.strip(),
        glibc=minimum_glibc(versions),
        header=header,
        source=source,
    )
    (output / "manifest-entry.json").write_text(
        json.dumps(fragment, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return output


def compiled_candidate(compiler: str, baseline: dict[str, object]) -> dict[str, object]:
    candidate = json.loads(json.dumps(baseline))
    header = (ROOT / "include/tny/tny.h").read_text()
    macro_names = set(re.findall(r"^#define (TNY_[A-Z0-9_]+)\b", header, re.M))
    macro_names -= {"TNY_API", "TNY_CALL", "TNY_CALLBACK_NOEXCEPT"}
    if macro_names != set(baseline["constants"]):
        raise AssertionError(
            f"header constant inventory drift: added={sorted(macro_names - set(baseline['constants']))}, "
            f"missing={sorted(set(baseline['constants']) - macro_names)}")
    struct_names = set(re.findall(r"}\s+(tny_[a-z0-9_]+)\s*;", header))
    if struct_names != set(baseline["structs"]):
        raise AssertionError(
            f"header struct inventory drift: added={sorted(struct_names - set(baseline['structs']))}, "
            f"missing={sorted(set(baseline['structs']) - struct_names)}")
    lines = ["#include <stddef.h>", "#include <stdio.h>", '#include "tny/tny.h"',
             "int main(void) {"]
    for name in sorted(baseline["constants"]):
        lines.append(f'printf("C {name} %lld\\n", (long long)({name}));')
    for name, definition in sorted(baseline["structs"].items()):
        lines.append(
            f'printf("S {name} %zu %zu\\n", sizeof({name}), _Alignof({name}));')
        for field in definition["fields"]:
            field_name = field["name"]
            lines.append(
                f'printf("F {name} {field_name} %zu %zu\\n", '
                f'offsetof({name}, {field_name}), '
                f'sizeof((({name} *)0)->{field_name}));')
    lines.append("return 0; }")
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "probe.c"
        binary = Path(directory) / "probe"
        source.write_text("\n".join(lines), encoding="utf-8")
        run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "include"), str(source), "-o", str(binary)])
        output = run([str(binary)]).stdout
    fields: dict[tuple[str, str], tuple[int, int]] = {}
    for line in output.splitlines():
        parts = line.split()
        if parts[0] == "C":
            candidate["constants"][parts[1]] = int(parts[2])
        elif parts[0] == "S":
            candidate["structs"][parts[1]]["size"] = int(parts[2])
            candidate["structs"][parts[1]]["alignment"] = int(parts[3])
        elif parts[0] == "F":
            fields[(parts[1], parts[2])] = (int(parts[3]), int(parts[4]))
    for name, definition in candidate["structs"].items():
        for field in definition["fields"]:
            offset, size = fields[(name, field["name"])]
            field["offset"] = offset
            field["size"] = size
    return candidate


class AbiArtifactTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cc = shutil.which("cc")
        cls.cxx = shutil.which("c++")
        if not cls.cc or not cls.cxx:
            raise unittest.SkipTest("C and C++ compilers are required")
        if not PRIMARY.exists() or not COMPAT0.exists():
            raise unittest.SkipTest("run `make test-abi` to build both ABI majors")
        (BUILD / "abi").mkdir(parents=True, exist_ok=True)

    def test_current_c_and_cpp_consumers(self) -> None:
        with tempfile.TemporaryDirectory() as workspace:
            c_binary = BUILD / "abi" / "current-consumer"
            cpp_binary = BUILD / "abi" / "current-consumer-cpp"
            boundary_binary = BUILD / "abi" / "capacity-boundaries"
            link(self.cc, ROOT / "tests/abi/current_consumer.c", c_binary,
                 ROOT / "include", PRIMARY)
            link(self.cxx, ROOT / "tests/abi/current_consumer.cpp", cpp_binary,
                 ROOT / "include", PRIMARY)
            link(self.cc, ROOT / "tests/abi/capacity_boundaries.c",
                 boundary_binary, ROOT / "include", PRIMARY)
            run([str(c_binary), workspace, "http://127.0.0.1:1/v1"])
            run([str(cpp_binary)])
            run([str(boundary_binary)])

    def test_minimum_and_current_header_source_cross_matrix(self) -> None:
        minimum = ROOT / "tests/abi/fixtures/abi1-min"
        cases = (
            (minimum / "consumer.c", minimum / "include", self.cc, "min-min-c"),
            (minimum / "consumer.c", ROOT / "include", self.cc, "min-current-c"),
            (minimum / "consumer.cpp", minimum / "include", self.cxx, "min-min-cpp"),
            (minimum / "consumer.cpp", ROOT / "include", self.cxx, "min-current-cpp"),
            (ROOT / "tests/abi/current_consumer.c", minimum / "include",
             self.cc, "current-min-c"),
        )
        with tempfile.TemporaryDirectory() as workspace:
            for source, include, compiler, name in cases:
                binary = BUILD / "abi" / name
                link(compiler, source, binary, include, PRIMARY)
                if source.name == "current_consumer.c":
                    run([str(binary), workspace, "http://127.0.0.1:1/v1"])
                else:
                    run([str(binary)])

    def test_immutable_abi1_binary_runs_against_current_library(self) -> None:
        manifest_path = ROOT / "tests/abi/fixtures/abi1-min/manifest.json"
        manifest = json.loads(manifest_path.read_text())
        minimum = manifest_path.parent
        self.assertEqual(
            hashlib.sha256((minimum / "include/tny/tny.h").read_bytes()).hexdigest(),
            manifest["header_sha256"],
        )
        for name, expected in manifest["sources"].items():
            self.assertEqual(hashlib.sha256((minimum / name).read_bytes()).hexdigest(),
                             expected)
        key = "macos-arm64" if IS_MAC else f"linux-{platform.machine()}"
        entry = manifest["binaries"].get(key)
        if entry is None:
            if platform.system() == "Linux":
                key = linux_fixture_key(platform.machine())
                seed = seed_linux_fixture(self.cc, key)
                self.fail(
                    f"immutable ABI1 fixture missing for {key}; generated "
                    f"review seed at {seed.relative_to(ROOT)}")
            self.fail(f"immutable ABI1 fixture missing for {key}")
        binary = (manifest_path.parent / entry["path"]).resolve()
        self.assertEqual(hashlib.sha256(binary.read_bytes()).hexdigest(),
                         entry["sha256"])
        environment = dict(os.environ)
        if IS_MAC:
            environment["DYLD_LIBRARY_PATH"] = str(PRIMARY.parent)
        else:
            environment["LD_LIBRARY_PATH"] = str(PRIMARY.parent)
        run([str(binary)], env=environment)

    def test_linux_fixture_seed_helpers_are_deterministic(self) -> None:
        self.assertEqual(linux_fixture_key("x86_64"), "linux-x86_64")
        self.assertEqual(linux_fixture_key("AMD64"), "linux-x86_64")
        self.assertEqual(linux_fixture_key("aarch64"), "linux-aarch64")
        self.assertEqual(linux_fixture_key("arm64"), "linux-aarch64")
        with self.assertRaises(ValueError):
            linux_fixture_key("riscv64")
        version_info = "Name: GLIBC_2.2.5\nName: GLIBC_2.34\nName: GLIBC_2.17\n"
        self.assertEqual(minimum_glibc(version_info), "2.34")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "abi1-consumer"
            header = root / "tny.h"
            source = root / "consumer.c"
            binary.write_bytes(b"ELF-fixture")
            header.write_text("header\n")
            source.write_text("source\n")
            first = linux_seed_fragment(
                key="linux-x86_64", binary=binary, compiler="cc 1.0",
                target="x86_64-linux-gnu", glibc="2.34",
                header=header, source=source)
            second = linux_seed_fragment(
                key="linux-x86_64", binary=binary, compiler="cc 1.0",
                target="x86_64-linux-gnu", glibc="2.34",
                header=header, source=source)
        self.assertEqual(first, second)
        encoded = json.dumps(first, indent=2, sort_keys=True) + "\n"
        self.assertNotIn(os.fspath(ROOT), encoded)
        self.assertEqual(first["binaries"]["linux-x86_64"]["minimum_glibc"],
                         "2.34")

    def test_frozen_abi0_consumers_and_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as workspace:
            c_binary = BUILD / "abi" / "compat0-consumer"
            cpp_binary = BUILD / "abi" / "compat0-consumer-cpp"
            link(self.cc, ROOT / "tests/abi/compat0_consumer.c", c_binary,
                 ABI0_INCLUDE, COMPAT0)
            link(self.cxx, ROOT / "tests/abi/compat0_consumer.cpp", cpp_binary,
                 ABI0_INCLUDE, COMPAT0)
            run([str(c_binary), workspace, "http://127.0.0.1:1/v1"])
            run([str(cpp_binary)])
            metadata = {
                "source_commit": ABI0_COMMIT,
                "compiler": run([self.cc, "--version"]).stdout.splitlines()[0],
                "target": run([self.cc, "-dumpmachine"]).stdout.strip(),
                "sha256": hashlib.sha256(c_binary.read_bytes()).hexdigest(),
            }
            (BUILD / "abi" / "compat0-consumer.json").write_text(
                json.dumps(metadata, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )

    def test_header_arity_collision_is_rejected(self) -> None:
        source = ROOT / "tests/abi/incompatible_legacy_arity.c"
        completed = subprocess.run(
            [self.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "include"), "-fsyntax-only", str(source)],
            cwd=ROOT, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("tny_runtime_options_init", completed.stderr)

    def test_python_abi1_consumers_never_use_legacy_arity(self) -> None:
        arities = {
            "tny_runtime_options_init": 2,
            "tny_runtime_options_v1_init": 2,
            "tny_host_services_v1_init": 2,
            "tny_tool_spec_v1_init": 2,
            "tny_tool_result_v1_init": 2,
            "tny_capabilities_init": 2,
            "tny_capabilities_v1_init": 2,
            "tny_runtime_create": 4,
            "tny_runtime_create_v1": 4,
            "tny_runtime_get_capabilities": 3,
            "tny_runtime_get_capabilities_v1": 3,
            "tny_event_view_init": 2,
            "tny_event_read": 3,
        }
        roots = (ROOT / "tests/integration", ROOT / "sdk/conformance/adapters")
        failures: list[str] = []
        for root in roots:
            for path in sorted(root.glob("*.py")):
                tree = ast.parse(path.read_text(), filename=str(path))
                for node in ast.walk(tree):
                    if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute):
                        continue
                    expected = arities.get(node.func.attr)
                    if expected is not None and len(node.args) != expected:
                        failures.append(
                            f"{path.relative_to(ROOT)}:{node.lineno} "
                            f"{node.func.attr} has {len(node.args)} args, want {expected}")
        self.assertEqual(failures, [])

    def test_every_export_and_callback_signature_in_c_and_cpp(self) -> None:
        manifest = json.loads((ROOT / "abi/signatures-v1.json").read_text())
        header = (ROOT / "include/tny/tny.h").read_text()

        def normalize(value: str) -> str:
            value = re.sub(r"\s+", " ", value.strip())
            return re.sub(r"\s*([(),*])\s*", r"\1", value)

        observed = {
            match.group(2): normalize(
                f"{match.group(1)} {match.group(2)}({match.group(3)})")
            for match in re.finditer(
                r"^TNY_API\s+(.+?)\s+TNY_CALL\s+(tny_[a-z0-9_]+)"
                r"\s*\((.*?)\);", header, re.M | re.S)
        }
        self.assertEqual(observed, manifest["exports"])
        c_lines = ['#include "tny/tny.h"']
        cpp_lines = ['#include "tny/tny.h"', "#include <type_traits>"]
        for index, (name, prototype) in enumerate(sorted(observed.items())):
            match = re.fullmatch(rf"(.+?) {name}\((.*)\)", prototype)
            self.assertIsNotNone(match)
            returns, parameters = match.groups()
            c_lines.append(
                f"{returns} (TNY_CALL *signature_{index})({parameters}) = {name};")
            cpp_lines.append(
                f"using signature_{index} = {returns} (TNY_CALL *)({parameters});")
            cpp_lines.append(
                f"static_assert(std::is_same_v<decltype(&{name}), signature_{index}>);")
        for index, (name, expected) in enumerate(
                sorted(manifest["callbacks"].items())):
            c_lines.append(
                f'_Static_assert(__builtin_types_compatible_p({name}, {expected}), '
                f'"{name}");')
            cpp_lines.append(f"using callback_{index} = {expected} noexcept;")
            cpp_lines.append(
                f"static_assert(std::is_same_v<{name}, callback_{index}>);")
        c_lines.append("int main(void) { return 0; }")
        cpp_lines.append("int main() { return 0; }")
        with tempfile.TemporaryDirectory() as directory:
            c_path = Path(directory) / "signatures.c"
            cpp_path = Path(directory) / "signatures.cpp"
            c_path.write_text("\n".join(c_lines))
            cpp_path.write_text("\n".join(cpp_lines))
            run([self.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "include"), "-fsyntax-only", str(c_path)])
            run([self.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "include"), "-fsyntax-only", str(cpp_path)])

    def test_exact_artifact_ids_and_exports(self) -> None:
        baseline = json.loads((ROOT / "abi/baseline-v1.json").read_text())
        candidate = compiled_candidate(self.cc, baseline)
        expected = set(baseline["exports"])
        if IS_MAC:
            identity = run(["otool", "-D", str(PRIMARY)]).stdout
            self.assertIn("@rpath/libtny.1.dylib", identity)
            load = run(["otool", "-L", str(PRIMARY)]).stdout
            self.assertIn("compatibility version 1.0.0", load)
            self.assertIn("current version 1.0.0", load)
            match = re.search(r"current version (\d+\.\d+\.\d+)", load)
            self.assertIsNotNone(match)
            candidate["artifacts"]["macos"]["current_version"] = match.group(1)
            compat_identity = run(["otool", "-D", str(COMPAT0)]).stdout
            self.assertIn("@rpath/libtny.0.dylib", compat_identity)
            symbols = run(["nm", "-gU", str(PRIMARY)]).stdout.splitlines()
            actual = {line.split()[-1].removeprefix("_") for line in symbols
                      if line.split()[-1].startswith("_tny_")}
        else:
            dynamic = run(["readelf", "-d", str(PRIMARY)]).stdout
            self.assertIn("Library soname: [libtny.so.1]", dynamic)
            compat_dynamic = run(["readelf", "-d", str(COMPAT0)]).stdout
            self.assertIn("Library soname: [libtny.so.0]", compat_dynamic)
            symbols = run(["readelf", "--dyn-syms", "--wide", str(PRIMARY)]).stdout
            actual = set()
            for line in symbols.splitlines():
                if " tny_" in line:
                    actual.add(line.split()[-1].split("@@", 1)[0])
                    self.assertIn("@@LIBTNY_1.0", line)
        self.assertEqual(actual, expected)
        candidate["exports"] = {name: "LIBTNY_1.0" for name in sorted(actual)}
        (BUILD / "abi" / "libtny-v1-current.json").write_text(
            json.dumps(candidate, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    def test_installed_pkg_config_roots_do_not_collide(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / "prefix"
            run(["make", "install-lib", f"PREFIX={prefix}",
                 "TNY_VERSION=1.0.0"])
            current_pc = prefix / "lib/pkgconfig/libtny.pc"
            compat_pc = prefix / "lib/pkgconfig/libtny-0.pc"
            self.assertIn("includedir=${prefix}/include\n", current_pc.read_text())
            self.assertIn("Cflags: -I${includedir}\n", current_pc.read_text())
            self.assertIn("includedir=${prefix}/include/tny-0\n", compat_pc.read_text())
            self.assertIn("Cflags: -I${includedir}\n", compat_pc.read_text())
            current_header = prefix / "include/tny/tny.h"
            compat_header = prefix / "include/tny-0/tny/tny.h"
            self.assertIn("#define TNY_ABI_MAJOR 1u", current_header.read_text())
            self.assertIn("#define TNY_ABI_MAJOR 0u", compat_header.read_text())
            self.assertNotEqual(current_header.read_bytes(), compat_header.read_bytes())

    def test_verified_archive_fallback_does_not_require_git_history(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "compat0.tar"
            archived = subprocess.run(
                ["git", "archive", ABI0_COMMIT], cwd=ROOT,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True,
            )
            archive.write_bytes(archived.stdout)
            build = root / "build"
            stamp = build / "compat0" / ABI0_COMMIT / ".source-verified"
            run(["make", f"BUILD={build}", "GIT=false",
                 f"ABI0_COMPAT_ARCHIVE={archive}", str(stamp)])
            self.assertTrue(stamp.exists())


if __name__ == "__main__":
    unittest.main()
