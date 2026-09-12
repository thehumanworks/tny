#!/usr/bin/env python3
"""A20: real durable job producers and owned image consumers.

Runs the existing actual detached supervisor and CLI against loopback image and
conversation providers. No selector, job state machine or upload path is stubbed.
"""

from __future__ import annotations

import hashlib
import json
import queue
import shlex
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

import test_image_preview_workflow as preview
from test_image_workflow import ROOT, TNY, WASM, ImageFixture, sha
from test_jobs import JobsFixture, argv_without_runner_binary


class CompiledArtifactChecks:
    @classmethod
    def setUpClass(cls):
        # The real linked core is a required prerequisite, built rather than
        # skipped when this focused suite is invoked after release-only builds.
        subprocess.run(["make", "-s", "build/tny-test"], cwd=ROOT, check=True)
        cls.fixture_dir = tempfile.TemporaryDirectory(
            prefix="tny-job-artifact-fixture-"
        )
        cls.addClassCleanup(cls.fixture_dir.cleanup)
        variables = subprocess.check_output(
            ["make", "-s", "-f", "Makefile", "-f", "-", "artifact-variables"],
            cwd=ROOT,
            text=True,
            input="artifact-variables:\n\t@printf '%s\\n' '$(CC)' '$(DBG_CFLAGS)' '$(DBG_LDFLAGS)' '$(TEST_OBJS)'\n",
        ).splitlines()
        compiler, flags, linker, objects = map(shlex.split, variables)
        objects = list(dict.fromkeys(objects))
        objects.remove("build/dbg/src/core/jobs.o")
        objects.remove("build/dbg/src/util/image_io.o")
        image_io = str(Path(cls.fixture_dir.name) / "image_io.o")
        subprocess.run(
            compiler
            + flags
            + [
                "-Dopenat=job_test_openat",
                "-Dread=job_test_read",
                "-c",
                "src/util/image_io.c",
                "-o",
                image_io,
            ],
            cwd=ROOT,
            check=True,
        )
        cls.checker = str(Path(cls.fixture_dir.name) / "checks")
        subprocess.run(
            compiler
            + flags
            + ["tests/fixtures/job_artifact_checks.c", image_io]
            + objects
            + linker
            + ["-o", cls.checker],
            cwd=ROOT,
            check=True,
        )
        cls.pending = str(Path(cls.fixture_dir.name) / "pending")
        subprocess.run(
            compiler
            + flags
            + ["tests/fixtures/job_artifact_pending.c"]
            + objects
            + ["build/dbg/src/core/jobs.o", "build/dbg/src/util/image_io.o"]
            + linker
            + ["-o", cls.pending],
            cwd=ROOT,
            check=True,
        )

    def check(self, *args):
        run = subprocess.run(
            [self.checker, *map(str, args)],
            cwd=self.home,
            env=self.env,
            capture_output=True,
            timeout=30,
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        return json.loads(run.stdout)


class JobArtifactProducer(CompiledArtifactChecks, JobsFixture):
    def test_real_producer_identity_is_preserved(self):
        output = self.workspace / "producer.png"
        _, submitted = self.submit(
            "image",
            "--output-file",
            str(output),
            "--prompt",
            "producer identity fixture",
        )
        record = self.await_terminal(submitted["id"])
        self.assertEqual(record["state"], "succeeded", record)
        item = record["items"][0]
        producer = json.loads(Path(item["log_path"]).read_text())
        self.assertTrue(producer["ok"], producer)
        self.assertEqual(
            producer["sha256"], hashlib.sha256(output.read_bytes()).hexdigest()
        )
        self.assertEqual(item["output_sha256"], producer["sha256"])
        self.assertEqual(item["output_bytes"], producer["bytes"])
        self.assertEqual(item["operation_id"], producer["operation_id"])
        self.assertEqual(item["manifest_path"], producer["manifest_path"])
        manifest = json.loads(Path(producer["manifest_path"]).read_text())
        self.assertEqual(manifest["operation_id"], producer["operation_id"])
        self.assertEqual(manifest["artifacts"][0]["sha256"], producer["sha256"])
        self.assertEqual(len(self.image_requests()), 1)

    def test_producer_replacement_malformed_identity_and_copy_failure(self):
        output = self.workspace / "checked.png"
        _, submitted = self.submit(
            "image", "--output-file", str(output), "--prompt", "analyzer fixture"
        )
        record = self.await_terminal(submitted["id"])
        self.assertEqual(record["state"], "succeeded", record)
        item = record["items"][0]
        producer = json.loads(Path(item["log_path"]).read_text())
        log = self.home / "checked-producer.json"
        log.write_text(json.dumps(producer))
        self.assertTrue(self.check("analyze", log, output.resolve())["ok"])
        original = output.read_bytes()
        output.write_bytes(b"replaced disk bytes")
        self.assertFalse(self.check("analyze", log, output.resolve())["ok"])
        output.write_bytes(original)
        for field, value in [
            ("sha256", None),
            ("sha256", True),
            ("sha256", "0" * 64),
            ("bytes", None),
            ("bytes", str(len(original))),
            ("bytes", True),
            ("bytes", float(len(original))),
            ("bytes", len(original) + 1),
            ("kind", "image\0hidden"),
            ("manifest_path", False),
        ]:
            with self.subTest(field=field, value=value):
                changed = dict(producer, **{field: value})
                log.write_text(json.dumps(changed))
                self.assertFalse(self.check("analyze", log, output.resolve())["ok"])
        for field in ("sha256", "bytes"):
            changed = dict(producer)
            del changed[field]
            log.write_text(json.dumps(changed))
            self.assertFalse(self.check("analyze", log, output.resolve())["ok"])
        log.write_text(json.dumps(producer))
        for field in ("sha256", "operation_id", "manifest_path"):
            with self.subTest(allocation=field):
                self.assertFalse(
                    self.check("analyze", log, output.resolve(), producer[field])["ok"]
                )

    def test_acquired_metadata_refuses_open_time_foreign_symlink(self):
        output = self.workspace / "confined.png"
        _, submitted = self.submit(
            "image", "--output-file", str(output), "--prompt", "confinement fixture"
        )
        record = self.await_terminal(submitted["id"])
        self.assertEqual(record["state"], "succeeded", record)
        item = record["items"][0]
        job = self.home / ".tny/jobs" / submitted["id"] / "job.json"
        foreign = self.home / "outside.json"
        args = (
            "select",
            self.workspace.resolve(),
            (self.home / ".tny").resolve(),
            submitted["id"],
            0,
        )
        self.assertTrue(self.check(*args)["ok"])
        for target in (job.resolve(), Path(item["manifest_path"])):
            with self.subTest(target=target.name):
                foreign.write_bytes(target.read_bytes())
                result = self.check(*args, target, foreign.resolve())
                self.assertEqual(
                    result, {"ok": False, "swapped": 1, "foreign_reads": 0}
                )
        saved = job.read_bytes()
        changed = json.loads(saved)
        changed["state"] = "succeeded\0hidden"
        job.write_text(json.dumps(changed))
        self.assertFalse(self.check(*args)["ok"])
        job.write_bytes(saved)

    def test_strict_snapshot_fields_and_declared_manifest_identity(self):
        output = self.workspace / "strict.png"
        _, submitted = self.submit(
            "image", "--output-file", str(output), "--prompt", "strict identity"
        )
        record = self.await_terminal(submitted["id"])
        self.assertEqual(record["state"], "succeeded", record)
        path = self.home / ".tny/jobs" / submitted["id"] / "job.json"
        original = json.loads(path.read_text())
        item = original["items"][0]
        manifest = Path(item["manifest_path"])
        saved_manifest = manifest.read_bytes()
        args = (
            "select",
            self.workspace.resolve(),
            (self.home / ".tny").resolve(),
            submitted["id"],
            0,
        )
        for field, value in (
            ("index", "0"),
            ("index", 1),
            ("index", False),
            ("state", "succeeded\0hidden"),
            ("output_bytes", True),
            ("output_bytes", 1.0),
            ("output_bytes", 0),
            ("output_sha256", None),
            ("manifest_path", False),
            ("operation_id", None),
        ):
            with self.subTest(item_field=field, value=value):
                changed = json.loads(json.dumps(original))
                changed["items"][0][field] = value
                path.write_text(json.dumps(changed))
                self.assertFalse(self.check(*args)["ok"])
        for field, value in (
            ("id", "f" * 32),
            ("id", submitted["id"] + "\0hidden"),
            ("job_kind", "ask"),
            ("version", 1.0),
            ("attempt", True),
        ):
            with self.subTest(root_field=field, value=value):
                changed = json.loads(json.dumps(original))
                changed[field] = value
                path.write_text(json.dumps(changed))
                self.assertFalse(self.check(*args)["ok"])
        path.write_text(json.dumps(original))
        foreign = self.home / "foreign-manifest.json"
        foreign.write_bytes(saved_manifest)
        changed = json.loads(json.dumps(original))
        changed["items"][0]["manifest_path"] = str(foreign.resolve())
        path.write_text(json.dumps(changed))
        self.assertFalse(self.check(*args)["ok"])
        foreign_image = self.home / "foreign.png"
        foreign_image.write_bytes(output.read_bytes())
        changed["items"][0].update(
            manifest_path=None, output_path=str(foreign_image.resolve())
        )
        path.write_text(json.dumps(changed))
        self.assertFalse(self.check(*args)["ok"])
        path.write_text(json.dumps(original))
        for field, value in (
            ("path", str(self.workspace / "different.png")),
            ("sha256", "0" * 64),
            ("bytes", item["output_bytes"] + 1),
        ):
            with self.subTest(manifest_artifact=field):
                changed = json.loads(saved_manifest)
                changed["artifacts"][0][field] = value
                manifest.write_text(json.dumps(changed))
                self.assertFalse(self.check(*args)["ok"])
        changed = json.loads(saved_manifest)
        changed["operation_id"] = "f" * 16
        manifest.write_text(json.dumps(changed))
        self.assertFalse(self.check(*args)["ok"])
        manifest.write_bytes(saved_manifest)
        path.write_bytes(b" " * (4 * 1024 * 1024 + 1))
        self.assertFalse(self.check(*args)["ok"])
        path.write_text(json.dumps(original))
        manifest.unlink()
        self.assertFalse(self.check(*args)["ok"])
        self.assertEqual(len(self.image_requests()), 1)


class JobArtifactConsumers(CompiledArtifactChecks, ImageFixture):
    # Reuse the real provider-wire fixture without inheriting its unrelated tests.
    configure = preview.PreviewWorkflow.configure
    ask = preview.PreviewWorkflow.ask
    results = preview.PreviewWorkflow.results
    pixels = preview.PreviewWorkflow.pixels
    result_object = preview.PreviewWorkflow.result_object
    assert_preview = preview.PreviewWorkflow.assert_preview

    def setUp(self):
        super().setUp()
        self.server.RequestHandlerClass = preview.PreviewHandler
        self.settings = self.home / ".tny/settings.json"
        self.settings.parent.mkdir()
        self.configure(True)

    def job_command(self, *args):
        return subprocess.run(
            [TNY, "--cwd", str(self.home), "jobs", *args, "--json"],
            env=self.env,
            capture_output=True,
            timeout=60,
        )

    def produced_job(self):
        run = self.job_command(
            "submit",
            "image",
            "--output-file",
            str(self.out),
            "--prompt",
            "actual job selected artifact",
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        job_id = json.loads(run.stdout)["id"]
        run = self.job_command("wait", job_id, "--timeout", "30")
        self.assertEqual(run.returncode, 0, (run.stderr, run.stdout))
        projection = json.loads(run.stdout)
        self.assertEqual(projection["state"], "succeeded", projection)
        return job_id, projection["items"][0]

    def test_actual_job_preview_next_request_both_wires_all_surfaces(self):
        job, item = self.produced_job()
        script = self.home / "preview-job.sh"
        script.write_text(
            f"{shlex.quote(TNY)} image preview --job {job} --item 0 --json\n"
        )
        calls = [
            ("image_preview", {"job": job, "item": 0}),
            ("terminal", {"command": f"tny image preview --job {job} --item 0 --json"}),
            ("terminal", {"command": "sh preview-job.sh"}),
        ]
        for wire in ("chat", "responses"):
            for call in calls:
                with self.subTest(wire=wire, call=call):
                    before = len(self.image_requests())
                    run = self.ask(
                        [call],
                        wire,
                        socket_cli=call[1].get("command") == "sh preview-job.sh",
                    )
                    self.assertEqual(run.returncode, 0, run.stderr)
                    value = self.assert_preview()
                    selected = value["preview"]["selected"]
                    self.assertEqual(selected["sha256"], item["output_sha256"])
                    self.assertEqual(
                        selected["job"],
                        {
                            "id": job,
                            "item_index": 0,
                            "projection_attempt": 1,
                            "item_attempt": 1,
                            "carried_from_attempt": 0,
                            "bytes": item["output_bytes"],
                        },
                    )
                    self.assertEqual(len(self.image_requests()), before)

    def test_actual_job_edit_upload_and_replay_keep_provenance(self):
        job, item = self.produced_job()
        run = self.cli(
            "edit",
            "--job",
            job,
            "--item",
            "0",
            "--output-file",
            "edited.png",
            "--json",
            prompt=b"blue",
        )
        result = self.result(run)
        self.assertEqual(self.uploaded(), self.out.read_bytes())
        manifest = Path(result["manifest_path"])
        record = json.loads(manifest.read_text())
        ref = record["references"][0]
        expected = {
            "id": job,
            "item_index": 0,
            "projection_attempt": 1,
            "item_attempt": 1,
            "carried_from_attempt": 0,
            "bytes": item["output_bytes"],
        }
        self.assertEqual(ref["job"], expected)
        self.assertEqual(ref["sha256"], item["output_sha256"])
        self.assertEqual(ref["source_operation"], item["operation_id"])
        # A replay may not need the mutable job or its source manifest.
        (self.home / ".tny/jobs" / job / "job.json").unlink()
        Path(item["manifest_path"]).unlink()
        replay = self.result(
            self.cli(
                "replay",
                "--manifest",
                str(manifest),
                "--output-file",
                "replayed.png",
                "--json",
            )
        )
        self.assertEqual(self.uploaded(), self.out.read_bytes())
        replayed = json.loads(Path(replay["manifest_path"]).read_text())
        self.assertEqual(replayed["references"][0]["job"], expected)
        self.assertEqual(
            replayed["references"][0]["sha256"], sha(self.out.read_bytes())
        )

    def test_no_manifest_job_identity_preview_and_edit(self):
        run = self.job_command(
            "submit",
            "image",
            "--output-file",
            str(self.out),
            "--prompt",
            "private artifact",
            "--no-manifest",
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        job = json.loads(run.stdout)["id"]
        run = self.job_command("wait", job, "--timeout", "30")
        self.assertEqual(run.returncode, 0, run.stdout)
        item = json.loads(run.stdout)["items"][0]
        self.assertIsNone(item["manifest_path"])
        self.assertEqual(item["output_sha256"], sha(self.out.read_bytes()))
        self.assertNotEqual(item["operation_id"], None)
        before = len(self.image_requests())
        self.ask([("image_preview", {"job": job, "item": 0})])
        value = self.assert_preview()
        self.assertIsNone(value["preview"]["selected"]["manifest_path"])
        self.assertEqual(value["preview"]["selected"]["job"]["id"], job)
        self.assertEqual(len(self.image_requests()), before)
        edited = self.result(
            self.cli(
                "edit",
                "--job",
                job,
                "--item",
                "0",
                "--output-file",
                "private-edit.png",
                "--json",
                prompt=b"blue",
            )
        )
        ref = json.loads(Path(edited["manifest_path"]).read_text())["references"][0]
        self.assertIsNone(ref["source_manifest"])
        self.assertEqual(ref["job"]["id"], job)
        self.assertEqual(self.uploaded(), self.out.read_bytes())

    def test_no_manifest_preview_checks_captured_length_all_callers(self):
        run = self.job_command(
            "submit",
            "image",
            "--output-file",
            str(self.out),
            "--prompt",
            "length fixture",
            "--no-manifest",
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        job = json.loads(run.stdout)["id"]
        run = self.job_command("wait", job, "--timeout", "30")
        self.assertEqual(run.returncode, 0, run.stdout)
        path = self.home / ".tny/jobs" / job / "job.json"
        record = json.loads(path.read_text())
        record["items"][0]["output_bytes"] += 1
        path.write_text(json.dumps(record))
        script = self.home / "length-preview.sh"
        script.write_text(
            f"{shlex.quote(TNY)} image preview --job {job} --item 0 --json\n"
        )
        calls = [
            ("image_preview", {"job": job, "item": 0}),
            ("terminal", {"command": f"tny image preview --job {job} --item 0 --json"}),
            ("terminal", {"command": "sh length-preview.sh"}),
        ]
        before = len(self.image_requests())
        for wire in ("chat", "responses"):
            for call in calls:
                with self.subTest(wire=wire, call=call):
                    self.ask(
                        [call],
                        wire,
                        socket_cli=call[1].get("command") == "sh length-preview.sh",
                    )
                    self.assertEqual(self.pixels(self.state["chat"][-1]), [])
                    self.assertEqual(
                        self.result_object()["preview"]["status"], "failed"
                    )
                    self.assertEqual(
                        self.result_object()["preview"]["error_code"],
                        "byte_count_mismatch",
                    )
        self.assertEqual(len(self.image_requests()), before)

    def test_typed_job_submit_advertises_and_honors_no_manifest(self):
        self.ask(
            [
                (
                    "job_submit",
                    {
                        "kind": "image",
                        "items": [
                            {
                                "prompt": "typed no manifest",
                                "output_file": str(self.out),
                                "persist_manifest": False,
                            }
                        ],
                    },
                )
            ]
        )
        schema = next(
            tool["function"]["parameters"]
            for tool in self.state["chat"][0]["tools"]
            if tool["function"]["name"] == "job_submit"
        )
        self.assertEqual(
            schema["properties"]["items"]["items"]["properties"]["persist_manifest"][
                "type"
            ],
            "boolean",
        )
        job = self.result_object()["id"]
        run = self.job_command("wait", job, "--timeout", "30")
        self.assertEqual(run.returncode, 0, (run.stdout, run.stderr))
        item = json.loads(run.stdout)["items"][0]
        self.assertIsNone(item["manifest_path"])
        self.assertEqual(item["output_sha256"], sha(self.out.read_bytes()))

    def test_selected_job_capture_survives_same_batch_replacement(self):
        job, _ = self.produced_job()
        original = self.out.read_bytes()
        other = self.home / "later.png"
        other.write_bytes(preview.png(2, 2))
        calls = [
            ("image_preview", {"job": job, "item": 0}),
            (
                "terminal",
                {
                    "command": f"cp {shlex.quote(str(other))} {shlex.quote(str(self.out))}"
                },
            ),
        ]
        for wire in ("chat", "responses"):
            self.out.write_bytes(original)
            before = len(self.image_requests())
            self.ask(calls, wire)
            self.assert_preview()
            self.assertEqual(self.out.read_bytes(), other.read_bytes())
            self.assertEqual(len(self.image_requests()), before)

    def test_real_carried_success_reports_attempt_one_under_projection_three(self):
        original = self.state["image"]

        def choose(body):
            self.state["image"] = (
                original
                if body["prompt"] == "carried success"
                else b"invalid image fixture"
            )

        self.state["on_image"] = choose
        request = self.home / "batch.json"
        request.write_text(
            json.dumps(
                {
                    "kind": "image",
                    "concurrency": 1,
                    "items": [
                        {"prompt": "carried success", "output_file": str(self.out)},
                        {
                            "prompt": "retry failure",
                            "output_file": str(self.home / "failed.png"),
                        },
                    ],
                }
            )
        )
        run = self.job_command("submit", "batch", "--request", str(request))
        self.assertEqual(run.returncode, 0, run.stderr)
        job = json.loads(run.stdout)["id"]
        for attempt in (1, 2, 3):
            if attempt > 1:
                retry = self.job_command("retry", job, "--failed")
                self.assertEqual(retry.returncode, 0, (retry.stdout, retry.stderr))
            run = self.job_command("wait", job, "--timeout", "30")
            self.assertEqual(run.returncode, 2, run.stdout)
        stored = json.loads((self.home / ".tny/jobs" / job / "job.json").read_text())
        self.assertEqual(stored["attempt"], 3)
        self.assertEqual(stored["items"][0]["attempt"], 1)
        self.assertEqual(stored["items"][0]["carried_from_attempt"], 1)
        self.state["image"] = original
        for wire in ("chat", "responses"):
            before = len(self.image_requests())
            self.ask([("image_preview", {"job": job, "item": 0})], wire)
            value = self.assert_preview()
            self.assertEqual(
                value["preview"]["selected"]["job"],
                {
                    "id": job,
                    "item_index": 0,
                    "projection_attempt": 3,
                    "item_attempt": 1,
                    "carried_from_attempt": 1,
                    "bytes": len(original),
                },
            )
            self.assertEqual(len(self.image_requests()), before)

    def test_malformed_stored_job_provenance_refuses_replay_without_request(self):
        job, _ = self.produced_job()
        result = self.result(
            self.cli(
                "edit",
                "--job",
                job,
                "--item",
                "0",
                "--output-file",
                "lineage.png",
                "--json",
                prompt=b"blue",
            )
        )
        path = Path(result["manifest_path"])
        original = json.loads(path.read_text())
        before = len(self.image_requests())
        for bad in (
            None,
            False,
            [],
            {},
            {"id": job},
            dict(original["references"][0]["job"], item_index=True),
            dict(original["references"][0]["job"], item_attempt=1.0),
            dict(original["references"][0]["job"], projection_attempt=0),
            dict(original["references"][0]["job"], carried_from_attempt=1),
            dict(original["references"][0]["job"], bytes="9"),
        ):
            with self.subTest(provenance=bad):
                record = json.loads(json.dumps(original))
                record["references"][0]["job"] = bad
                path.write_text(json.dumps(record))
                run = self.cli(
                    "replay",
                    "--manifest",
                    str(path),
                    "--output-file",
                    "invalid-lineage.png",
                    "--json",
                )
                self.assertEqual(run.returncode, 1, run.stdout)
        self.assertEqual(len(self.image_requests()), before)
        # Absence remains a valid old reference record.
        del original["references"][0]["job"]
        path.write_text(json.dumps(original))
        self.result(
            self.cli(
                "replay",
                "--manifest",
                str(path),
                "--output-file",
                "old-lineage.png",
                "--json",
            )
        )

    def test_job_selector_incomplete_wrong_command_and_wrong_types_refuse(self):
        job, _ = self.produced_job()
        before = len(self.image_requests())
        invalid = [
            ("preview", "--job", job),
            ("preview", "--item", "0"),
            ("preview", "--job", job, "--item", "0.0"),
            ("preview", "--job", job, "--item", "-1"),
            ("generate", "--job", job, "--item", "0", "--output-file", "bad.png"),
            ("replay", "--job", job, "--item", "0", "--output-file", "bad.png"),
            ("edit", "--job", job, "--output-file", "bad.png"),
        ]
        for argv in invalid:
            with self.subTest(argv=argv):
                run = self.cli(*argv, prompt=b"invalid selector")
                self.assertEqual(run.returncode, 1, run.stdout)
        for args in (
            {"job": job},
            {"item": 0},
            {"job": job, "item": False},
            {"job": job, "item": 0.0},
            {"job": job, "item": "0"},
        ):
            with self.subTest(args=args):
                self.ask([("image_preview", args)])
                self.assertEqual(self.pixels(self.state["chat"][-1]), [])
                self.assertTrue(self.results()[0].startswith("error:"), self.results())
        self.assertEqual(len(self.image_requests()), before)

    def pending_selection(self, operation, caller, change, wire):
        job, item = self.produced_job()
        path = self.home / ".tny/jobs" / job / "job.json"
        manifest = Path(item["manifest_path"])
        original = self.out.read_bytes()
        saved_job, saved_manifest = path.read_bytes(), manifest.read_bytes()
        replacement = self.root / "replacement.png"
        replacement.write_bytes(preview.png(2, 2))
        output = self.root / "approved.png"
        if operation == "preview":
            call = ("image_preview", {"job": job, "item": 0})
            command = f"tny image preview --job {job} --item 0 --json"
        else:
            call = (
                "image_edit",
                {
                    "job": job,
                    "item": 0,
                    "prompt": "approved",
                    "output_file": str(output),
                },
            )
            command = f"printf approved | tny image edit --job {job} --item 0 --output-file {shlex.quote(str(output))} --json"
        if caller == "terminal":
            call = ("terminal", {"command": command})
        self.state["calls"] = [call]
        self.state["chat"].clear()
        proc = subprocess.Popen(
            [
                self.pending,
                str(self.root),
                self.url + "/v1",
                self.url + "/backend-api/codex",
                wire,
            ],
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

        reader = threading.Thread(target=read, daemon=True)
        reader.start()

        def receive():
            line = messages.get(timeout=30)
            self.assertIsNotNone(line, "pending fixture closed stdout")
            return json.loads(line)

        def send(text):
            proc.stdin.write(text + "\n")
            proc.stdin.flush()

        try:
            summaries = []
            for turn in range(2):
                event = receive()
                self.assertEqual(event["event"], "permission", event)
                summaries.append(event["summary"])
                self.assertIn(job, event["summary"])
                before = len(self.image_requests())
                if turn == 0:
                    if change == "delete":
                        path.unlink()
                        manifest.unlink()
                    elif change == "mapping":
                        changed = json.loads(saved_job)
                        changed["attempt"] = 2
                        changed["items"][0].update(
                            attempt=2,
                            output_path=str(replacement),
                            output_sha256=sha(replacement.read_bytes()),
                            output_bytes=replacement.stat().st_size,
                        )
                        path.write_text(json.dumps(changed))
                        changed = json.loads(saved_manifest)
                        changed["output"] = str(replacement)
                        changed["artifacts"][0].update(
                            path=str(replacement), sha256=sha(replacement.read_bytes())
                        )
                        changed["result"]["bytes"] = replacement.stat().st_size
                        manifest.write_text(json.dumps(changed))
                    elif change == "bytes":
                        self.out.write_bytes(replacement.read_bytes())
                send("allow")
                end = receive()
                self.assertEqual(end["event"], "ended", end)
                self.assertEqual(end["grants"], 0)
                self.assertEqual(end["questions"], 1)
                failed = turn == 0 and change == "bytes"
                self.assertEqual(
                    len(self.image_requests()) - before,
                    int(operation == "edit" and not failed),
                )
                if operation == "preview":
                    self.assertEqual(
                        self.pixels(self.state["chat"][-1]),
                        [] if failed else [original],
                    )
                elif not failed:
                    self.assertEqual(self.uploaded(), original)
                    recorded = json.loads(
                        next(
                            self.home.glob("approved.png.tny-image-*.json")
                        ).read_text()
                    )
                    self.assertEqual(
                        recorded["references"][0]["job"]["projection_attempt"], 1
                    )
                if turn == 0:
                    path.write_bytes(saved_job)
                    manifest.write_bytes(saved_manifest)
                    self.out.write_bytes(original)
                    output.unlink(missing_ok=True)
                    for record in self.home.glob("approved.png.tny-image-*.json"):
                        record.unlink()
                    self.state["chat"].clear()
                    send("next")
            self.assertEqual(summaries[0], summaries[1])
            proc.stdin.close()
            self.assertEqual(proc.wait(timeout=30), 0, proc.stderr.read())
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=10)
            for stream in (proc.stdin, proc.stdout, proc.stderr):
                stream.close()
            reader.join(timeout=5)

    def test_job_permission_ownership_and_one_time_grants(self):
        for operation in ("preview", "edit"):
            for caller in ("typed", "terminal"):
                for change in ("delete", "mapping", "bytes"):
                    with self.subTest(
                        operation=operation, caller=caller, change=change
                    ):
                        case = type(self)(
                            "test_job_permission_ownership_and_one_time_grants"
                        )
                        case.setUp()
                        try:
                            case.pending_selection(operation, caller, change, "chat")
                        finally:
                            case.tearDown()


if WASM:

    class JobArtifactWasm(ImageFixture):
        def test_selectors_refuse_without_a_job_or_provider_request(self):
            job = "0" * 32
            for args in (
                ("preview", "--job", job, "--item", "0", "--json"),
                (
                    "edit",
                    "--job",
                    job,
                    "--item",
                    "0",
                    "--output-file",
                    "blocked.png",
                    "--json",
                ),
            ):
                with self.subTest(args=args):
                    run = self.cli(*args, prompt=b"not sent")
                    self.assertEqual(run.returncode, 1, run.stdout)
                    self.assertIn(b"unsupported", run.stderr)
            self.assertEqual(self.image_requests(), [])
            self.assertFalse((self.home / ".tny/jobs").exists())

        def test_recorded_job_provenance_replay_stays_shared(self):
            source = self.home / "source.png"
            source.write_bytes(self.state["image"])
            created = self.result(
                self.cli(
                    "edit",
                    "--image",
                    str(source),
                    "--output-file",
                    "prior.png",
                    "--json",
                    prompt=b"recorded",
                )
            )
            manifest = Path(created["manifest_path"])
            record = json.loads(manifest.read_text())
            job = {
                "id": "0123456789abcdef0123456789abcdef",
                "item_index": 0,
                "projection_attempt": 3,
                "item_attempt": 1,
                "carried_from_attempt": 1,
                "bytes": source.stat().st_size,
            }
            record["references"][0]["job"] = job
            manifest.write_text(json.dumps(record))
            replayed = self.result(
                self.cli(
                    "replay",
                    "--manifest",
                    str(manifest),
                    "--output-file",
                    "replayed.png",
                    "--json",
                )
            )
            self.assertEqual(self.uploaded(), source.read_bytes())
            final = json.loads(Path(replayed["manifest_path"]).read_text())
            self.assertEqual(final["references"][0]["job"], job)
            self.assertFalse((self.home / ".tny/jobs").exists())


if __name__ == "__main__":
    if WASM:
        result = unittest.TextTestRunner(verbosity=2).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(JobArtifactWasm)
        )
        sys.exit(0 if result.wasSuccessful() else 1)
    unittest.main(argv=argv_without_runner_binary())
