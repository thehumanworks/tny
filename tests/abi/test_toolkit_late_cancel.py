"""A deterministic real cross-thread cancellation after strict JSON construction.

Only a disposable translation-unit copy is renamed for link interposition. The
actual serializer, job lifecycle, error classification and public cancellation
API run unmodified. No provider or production cancellation branch is mocked.
"""

import hashlib
import json
import os
import shlex
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ToolkitLateCancellation(unittest.TestCase):
    def test_cancel_after_real_strict_serialization_discards_detail(self):
        evidence_base = Path(
            os.environ.get("TNY_TEST_EVIDENCE_DIR", ROOT / "build/late-cancel-evidence")
        )
        evidence = evidence_base / str(time.time_ns())
        evidence.mkdir(parents=True)
        self.addCleanup(
            lambda: None
        )  # Evidence deliberately survives the scratch tree.
        with tempfile.TemporaryDirectory(prefix="tny-late-cancel-") as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            for folder in ("src", "include", "third_party", "abi", "scripts", "tests"):
                shutil.copytree(
                    ROOT / folder,
                    source / folder,
                    ignore=shutil.ignore_patterns(
                        "__pycache__", "build", "node_modules"
                    ),
                )
            shutil.copy2(ROOT / "Makefile", source / "Makefile")
            inputs = {
                str(p.relative_to(source)): digest(p)
                for p in source.rglob("*")
                if p.is_file()
            }
            (evidence / "inputs.json").write_text(json.dumps(inputs, indent=2) + "\n")
            env = os.environ.copy()
            for name in tuple(env):
                if name.startswith("TNY_") or any(
                    part in name
                    for part in ("TOKEN", "SECRET", "PASSWORD", "API_KEY", "ACCESS_KEY")
                ):
                    env.pop(name)
            home = root / "home"
            home.mkdir()
            env.update(HOME=str(home), TNY_VERSION="1.0.0-test")
            commands = []

            def command(label, argv, *, stdin=None, expected=0):
                result = subprocess.run(
                    argv,
                    cwd=source,
                    env=env,
                    input=stdin,
                    text=True,
                    capture_output=True,
                    timeout=240,
                )
                (evidence / (label + ".log")).write_text(result.stdout + result.stderr)
                commands.append(
                    {
                        "label": label,
                        "cwd": str(source),
                        "argv": argv,
                        "exit_code": result.returncode,
                        "log_sha256": digest(evidence / (label + ".log")),
                    }
                )
                (evidence / "runs.json").write_text(
                    json.dumps(commands, indent=2) + "\n"
                )
                self.assertEqual(result.returncode, expected, result.stderr[-4000:])
                return result

            command("build-original", ["make", "-j4", "lib-shared-active"])
            makefile = """
.PHONY: late-variables
late-variables:
	@printf '%s\\n' '$(CC)' '$(PIC_CFLAGS)' '$(REL_LDFLAGS)' '$(LIB_PIC_OBJS)'
"""
            variables = command(
                "compile-variables",
                ["make", "-s", "-f", "Makefile", "-f", "-", "late-variables"],
                stdin=makefile,
            ).stdout.splitlines()
            self.assertEqual(len(variables), 4)
            compiler, flags, linker, objects = map(shlex.split, variables)
            target = "src/core/image_service.c"
            original_object = "build/pic/src/core/image_service.o"
            self.assertIn(original_object, objects)
            objects.remove(original_object)
            command(
                "compile-real-serializer",
                compiler
                + flags
                + [
                    "-Dtny_image_error_json=tny_image_error_json_original",
                    "-c",
                    target,
                    "-o",
                    "build/renamed-image.o",
                ],
            )
            command(
                "compile-barrier",
                compiler
                + flags
                + [
                    "-c",
                    "tests/fixtures/toolkit_late_cancel.c",
                    "-o",
                    "build/late-cancel.o",
                ],
            )
            executable = source / "build/late-cancel"

            def link(label):
                command(
                    label,
                    compiler
                    + ["-o", str(executable)]
                    + objects
                    + ["build/renamed-image.o", "build/late-cancel.o"]
                    + linker
                    + ["-pthread"],
                )

            request = root / "request.json"
            request.write_text(
                json.dumps(
                    {
                        "version": 1,
                        "operation": "generate_image",
                        "config": {
                            "workspace": str(home),
                            "chatgpt_token": "fixture-token",
                            "chatgpt_account_id": "fixture-account",
                            "codex_base_url": "http://127.0.0.1:9",
                        },
                        "request": {
                            "prompt": "fixture",
                            "output_file": "never.png",
                            "size": "auto",
                            "strict_size": True,
                        },
                    }
                )
            )
            link("link-original")
            command("original", [str(executable), str(request)])
            toolkit = source / "src/lib/toolkit.c"
            original = toolkit.read_text()
            guard = "if (job->image_detail && !final_override) return;"
            self.assertEqual(original.count(guard), 1)
            # This compiled fault keeps cancellation's status but wrongly retains
            # its completed strict detail. It must fail the precise state oracle.
            toolkit.write_text(
                original.replace(
                    guard,
                    "(void)final_override;\n    if (job->image_detail) return;",
                    1,
                )
            )
            try:
                command(
                    "compile-mutant",
                    compiler
                    + flags
                    + ["-c", "src/lib/toolkit.c", "-o", "build/pic/src/lib/toolkit.o"],
                )
                link("link-mutant")
                result = command("mutant", [str(executable), str(request)], expected=1)
                self.assertIn(
                    "ASSERT late cancellation must discard completed strict detail",
                    result.stderr,
                )
                records = [json.loads(line) for line in result.stdout.splitlines()]
                self.assertEqual(records[-1]["status"], -12)
                self.assertGreater(records[-1]["detail_bytes"], 0)
            finally:
                toolkit.write_text(original)
            command(
                "compile-restored",
                compiler
                + flags
                + ["-c", "src/lib/toolkit.c", "-o", "build/pic/src/lib/toolkit.o"],
            )
            link("link-restored")
            command("restored", [str(executable), str(request)])
            self.assertFalse((home / "never.png").exists())
            self.assertEqual(digest(toolkit), inputs["src/lib/toolkit.c"])
            print(f"late-cancel evidence: {evidence}", flush=True)
