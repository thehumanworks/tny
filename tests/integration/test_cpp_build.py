#!/usr/bin/env python3
"""Exercise the real Makefile in an isolated, disposable mixed-language tree.

This is build/quality evidence, not parser behavioral evidence. C-only syntax
and C++20/STL/throw-catch code make driver, macro and exception mistakes fail.
The negative fixtures use the shipped formatter and enabled analyzer settings.
"""

from __future__ import annotations

import os
import runpy
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
CPP_DIRS = ("util", "json", "net", "backends/openai", "core", "lib")


class CppBuild(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tny-cpp-build-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name in ("Makefile", ".clang-format", ".clang-tidy"):
            shutil.copyfile(ROOT / name, self.root / name)
        (self.root / "scripts").mkdir()
        shutil.copyfile(ROOT / "scripts/tidy_cpp.py", self.root / "scripts/tidy_cpp.py")
        self.run_command(["git", "init", "-q"])
        self.write(".gitignore", "build/\n")
        self.write(
            "third_party/yyjson/version", "this vendor metadata is not a C++ header\n"
        )
        self.write(
            "third_party/yyjson/yyjson.h", "#define TNY_FIXTURE_VENDOR_VALUE 0\n"
        )
        self.write("tests/unit.cpp", "int cpp_unit() { return 1; }\n")
        self.write(
            "src/main.c",
            "int tny_probe(void);\nint main(void) { return tny_probe() != 42; }\n",
        )
        self.write("tests/test_main.c", (self.root / "src/main.c").read_text())
        self.write(
            "src/util/probe.c",
            "int c_value(void);\nint c_value(void) { return _Generic(1, int: 40); }\n",
        )
        self.write(
            "src/util/probe.hpp",
            "#ifndef TNY_FIXTURE_PROBE_HPP\n#define TNY_FIXTURE_PROBE_HPP\ninline int increment(int n) { return n + 2; }\n#endif\n",
        )
        self.write(
            "src/util/probe.cpp",
            """#include "probe.hpp"
#include <vector>
#include <span>
#include <cstdlib>
#include <version>
#include "yyjson.h"
extern "C" int c_value(void);
extern "C" __attribute__((visibility("default"))) int tny_probe(void) {
    try {
        std::vector<int> values(2, c_value());
        std::span<const int> view(values);
        throw increment(view.front()) + TNY_FIXTURE_VENDOR_VALUE;
    } catch (int result) { return result; } catch (...) { return -1; }
}
""",
        )
        for directory in CPP_DIRS[1:]:
            symbol = directory.replace("/", "_")
            self.write(
                f"src/{directory}/discovery.cpp",
                f"int discovery_{symbol}() {{ return 1; }}\n",
            )
        for name in ("host_services", "custom_tools"):
            self.write(f"src/lib/{name}.c", f"typedef int {name}_fixture;\n")
        self.write(
            "src/util/alloc_override.h",
            "#define malloc(...) forbidden_allocator_macro\n#define free(...) forbidden_allocator_macro\n",
        )
        self.write("abi/libtny.exports.macos", "_tny_probe\n")
        self.write("abi/libtny.map", "TNY_1 { global: tny_probe; local: *; };\n")
        self.write("src/wasm/pre_node.js", "")
        self.write("src/wasm/pre_web.js", "")
        self.write("tests/fuzz/fuzz_libtny.c", "int main(void) { return 0; }\n")
        self.write(
            "tests/fuzz/fuzz_parsers.cpp",
            """#include <cstddef>
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *, size_t) { return 0; }
#ifdef TNY_FUZZ_STANDALONE
int main() { return 0; }
#endif
""",
        )
        self.write("tests/fuzz/parser-corpus/seed", "data: fixture\n\n")
        # Native transport seams and vendors are outside this fixture. The
        # production discovery expression for private C++ stays unmodified.
        self.make_args = [
            "SRC_NATIVE=",
            "TP=",
            "TP_WASM=",
            "SRC_WASM_ONLY=",
            "TNY_VERSION=1.0.0",
            "LIBTNY_MACH_CURRENT_VERSION=1.0.0",
        ]

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    @staticmethod
    def child_environment():
        environment = dict(os.environ)
        # Python closes the parent's jobserver fds. These independent fixture
        # builds use explicit arguments, not stale parent make orchestration.
        for name in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL"):
            environment.pop(name, None)
        return environment

    def run_command(self, argv, **kwargs):
        kwargs.setdefault("env", self.child_environment())
        run = subprocess.run(
            argv, cwd=self.root, text=True, capture_output=True, **kwargs
        )
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        return run

    def make(self, *args, succeeds=True):
        run = subprocess.run(
            ["make", "--no-print-directory", *self.make_args, *args],
            cwd=self.root,
            env=self.child_environment(),
            text=True,
            capture_output=True,
        )
        if succeeds:
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        else:
            self.assertNotEqual(run.returncode, 0, run.stdout + run.stderr)
        return (run.stdout + run.stderr).replace("\\\n", " ").replace("\t", " ")

    def test_isolated_child_drops_unavailable_parent_jobserver(self):
        inherited = {
            "MAKEFLAGS": "-j --jobserver-fds=987,988",
            "MFLAGS": "-j --jobserver-fds=987,988",
            "MAKELEVEL": "2",
        }
        with patch.dict(os.environ, inherited):
            run = self.run_command(
                ["make", "--no-print-directory", "-s", "-f", "-", "probe"],
                input="probe:\n\t@echo child\n",
            )
        self.assertEqual(run.stdout, "child\n")
        self.assertEqual(run.stderr, "")

    def test_native_compile_link_and_exception_catching(self):
        self.make("-j2", "release", "debug", "test-parser-fuzz-smoke", "SANITIZE=0")
        self.run_command([str(self.root / "build/tny")])
        self.run_command([str(self.root / "build/tny-test")])
        for directory in CPP_DIRS:
            source = "probe" if directory == "util" else "discovery"
            for mode in ("rel", "dbg"):
                self.assertTrue(
                    (
                        self.root / f"build/{mode}/src/{directory}/{source}.cpp.o"
                    ).is_file()
                )
        self.assertTrue((self.root / "build/rel/src/util/probe.o").is_file())
        self.assertTrue((self.root / "build/dbg/tests/unit.cpp.o").is_file())
        header = self.root / "src/util/probe.hpp"
        original = header.read_text()
        time.sleep(1.1)  # GNU make 3.81 compares whole-second mtimes.
        header.write_text(original.replace("n + 2", "n + 3"))
        self.make("release", "SANITIZE=0")
        changed = subprocess.run([str(self.root / "build/tny")], check=False)
        self.assertEqual(changed.returncode, 1)
        time.sleep(1.1)
        header.write_text(original)
        self.make("release", "SANITIZE=0")
        self.run_command([str(self.root / "build/tny")])
        self.make("warn-strict", "TIDY_SRC=src/util/probe.c src/util/probe.cpp")
        self.make("tidy", "TIDY_SRC=src/util/probe.cpp")
        supported = self.run_command(
            [
                "make",
                "-s",
                "-f",
                "Makefile",
                "-f",
                "-",
                *self.make_args,
                "shared-support",
            ],
            input="shared-support:\n\t@printf '%s' '$(LIBTNY_SHARED_SUPPORTED)'\n",
        ).stdout
        if supported == "1":
            self.make(
                "lib-shared-active", "lib-shared-fault", "lib-shared-fault-sanitize"
            )
            # A C executable consumes the actual hidden-visibility C++ library.
            suffix = "dylib" if sys.platform == "darwin" else "so"
            self.run_command(
                [
                    *shlex.split(os.environ.get("CC", "cc")),
                    "-std=c11",
                    "src/main.c",
                    "-Lbuild/lib",
                    "-ltny",
                    f"-Wl,-rpath,{self.root / 'build/lib'}",
                    "-o",
                    "build/consumer",
                ]
            )
            self.assertTrue((self.root / f"build/lib/libtny.{suffix}").exists())
            self.run_command([str(self.root / "build/consumer")])

    def test_lto_exempt_cpp_object_links_into_the_lto_executable(self):
        # ADR 0122: a listed private C++ module compiles to a native object
        # while every other object and the executable link keep LTO. The
        # exception path through that native object still runs.
        exempt = "LTO_EXEMPT_CPP=src/util/probe.cpp"
        self.make("-j2", "release", "SANITIZE=0", exempt)
        self.run_command([str(self.root / "build/tny")])
        output = self.make(
            "-n", "-B", "release", exempt, "CC=probe-cc", "CXX=probe-cxx"
        )

        def options(target):
            matches = [line for line in output.splitlines() if f" -o {target} " in line]
            self.assertEqual(len(matches), 1, target)
            return shlex.split(matches[0])

        native = options("build/rel/src/util/probe.cpp.o")
        self.assertIn("-fno-lto", native)
        self.assertFalse([option for option in native if option.startswith("-flto")])
        for required in ("-std=c++20", "-fexceptions", "-fno-rtti", "-Werror"):
            self.assertIn(required, native)
        for target in (
            "build/rel/src/json/discovery.cpp.o",
            "build/rel/src/util/probe.o",
            "build/tny",
        ):
            with self.subTest(target=target):
                kept = options(target)
                self.assertTrue(
                    [option for option in kept if option.startswith("-flto")]
                )
                self.assertNotIn("-fno-lto", kept)

    def test_windows_request_and_runner_lto_exemption_is_narrow(self):
        self.write(
            "src/backends/openai/responses.cpp",
            "int response_fixture() { return 0; }\n",
        )
        self.write("src/core/runner.cpp", "int runner_fixture() { return 0; }\n")
        for windows in (0, 1):
            output = self.make(
                "-n", "-B", "release", f"WINDOWS={windows}", "CC=echo", "CXX=echo"
            )
            commands = [
                shlex.split(line) for line in output.splitlines() if " -o " in line
            ]

            def options(suffix):
                return next(
                    command
                    for command in commands
                    if command[command.index("-o") + 1].endswith(suffix)
                )

            for suffix in (
                "src/backends/openai/responses.cpp.o",
                "src/core/runner.cpp.o",
            ):
                response = options(suffix)
                self.assertIn("-Os", response)
                self.assertIn("-Werror", response)
                self.assertIn("-fexceptions", response)
                self.assertEqual("-fno-lto" in response, bool(windows))
                self.assertEqual("-flto=auto" in response, not windows)
            for suffix in (
                "src/util/probe.o",
                "src/util/probe.cpp.o",
                "tny.exe" if windows else "tny",
            ):
                self.assertIn("-flto=auto", options(suffix))
                self.assertNotIn("-fno-lto", options(suffix))
        for lane in ("dbg", "fault-pic", "fault-san-pic"):
            output = self.make(
                "-n",
                f"build/{lane}/src/backends/openai/responses.cpp.o",
                "WINDOWS=1",
                "CC=echo",
                "CXX=echo",
            )
            self.assertNotIn("-fno-lto", output)

    def test_darwin_numeric_version_survives_nested_make_environment(self):
        self.make_args = [
            arg
            for arg in self.make_args
            if not arg.startswith("LIBTNY_MACH_CURRENT_VERSION=")
        ]
        with patch.dict(os.environ, {"LIBTNY_MACH_CURRENT_VERSION": "1.2.3"}):
            output = self.make(
                "-n",
                "-B",
                "lib-shared-active",
                "UNAME_S=Darwin",
                "TNY_VERSION=abc1234",
                "CC=echo",
                "CXX=echo",
            )
        self.assertIn("-Wl,-current_version,1.2.3", output)

    def test_gitless_quality_discovery_keeps_first_party_sources(self):
        self.write("scripts/discovery.sh", "#!/bin/sh\necho discovery\n")
        self.write("build/ignored.cpp", "invalid generated source\n")
        self.write("third_party/ignored.hpp", "vendor source\n")
        run = self.run_command(
            [
                "make",
                "-s",
                "-f",
                "Makefile",
                "-f",
                "-",
                *self.make_args,
                "GIT=tny-fixture-unavailable-git",
                "file-inventory",
            ],
            input="file-inventory:\n\t@printf '%s\\n' '$(FMT_SRC)' '$(SH_SRC)'\n",
        )
        self.assertEqual(run.stderr, "")
        paths = run.stdout.split()
        self.assertIn("src/util/probe.hpp", paths)
        self.assertIn("src/util/probe.cpp", paths)
        self.assertIn("scripts/discovery.sh", paths)
        self.assertNotIn("build/ignored.cpp", paths)
        self.assertNotIn("third_party/ignored.hpp", paths)

    def test_release_archive_does_not_read_missing_unit_test_inventory(self):
        (self.root / "tests/test_main.c").unlink()
        run = self.run_command(
            ["make", "--no-print-directory", *self.make_args, "release", "SANITIZE=0"]
        )
        self.assertEqual(run.stderr, "")
        self.run_command([str(self.root / "build/tny")])

    def test_wasm_exception_catching(self):
        emcc = shlex.split(os.environ.get("EMCC", "emcc"))[0]
        if not shutil.which(emcc):
            self.skipTest("Emscripten runtime is exercised by the wasm CI lane")
        self.make("-j2", "wasm", "wasm-web")
        self.run_command(["node", "build/wasm/tny.js"])
        self.assertTrue((self.root / "build/wasm/tny-web.wasm").is_file())

    def test_every_lane_discovers_cpp_and_uses_matching_drivers(self):
        lanes = {
            "rel": "-flto",
            "dbg": "-fsanitize=address,undefined",
            "pic": "-fvisibility=hidden",
            "fault-pic": "-DTNY_ALLOC_TESTING=1",
            "fault-san-pic": "-fsanitize=address,undefined",
            "tsan-pic": "-fsanitize=thread",
            "fuzz-libfuzzer/obj": "-fsanitize=fuzzer-no-link,address,undefined",
            "wasm/obj": "-fexceptions",
        }
        args = [
            "CC=probe-cc",
            "CXX=probe-cxx",
            "FUZZ_CC=probe-fuzz-cc",
            "FUZZ_CXX=probe-fuzz-cxx",
            "EMCC=probe-emcc",
            "EMCXX=probe-emcxx",
        ]
        for lane, flag in lanes.items():
            with self.subTest(lane=lane):
                output = self.make("-n", f"build/{lane}/src/util/probe.cpp.o", *args)
                command = next(
                    line for line in output.splitlines() if " -c -o " in line
                )
                self.assertIn("cxx ", command)
                for required in (
                    "-std=c++20",
                    "-fexceptions",
                    "-fno-rtti",
                    "-MMD",
                    "-Werror",
                    flag,
                ):
                    self.assertIn(required, command)
                self.assertNotIn("-std=c11", command)
                self.assertNotIn("alloc_override.h", command)
                c_command = self.make("-n", f"build/{lane}/src/util/probe.o", *args)
                self.assertIn("-std=c11", c_command)
                self.assertNotIn("-std=c++20", c_command)
        for target, driver in (
            ("wasm", "probe-emcxx"),
            ("wasm-web", "probe-emcxx"),
            ("test-parser-fuzz", "probe-fuzz-cxx"),
            ("test-libtny-fuzz", "probe-fuzz-cxx"),
            ("lib-shared-tsan", "probe-cxx"),
        ):
            with self.subTest(target=target):
                output = self.make(
                    "-n", target, "UNAME_S=Linux", "UNAME_M=x86_64", *args
                )
                links = [
                    line
                    for line in output.splitlines()
                    if line.startswith(driver + " ")
                    and " -o " in line
                    and " -c " not in line
                ]
                self.assertTrue(links, output)
                self.assertIn("src/util/probe.cpp.o", " ".join(links))
                if target.startswith("wasm"):
                    self.assertIn("-fexceptions", " ".join(links))
                if "fuzz" in target:
                    for bound in (
                        "-max_total_time=30",
                        "-runs=10000",
                        "-rss_limit_mb=1024",
                    ):
                        self.assertIn(bound, output)

    def test_quality_discovers_cpp_without_source_overrides(self):
        for target in ("tidy", "warn-strict", "analyze"):
            with self.subTest(target=target):
                output = self.make("-n", target)
                for directory in CPP_DIRS:
                    source = "probe" if directory == "util" else "discovery"
                    self.assertIn(f"src/{directory}/{source}.cpp", output)
                self.assertIn("src/util/probe.c", output)
                self.assertIn("-std=c11", output)
                self.assertIn("-std=c++20", output)

    def test_untracked_cpp_and_hpp_format_violations_fail_gate(self):
        # Use the exact production subgate of format-check, with automatic
        # Git discovery, not a stand-in invocation or a source-list override.
        self.run_command(
            [
                *shlex.split(os.environ.get("CLANG_FORMAT", "clang-format")),
                "-i",
                *[
                    str(p.relative_to(self.root))
                    for p in (self.root / "src").rglob("*")
                    if p.suffix in (".c", ".cpp", ".h", ".hpp")
                ],
                "tests/test_main.c",
                "tests/unit.cpp",
                "tests/fuzz/fuzz_libtny.c",
                "tests/fuzz/fuzz_parsers.cpp",
            ]
        )
        self.make("format-c-check")
        for name in ("src/util/new.cpp", "src/util/new.hpp"):
            with self.subTest(source=name):
                self.write(name, "inline int bad( ){return 1;}\n")
                output = self.make("format-c-check", succeeds=False)
                self.assertIn(name, output)
                self.assertIn("[-Wclang-format-violations]", output)
                (self.root / name).unlink()
        self.make("format-c-check")

    def test_cpp_header_advice_preserves_c_abi_but_checks_private_headers(self):
        source = "src/util/header_check.cpp"
        self.write("src/util/interface.h", "enum AbiKind { ABI_ONE, ABI_TWO };\n")
        self.write(
            source,
            '#include <vector>\n#include <cstdlib>\n#include "interface.h"\nint probe() { return ABI_ONE; }\n',
        )
        self.make("tidy", f"TIDY_SRC={source}")
        self.write(
            "src/util/private.hpp", "enum PrivateKind { PRIVATE_ONE, PRIVATE_TWO };\n"
        )
        self.write(
            source,
            '#include <vector>\n#include "interface.h"\n#include "private.hpp"\nint probe() { return ABI_ONE; }\n',
        )
        output = self.make("tidy", f"TIDY_SRC={source}", succeeds=False)
        self.assertIn("performance-enum-size", output)
        self.assertIn("private.hpp", output)
        (self.root / "src/util/private.hpp").write_text("")
        self.write("src/util/interface.h", "this is invalid C and C++;\n")
        output = self.make("tidy", f"TIDY_SRC={source}", succeeds=False)
        self.assertIn("clang-diagnostic-error", output)
        self.assertIn("interface.h", output)

    def test_enabled_cpp_analyzer_diagnostic_fails_gate(self):
        source = "src/util/analyzer.cpp"
        self.write(source, "int probe() { return 1; }\n")
        self.make("tidy", f"TIDY_SRC={source}")
        self.write(
            source, "int probe() { int *p = new int(1); delete p; return *p; }\n"
        )
        output = self.make("tidy", f"TIDY_SRC={source}", succeeds=False)
        self.assertIn("clang-analyzer-cplusplus.NewDelete", output)
        self.assertIn("error:", output)
        self.write(source, "int probe() { return 1; }\n")
        self.make("tidy", f"TIDY_SRC={source}")


class NativeMutationEnvironment(unittest.TestCase):
    def test_runner_uses_allowlist_and_throwaway_home(self):
        namespace = runpy.run_path(str(ROOT / "tests/mutation/native_ownership.py"))
        with tempfile.TemporaryDirectory() as directory:
            with patch.dict(
                os.environ,
                {
                    "TNY_TOOLS": "none",
                    "TNY_PROVIDER_EXTRAS": "0",
                    "OPENAI_API_KEY": "dummy",
                    "TNY_TEST_ALLOC_SCOPE": "inherited",
                    "MAKEFLAGS": "inherited",
                },
            ):
                environment = namespace["child_environment"](Path(directory))
            for key in (
                "TNY_TOOLS",
                "TNY_PROVIDER_EXTRAS",
                "OPENAI_API_KEY",
                "TNY_TEST_ALLOC_SCOPE",
                "MAKEFLAGS",
            ):
                self.assertNotIn(key, environment)
            self.assertEqual(environment["HOME"], str(Path(directory) / "home"))
            self.assertTrue(Path(environment["TMPDIR"]).is_dir())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
