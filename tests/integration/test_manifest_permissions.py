#!/usr/bin/env python3
"""Permission identity and reserved record names for image operations (#127).

    env -u TNY_TOOLS python3 tests/integration/test_manifest_permissions.py

`tests/integration/run.sh` may append the binary path instead of setting $TNY;
both forms work. The provider is the same local HTTP fixture the rest of the
image suite uses, with fake credentials: every "no request was made" assertion
below is observed on that server, not reported by tny.

These cases cover what a unit test cannot: the real rules engine reading real
settings, deciding on the real permission detail, and a real CLI writing real
records (ADR 0095).
"""

from __future__ import annotations

import json
import os
import queue
import shlex
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _binary_argument() -> list[str]:
    """run.sh appends $TNY; take it as the binary, not as a test name."""
    kept = [sys.argv[0]]
    for argument in sys.argv[1:]:
        if (
            not argument.startswith("-")
            and os.path.isfile(argument)
            and os.access(argument, os.X_OK)
        ):
            os.environ["TNY"] = argument
            continue
        kept.append(argument)
    return kept


ARGV = _binary_argument()
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_image_workflow import (  # noqa: E402
    MANIFEST_NAME,
    WASM,
    ImageFixture,
    png,
    sha,
)


class ManifestPermissions(ImageFixture):
    def rules(self, permission):
        directory = self.home / ".tny"
        directory.mkdir(exist_ok=True)
        (directory / "settings.json").write_text(json.dumps({"permission": permission}))

    def handwritten(self, name, provider):
        """A succeeded record of an earlier generate, by the documented schema."""
        record = self.home / name
        record.write_text(
            json.dumps(
                {
                    "version": 1,
                    "kind": "image_manifest",
                    "operation_id": "1234abcd1234abcd",
                    "operation": "generate",
                    "status": "succeeded",
                    "workspace": str(self.root),
                    "started": "2026-09-12T00:00:00Z",
                    "finished": "2026-09-12T00:00:05Z",
                    "prompt": "a recorded prompt",
                    "output": "old.png",
                    "references": [],
                    "requested": {"provider": provider, "size": "auto"},
                    "effective": {"provider": provider},
                }
            )
        )
        return record

    def test_a_replay_is_granted_as_the_provider_it_actually_runs(self):
        # The record names a provider this build does not have. A rule written
        # for the default provider must not authorize it.
        record = self.handwritten("source.json", "nonesuch")
        arguments = {"output_file": "replayed.png", "from_manifest": str(record)}
        self.rules({"image_generate": {'{"provider":"codex",*': "allow"}})
        run = self.agent("all", arguments, mode="ask")
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual(self.image_requests(), [])
        self.assertEqual(list(self.home.glob("replayed.png*")), [])
        # A rule for the provider the record really names does authorize it,
        # and the call then runs exactly that provider — and fails on it.
        self.rules({"image_generate": {'{"provider":"nonesuch",*': "allow"}})
        run = self.agent("all", arguments, mode="ask")
        self.assertEqual(run.returncode, 0, run.stderr)
        result = self.tool_result()
        self.assertTrue(result.startswith("error: "), result)
        self.assertIn("unknown image provider", result)
        self.assertEqual(self.image_requests(), [])
        self.assertEqual(list(self.home.glob("replayed.png*")), [])

    def test_a_denied_image_call_spends_and_writes_nothing(self):
        self.rules({"image_generate": {"*": "deny"}})
        run = self.agent(
            "all", {"prompt": "A blue robot", "output_file": "denied.png"}, mode="ask"
        )
        # A denied sensitive tool stops the ask-mode run, exactly as before.
        self.assertEqual(run.returncode, 2, run.stderr)
        self.assertIn(b"image_generate", run.stderr)
        self.assertEqual(self.image_requests(), [])
        self.assertEqual(list(self.home.glob("denied.png*")), [])

    def test_an_approved_call_still_pays_exactly_once(self):
        self.rules({"image_generate": {'{"provider":"codex",*': "allow"}})
        self.state["image"] = png(6, 6)
        run = self.agent(
            "all", {"prompt": "A blue robot", "output_file": "granted.png"}, mode="ask"
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(self.tool_result())
        self.assertEqual(
            result["operation_id"], self.record("granted.png")["operation_id"]
        )
        self.assertEqual(len(self.image_requests()), 1)
        self.assertEqual((self.home / "granted.png").read_bytes(), self.state["image"])

    @unittest.skipIf(WASM, "the reserved-name check is exercised natively")
    def test_a_record_beside_a_marked_output_is_still_reserved(self):
        # An output whose own name contains the record marker is allowed, and
        # the record written beside it carries a second, real marker.
        self.state["image"] = png(7, 7)
        output = self.home / "shot.tny-image-1.png"
        run = self.generate("--json", output=output)
        self.assertEqual(run.returncode, 0, run.stderr)
        records = [
            p
            for p in self.home.glob("shot.tny-image-1.png.*")
            if MANIFEST_NAME.fullmatch(p.name)
        ]
        self.assertEqual(len(records), 1, records)
        record, before = records[0], records[0].read_bytes()
        self.assertEqual(output.read_bytes(), self.state["image"])
        # That real record may not become the next generation's destination.
        run = self.generate("--json", output=record)
        self.assertEqual(run.returncode, 1, run.stderr)
        self.assertIn(b"reserved", run.stderr)
        self.assertEqual(record.read_bytes(), before)
        self.assertEqual(len(self.image_requests()), 1)  # nothing was requested
        # The ordinary case is unchanged: a plain name is not reserved.
        run = self.generate("--json", output=self.home / "plain.png")
        self.assertEqual(run.returncode, 0, run.stderr)


@unittest.skipIf(WASM, "native pending-permission ownership fixture")
class PendingManifestPermissions(ImageFixture):
    @classmethod
    def setUpClass(cls):
        # Link the real native backend and tools; no substituted executor or
        # permission hook. The fixture lives in the existing fixtures directory.
        supplied = os.environ.get("TNY_MANIFEST_PENDING_BIN")
        if supplied:
            cls.binary = supplied
            return
        cls.build = tempfile.TemporaryDirectory(prefix="tny-manifest-pending-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = str(Path(cls.build.name) / "pending")
        subprocess.run(["make", "-s", "lib-shared-active"], cwd=ROOT, check=True)
        variables = subprocess.check_output(
            ["make", "-s", "-f", "Makefile", "-f", "-", "pending-variables"],
            cwd=ROOT,
            text=True,
            input="""
.PHONY: pending-variables
pending-variables:
	@printf '%s\\n' '$(CC)' '$(PIC_CFLAGS)' '$(REL_LDFLAGS)' '$(LIB_PIC_OBJS)'
""",
        ).splitlines()
        compiler, flags, linker, objects = map(shlex.split, variables)
        # Rename only this translation unit's allocation/ownership calls. The
        # fixture still runs the real parser, prepare, pending move and backend.
        objects.remove("build/pic/src/core/image_service.o")
        service = str(Path(cls.build.name) / "image-service.o")
        subprocess.run(
            compiler
            + flags
            + [
                "-Dxstrdup=manifest_test_strdup",
                "-Dfree=manifest_test_free",
                "-Dtny_image_manifest_load=manifest_test_load",
                "-Dtny_image_manifest_resolve=manifest_test_resolve",
                "-Dtny_image_manifest_free=manifest_test_manifest_free",
                "-c",
                "src/core/image_service.c",
                "-o",
                service,
            ],
            cwd=ROOT,
            check=True,
        )
        objects.append(service)
        subprocess.run(
            compiler
            + flags
            + [str(ROOT / "tests/fixtures/manifest_pending.c")]
            + objects
            + linker
            + ["-pthread", "-o", cls.binary],
            cwd=ROOT,
            check=True,
        )

    def pending_case(self, caller, source_kind, change):
        # A real image and immutable record produced by the CLI, then used as
        # either an artifact-only input or a recorded edit's original reference.
        original = png(3, 4)
        substitute = png(5, 6)
        reference = self.home / "original.png"
        self.state["image"] = original
        self.result(self.generate("--json", output=reference))
        artifact_record = self.manifests(reference.name)[0]
        record = artifact_record
        if source_kind == "replay":
            prior = self.home / "prior.png"
            self.result(
                self.cli(
                    "edit",
                    "--artifact",
                    str(artifact_record),
                    "--output-file",
                    str(prior),
                    "--model",
                    "approved-model",
                    "--quality",
                    "low",
                    "--size",
                    "3x4",
                    "--json",
                    prompt=b"approved prompt",
                )
            )
            record = self.manifests(prior.name)[0]
        saved = record.read_bytes()
        lineage = None
        if source_kind == "replay":
            lineage = json.loads(saved)["references"][0]["source_manifest"]
            self.assertTrue(lineage, "replay must contain artifact-derived lineage")
        replacement = self.home / "substitute.png"
        replacement.write_bytes(substitute)
        output = self.home / "approved.png"
        if change in ("allocation", "destination"):
            output.write_bytes(b"existing output must not change")
        before_files = {
            p.name: p.read_bytes() for p in self.home.iterdir() if p.is_file()
        }
        arguments = {"output_file": str(output)}
        if source_kind == "replay":
            arguments["from_manifest"] = str(record)
            command = f"tny image replay --manifest {shlex.quote(str(record))}"
        else:
            arguments.update(
                prompt="approved prompt",
                artifact=str(record),
                model="approved-model",
                quality="low",
                size="3x4",
            )
            command = (
                f"printf 'approved prompt' | tny image edit --artifact {shlex.quote(str(record))}"
                " --model approved-model --quality low --size 3x4"
            )
        command += f" --output-file {shlex.quote(str(output))} --json"
        self.state.update(
            tool_args=arguments,
            tool_name="image_edit",
            command=command if caller == "terminal" else None,
            image=png(3, 4),
        )
        self.state["requests"].clear()
        self.state["chat"].clear()
        if caller == "ordinary":
            # Replaying into the source itself would destroy provenance if the
            # failed protection lookup were treated as a non-alias.
            run = subprocess.run(
                [
                    self.binary,
                    str(self.root),
                    self.url + "/v1",
                    self.url + "/backend-api/codex",
                    caller,
                    str(record),
                    str(prior),
                ],
                cwd=self.home,
                env=self.env,
                text=True,
                capture_output=True,
                timeout=30,
            )
            self.assertEqual(
                self.image_requests(), [], "failed protection must not spend"
            )
            self.assertEqual(run.returncode, 0, run.stderr)
            end = json.loads(run.stdout)
            self.assertEqual(end["rc"], 1)
            self.assertEqual(end["injected"], 1)
            self.assertEqual(end["live"], 0)
            self.assertGreaterEqual(end["destination_owned"], 8)
            self.assertEqual(end["acquired"], end["disposed"])
            self.assertIn(
                "out of memory resolving the replay source artifact", run.stderr
            )
            self.assertEqual(list(self.home.glob("**/image-guards/*")), [])
            self.assertEqual(self.image_requests(), [])
            self.assertEqual(
                {p.name: p.read_bytes() for p in self.home.iterdir() if p.is_file()},
                before_files,
            )
            print(
                f"destination failure caller={caller}: {json.dumps(end, sort_keys=True)}",
                flush=True,
            )
            return
        proc = subprocess.Popen(
            [
                self.binary,
                str(self.root),
                self.url + "/v1",
                self.url + "/backend-api/codex",
                caller,
            ]
            + (
                [lineage]
                if change == "allocation"
                else ["destination"]
                if change == "destination"
                else []
            ),
            cwd=self.home,
            env=self.env,
            text=True,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        messages = queue.Queue()

        def read():
            for line in proc.stdout:
                messages.put(line)
            messages.put(None)

        thread = threading.Thread(target=read, daemon=True)
        thread.start()

        def receive():
            line = messages.get(timeout=30)
            if line is None:
                self.fail("pending fixture closed stdout: " + proc.stderr.read())
            return json.loads(line)

        def send(action):
            proc.stdin.write(action + "\n")
            proc.stdin.flush()

        try:
            summaries = []
            for turn in range(2):
                pending = receive()
                if turn == 0 and change == "allocation":
                    self.assertEqual(
                        pending["event"], "ended", "failed prepare must not ask"
                    )
                    self.assertEqual(pending["questions"], 0)
                    self.assertEqual(pending["grants"], 0)
                    self.assertEqual(pending["injected"], 1)
                    self.assertEqual(pending["live"], 0)
                    self.assertGreaterEqual(pending["acquired"], 3)
                    self.assertEqual(pending["acquired"], pending["disposed"])
                    print(
                        f"lineage failure caller={caller}: {json.dumps(pending, sort_keys=True)}",
                        flush=True,
                    )
                    self.assertEqual(self.image_requests(), [])
                    self.assertIn(
                        "out of memory resolving an image reference", self.tool_result()
                    )
                    self.assertEqual(
                        {
                            p.name: p.read_bytes()
                            for p in self.home.iterdir()
                            if p.is_file()
                        },
                        before_files,
                    )
                    self.assertEqual(list(self.home.glob("approved.png*")), [output])
                    # Reuse this exact backend/permission fixture after failure:
                    # no stale pending plan or one-time grant can survive it.
                    output.unlink()
                    self.state["chat"].clear()
                    send("next")
                    continue
                self.assertEqual(pending["event"], "permission")
                summaries.append(pending["summary"])
                before = len(self.image_requests())
                if change == "destination":
                    self.assertEqual(output.read_bytes(), before_files[output.name])
                else:
                    self.assertFalse(output.exists())
                    self.assertEqual(list(self.home.glob("approved.png*")), [])
                if turn == 0:
                    self.assertEqual(before, 0)
                    if change == "mapping":
                        changed = json.loads(saved)
                        changed["prompt"] = "unapproved prompt"
                        changed["requested"].update(
                            provider="nonesuch",
                            model="wrong-model",
                            quality="max",
                            size="5x6",
                        )
                        if source_kind == "artifact":
                            changed["output"] = str(replacement)
                            changed["artifacts"][0].update(
                                path=str(replacement), sha256=sha(substitute)
                            )
                        else:
                            changed["references"][0].update(
                                path=str(replacement), sha256=sha(substitute)
                            )
                        record.write_text(json.dumps(changed))
                    elif change == "bytes":
                        reference.write_bytes(substitute)
                action = (
                    change if turn == 0 and change in ("cancel", "destroy") else "allow"
                )
                send(action)
                end = receive()
                self.assertEqual(end["event"], "ended")
                self.assertEqual(end["grants"], 0)
                if turn == 0 and change == "destination":
                    self.assertEqual(end["questions"], 1)
                    self.assertEqual(end["injected"], 1)
                    self.assertEqual(end["live"], 0)
                    self.assertGreaterEqual(end["acquired"], 8)
                    self.assertEqual(end["acquired"], end["disposed"])
                    self.assertEqual(self.image_requests(), [])
                    self.assertIn(
                        "out of memory resolving the replay source artifact",
                        self.tool_result(),
                    )
                    self.assertEqual(
                        {
                            p.name: p.read_bytes()
                            for p in self.home.iterdir()
                            if p.is_file()
                        },
                        before_files,
                    )
                    self.assertEqual(list(self.home.glob("approved.png*")), [output])
                    self.assertEqual(list(self.home.glob("**/image-guards/*")), [])
                    print(
                        f"destination failure caller={caller}: {json.dumps(end, sort_keys=True)}",
                        flush=True,
                    )
                    self.state["chat"].clear()
                    send("next")
                    continue
                failed = turn == 0 and change in ("bytes", "cancel", "destroy")
                self.assertEqual(
                    len(self.image_requests()) - before, 0 if failed else 1
                )
                if failed:
                    self.assertFalse(output.exists())
                    self.assertEqual(list(self.home.glob("approved.png*")), [])
                    if change == "cancel":
                        self.assertEqual(end["stop"], 1)
                    if change == "bytes":
                        self.assertIn("no longer matches its hash", self.tool_result())
                else:
                    route, body = self.image_requests()[-1]
                    self.assertEqual(route, "/backend-api/codex/images/edits")
                    self.assertEqual(self.uploaded(), original)
                    self.assertEqual(body["prompt"], "approved prompt")
                    self.assertEqual(body["model"], "approved-model")
                    self.assertEqual(body["quality"], "low")
                    self.assertEqual(body["size"], "3x4")
                    self.assertEqual(output.read_bytes(), self.state["image"])
                    finished = self.record(output.name)
                    self.assertEqual(finished["status"], "succeeded")
                    self.assertEqual(finished["references"][0]["sha256"], sha(original))
                    if lineage is not None:
                        self.assertEqual(
                            finished["references"][0]["source_manifest"], lineage
                        )
                if turn == 0:
                    record.write_bytes(saved)
                    reference.write_bytes(original)
                    output.unlink(missing_ok=True)
                    for manifest in self.manifests(output.name):
                        manifest.unlink()
                    self.state["chat"].clear()
                    send("next")
            if change == "allocation":
                self.assertEqual(len(summaries), 1)
                self.assertEqual(len(self.image_requests()), 1)
            else:
                self.assertEqual(summaries[0], summaries[1])
            proc.stdin.close()
            self.assertEqual(proc.wait(timeout=30), 0, proc.stderr.read())
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=10)
            for stream in (proc.stdin, proc.stdout, proc.stderr):
                stream.close()
            thread.join(timeout=5)

    def test_replay_lineage_allocation_failure_has_no_pending_plan(self):
        for caller in ("typed", "terminal"):
            with self.subTest(caller=caller):
                case = type(self)(
                    "test_replay_lineage_allocation_failure_has_no_pending_plan"
                )
                case.setUp()
                try:
                    case.pending_case(caller, "replay", "allocation")
                finally:
                    case.tearDown()

    def test_execution_destination_allocation_failure(self):
        for caller in ("ordinary", "typed", "terminal"):
            with self.subTest(caller=caller):
                case = type(self)("test_execution_destination_allocation_failure")
                case.setUp()
                try:
                    case.pending_case(caller, "replay", "destination")
                finally:
                    case.tearDown()

    def test_pending_permission_matrix(self):
        # Every row crosses the native pending tools_call transfer. Repeated
        # identical permission detail must ask again: ALLOW_ONCE stored no grant.
        for caller in ("typed", "terminal"):
            for source in ("artifact", "replay"):
                for change in ("unchanged", "mapping", "bytes", "cancel", "destroy"):
                    with self.subTest(caller=caller, source=source, change=change):
                        # Each row owns a fresh filesystem/server; retain the
                        # normal unittest fixture's setup/teardown isolation.
                        case = type(self)("test_pending_permission_matrix")
                        case.setUp()
                        try:
                            case.pending_case(caller, source, change)
                            print(
                                f"pending PASS caller={caller} source={source} change={change}: "
                                "2 permission events, 0 remembered grants",
                                flush=True,
                            )
                        finally:
                            case.tearDown()


if __name__ == "__main__":
    unittest.main(argv=ARGV)
