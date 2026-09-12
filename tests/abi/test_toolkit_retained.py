"""A real committed artifact whose record fails, cancelled at the exact moment.

Only a disposable translation-unit copy is renamed for link interposition. The
provider, the filesystem failure, the job lifecycle, the serializer and the
public cancellation API all run unmodified. No production hook exists, and no
provider or failure branch is mocked.
"""

import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/fixtures"))
from toolkit_provider import ACCOUNT, PNG, TOKEN, Provider  # noqa: E402


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ToolkitRetainedArtifact(unittest.TestCase):
    def obstruct(self, home, stem, provider):
        """Turn this operation's live record into a directory, mid-request.

        The running record exists and the output is not yet committed, so only
        the finalizing rename can fail. This is a real filesystem failure.
        """
        self.assertTrue(provider.arrived.wait(30), "the request never arrived")
        running = sorted(home.glob(stem + ".tny-image-*.json"))
        self.assertEqual(len(running), 1, running)
        running[0].unlink()
        running[0].mkdir()
        # The already successful, held response carries real provider fields.
        provider.mode = "identified"
        provider.release.set()
        return running[0]

    def test_cancel_after_a_committed_artifact_keeps_its_io_detail(self):
        evidence_base = Path(
            os.environ.get("TNY_TEST_EVIDENCE_DIR", ROOT / "build/retained-evidence")
        )
        evidence = evidence_base / str(time.time_ns())
        evidence.mkdir(parents=True)
        with tempfile.TemporaryDirectory(prefix="tny-retained-") as temporary:
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

            def command(label, argv, *, stdin=None, expected=0, watch=None):
                started = time.time()
                if watch is None:
                    result = subprocess.run(
                        argv,
                        cwd=source,
                        env=env,
                        input=stdin,
                        text=True,
                        capture_output=True,
                        timeout=600,
                    )
                else:
                    with subprocess.Popen(
                        argv,
                        cwd=source,
                        env=env,
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                    ) as child:
                        watcher = threading.Thread(target=watch)
                        watcher.start()
                        out, err = child.communicate(timeout=600)
                        watcher.join(30)
                    result = subprocess.CompletedProcess(
                        argv, child.returncode, out, err
                    )
                (evidence / (label + ".log")).write_text(result.stdout + result.stderr)
                commands.append(
                    {
                        "label": label,
                        "cwd": str(source),
                        "argv": argv,
                        "exit_code": result.returncode,
                        "seconds": round(time.time() - started, 3),
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
.PHONY: retained-variables
retained-variables:
\t@printf '%s\\n' '$(CC)' '$(PIC_CFLAGS)' '$(REL_LDFLAGS)' '$(LIB_PIC_OBJS)'
"""
            variables = command(
                "compile-variables",
                ["make", "-s", "-f", "Makefile", "-f", "-", "retained-variables"],
                stdin=makefile,
            ).stdout.splitlines()
            self.assertEqual(len(variables), 4)
            compiler, flags, linker, objects = map(shlex.split, variables)
            original_object = "build/pic/src/core/image_service.o"
            self.assertIn(original_object, objects)
            objects.remove(original_object)
            command(
                "compile-real-serializer",
                compiler
                + flags
                + [
                    "-Dtny_image_retained_json=tny_image_retained_json_original",
                    "-c",
                    "src/core/image_service.c",
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
                    "tests/fixtures/toolkit_retained.c",
                    "-o",
                    "build/retained.o",
                ],
            )
            executable = source / "build/retained"

            def link(label):
                command(
                    label,
                    compiler
                    + ["-o", str(executable)]
                    + objects
                    + ["build/renamed-image.o", "build/retained.o"]
                    + linker
                    + ["-pthread"],
                )

            provider = Provider()
            self.addCleanup(provider.close)
            request = root / "request.json"

            def case(label, mode, *, expected=0, output="kept.png"):
                (home / output).unlink(missing_ok=True)
                for stale in home.glob(output + ".tny-image-*"):
                    shutil.rmtree(stale, ignore_errors=True)
                request.write_text(
                    json.dumps(
                        {
                            "version": 1,
                            "operation": "generate_image",
                            "config": {
                                "workspace": str(home),
                                "chatgpt_token": TOKEN,
                                "chatgpt_account_id": ACCOUNT,
                                "codex_base_url": provider.url + "/backend-api/codex",
                            },
                            "request": {"prompt": "fixture", "output_file": output},
                        }
                    )
                )
                provider.mode = "stall"
                provider.arrived.clear()
                provider.release.clear()
                record = []
                result = command(
                    label,
                    [str(executable), str(request), mode],
                    expected=expected,
                    watch=lambda: record.append(self.obstruct(home, output, provider)),
                )
                self.assertEqual(len(record), 1)
                # The paid artifact survives in every case.
                self.assertEqual((home / output).read_bytes(), PNG)
                return result, record[0]

            link("link-original")
            control, _ = case("original-control", "control")
            self.assertEqual(json.loads(control.stdout)["status"], -7)
            cancelled, record = case("original-cancelled", "cancel")
            observed = json.loads(cancelled.stdout)
            self.assertEqual(observed["status"], -7)
            self.assertEqual(observed["code"], "IMAGE_MANIFEST_FINALIZE_FAILED")
            self.assertGreater(observed["detail_bytes"], 0)
            self.assertTrue(observed["serializer_observed"])
            self.assertTrue(record.is_dir())

            # Reintroduce review finding 1 in a disposable translation unit.
            # The real native result accessor must reject leaked provider data.
            service = source / "src/core/image_service.c"
            service_original = service.read_text()
            local_call = "    local_provenance_metadata(result, out);"
            at = service_original.index("void tny_image_retained_json(")
            self.assertEqual(service_original[at:].count(local_call), 1)
            service.write_text(
                service_original[:at]
                + service_original[at:].replace(
                    local_call, "    provenance_metadata(result, out);", 1
                )
            )
            try:
                command(
                    "compile-provider-metadata-mutant",
                    compiler
                    + flags
                    + [
                        "-Dtny_image_retained_json=tny_image_retained_json_original",
                        "-c",
                        "src/core/image_service.c",
                        "-o",
                        "build/renamed-image.o",
                    ],
                )
                link("link-provider-metadata-mutant")
                leaked, _ = case("provider-metadata-mutant", "control", expected=1)
                self.assertIn(
                    "ASSERT a committed artifact must keep its retained IO detail",
                    leaked.stderr,
                )
                self.assertEqual(json.loads(leaked.stdout)["status"], -7)
            finally:
                service.write_text(service_original)
            command(
                "compile-provider-metadata-restored",
                compiler
                + flags
                + [
                    "-Dtny_image_retained_json=tny_image_retained_json_original",
                    "-c",
                    "src/core/image_service.c",
                    "-o",
                    "build/renamed-image.o",
                ],
            )
            link("link-provider-metadata-restored")
            case("provider-metadata-restored", "control")
            self.assertEqual(digest(service), inputs["src/core/image_service.c"])

            toolkit = source / "src/lib/toolkit.c"
            original = toolkit.read_text()
            precedence = (
                "    if (job->image_committed && job->image_failure_status) "
                "return job->image_failure_status;\n"
            )
            cancel_line = (
                "    if (rc == 130 || stopped(job)) return TNY_STATUS_CANCELLED;\n"
            )
            self.assertEqual(original.count(precedence), 1)
            self.assertEqual(original.count(cancel_line), 1)
            # This compiled fault restores the old late-cancel precedence: a
            # cancellation arriving after the commit would erase the detail.
            toolkit.write_text(
                original.replace(precedence, "", 1).replace(
                    cancel_line, cancel_line + precedence, 1
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
                mutant, _ = case("mutant-cancelled", "cancel", expected=1)
                self.assertIn(
                    "ASSERT a committed artifact must keep its retained IO detail",
                    mutant.stderr,
                )
                observed = json.loads(mutant.stdout)
                self.assertEqual(observed["status"], -12)  # cancelled
                self.assertEqual(observed["detail_bytes"], 0)
                # The control case is unaffected by the fault, which is why the
                # cancelled one is the oracle.
                control, _ = case("mutant-control", "control")
                self.assertEqual(json.loads(control.stdout)["status"], -7)
            finally:
                toolkit.write_text(original)
            command(
                "compile-restored",
                compiler
                + flags
                + ["-c", "src/lib/toolkit.c", "-o", "build/pic/src/lib/toolkit.o"],
            )
            link("link-restored")
            restored, _ = case("restored-cancelled", "cancel")
            self.assertEqual(json.loads(restored.stdout)["status"], -7)
            self.assertEqual(digest(toolkit), inputs["src/lib/toolkit.c"])
            print(f"retained evidence: {evidence}", flush=True)
