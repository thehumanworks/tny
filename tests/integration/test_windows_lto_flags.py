#!/usr/bin/env python3
"""Keep MSYS executable LTO strict without unsupported yyjson DLL annotations."""

import ast
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class WindowsLtoFlags(unittest.TestCase):
    def test_native_fault_links_retain_the_captured_release_flags(self):
        for filename in ("test_image_workflow.py", "test_jobs_msys.py"):
            with self.subTest(fixture=filename):
                tree = ast.parse((ROOT / "tests/integration" / filename).read_text())
                links = []
                for node in ast.walk(tree):
                    if not isinstance(node, ast.List):
                        continue
                    names = [
                        elt.value.id
                        for elt in node.elts
                        if isinstance(elt, ast.Starred)
                        and isinstance(elt.value, ast.Name)
                    ]
                    constants = [
                        elt.value for elt in node.elts if isinstance(elt, ast.Constant)
                    ]
                    if "cxx" in names and "-o" in constants and "-c" not in constants:
                        links.append(names)
                self.assertEqual(len(links), 1)
                self.assertEqual(links[0][:3], ["cxx", "cxx_flags", "lto"])

    def test_linux_clang_size_policy_uses_selected_compiler_and_native_recipes(self):
        with tempfile.TemporaryDirectory(prefix="tny-compiler-policy-") as tmp:
            compiler = Path(tmp) / "compiler.py"
            compiler.write_text("import sys\nprint(sys.argv[1])\n")
            # Linux native executables omit the frame pointer (ADR 0111);
            # Linux Clang adds -Oz after it (ADR 0102); other hosts get neither.
            omit = "-fomit-frame-pointer -momit-leaf-frame-pointer"
            for platform, vendor, expected, level in (
                ("Linux", "clang", f"{omit} -Oz", "-Oz"),
                ("Linux", "gcc", omit, "-Os"),
                ("Darwin", "clang", "", "-Os"),
                ("MSYS_NT-10.0", "clang", "", "-Os"),
            ):
                with self.subTest(platform=platform, vendor=vendor):
                    cc = shlex.join([sys.executable, str(compiler), vendor])
                    args = [f"CC={cc}", f"UNAME_S={platform}", "UNAME_M=x86_64"]
                    flags = subprocess.run(
                        [
                            "make",
                            "-s",
                            "-f",
                            "Makefile",
                            "-f",
                            "-",
                            "size-policy",
                            *args,
                        ],
                        input=".PHONY: size-policy\nsize-policy:\n"
                        "\t@printf '%s\\n' '$(REL_SIZE_OPT)' '$(PIC_CFLAGS)' "
                        "'$(DBG_CFLAGS)' '$(WASM_CFLAGS)' "
                        "'$(OBJ_REL)/src/core/image_service.o' '$(DICTATION_FIXTURE_OBJ)' "
                        "'$(BIN)' '$(DICTATION_FIXTURE)'\n",
                        cwd=ROOT,
                        capture_output=True,
                        text=True,
                        check=True,
                    ).stdout.splitlines()
                    self.assertEqual(flags[0], expected)
                    for other in flags[1:4]:
                        self.assertNotIn("-Oz", other.split())
                        self.assertNotIn("-fomit-frame-pointer", other.split())
                    commands = subprocess.run(
                        ["make", "-n", "-B", "release", "dictation-fixture", *args],
                        cwd=ROOT,
                        capture_output=True,
                        text=True,
                        check=True,
                    ).stdout.splitlines()
                    for target in flags[4:]:
                        matches = [
                            line for line in commands if f" -o {target} " in line
                        ]
                        self.assertEqual(len(matches), 1, target)
                        options = shlex.split(matches[0])
                        optimization = [opt for opt in options if opt.startswith("-O")]
                        self.assertEqual(optimization[-1], level)
                        self.assertEqual(
                            "-fomit-frame-pointer" in options, platform == "Linux"
                        )

    def flags(self, windows, *extra):
        result = subprocess.run(
            [
                "make",
                "-s",
                "-f",
                "Makefile",
                "-f",
                "-",
                "flags-for-test",
                f"WINDOWS={int(windows)}",
                "UNAME_S=MSYS_NT-10.0" if windows else "UNAME_S=Linux",
                "UNAME_M=x86_64",
                *extra,
            ],
            input=".PHONY: flags-for-test\nflags-for-test:\n"
            "\t@printf '%s\\n' '$(DEFS)' '$(REL_CFLAGS)' '$(REL_LTO)'\n",
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        return result.stdout.splitlines()

    def expected_lto(self, windows, *extra):
        version = subprocess.run(
            [
                "make",
                "-s",
                "-f",
                "Makefile",
                "-f",
                "-",
                "cc-version-for-test",
                f"WINDOWS={int(windows)}",
                "UNAME_S=MSYS_NT-10.0" if windows else "UNAME_S=Linux",
                "UNAME_M=x86_64",
                *extra,
            ],
            input=".PHONY: cc-version-for-test\ncc-version-for-test:\n"
            "\t@$(CC) --version\n",
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        text = version.stdout + version.stderr
        return "-flto" if "clang" in text else "-flto=auto"

    def test_msys_uses_supported_static_annotation_override(self):
        definitions, flags, lto = self.flags(True)
        self.assertIn("-Dyyjson_api=", definitions.split())
        self.assertIn("-Werror", flags.split())
        self.assertNotIn("-Wno-attributes", flags.split())
        self.assertEqual(lto, self.expected_lto(True))

    def test_elf_keeps_vendor_visibility_and_lto(self):
        definitions, flags, lto = self.flags(False)
        self.assertNotIn("-Dyyjson_api=", definitions.split())
        self.assertIn("-Werror", flags.split())
        self.assertEqual(lto, self.expected_lto(False))

    def test_gcc_schedules_lto_automatically_and_clang_keeps_generic_lto(self):
        with tempfile.TemporaryDirectory(prefix="tny-lto-vendor-") as tmp:
            compiler = Path(tmp) / "compiler.py"
            compiler.write_text(
                "import sys\n"
                "print('clang version 18.0.0' if 'clang' in sys.argv[1] "
                "else 'gcc (GCC) 14.2.0')\n"
            )
            for vendor, expected in (("clang", "-flto"), ("gcc", "-flto=auto")):
                with self.subTest(vendor=vendor):
                    cc = shlex.join([sys.executable, str(compiler), vendor])
                    _, _, lto = self.flags(False, f"CC={cc}")
                    self.assertEqual(lto, expected)

    def test_json_inlining_stays_out_of_shared_debug_and_wasm_flags(self):
        result = subprocess.run(
            ["make", "-s", "-f", "Makefile", "-f", "-", "inline-flags-for-test"],
            input=".PHONY: inline-flags-for-test\ninline-flags-for-test:\n"
            "\t@printf '%s\\n' '$(REL_CFLAGS) $(REL_LTO) $(REL_INLINE)' "
            "'$(PIC_CFLAGS)' '$(DBG_CFLAGS)' '$(WASM_CFLAGS)'\n",
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        native, *other = result.stdout.splitlines()
        self.assertIn("-Dyyjson_inline=inline", native.split())
        for flags in other:
            self.assertNotIn("-Dyyjson_inline=inline", flags.split())

    def test_real_native_and_dictation_recipes_use_inline_policy(self):
        # Inspect make's expanded commands even when outputs already exist.
        targets = subprocess.run(
            ["make", "-s", "-f", "Makefile", "-f", "-", "inline-targets-for-test"],
            input=".PHONY: inline-targets-for-test\ninline-targets-for-test:\n"
            "\t@printf '%s\\n' '$(OBJ_REL)/src/core/image_service.o' "
            "'$(DICTATION_FIXTURE_OBJ)' '$(BIN)' '$(DICTATION_FIXTURE)'\n",
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        ).stdout.splitlines()
        result = subprocess.run(
            ["make", "-n", "-B", "release", "dictation-fixture"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        for target in targets:
            commands = [
                line for line in result.stdout.splitlines() if f" -o {target} " in line
            ]
            self.assertEqual(len(commands), 1, target)
            self.assertIn("-Dyyjson_inline=inline", commands[0].split())


if __name__ == "__main__":
    # The integration runner supplies its binary even though this check reads
    # the build contract rather than executing that binary.
    unittest.main(argv=[sys.argv[0]])
