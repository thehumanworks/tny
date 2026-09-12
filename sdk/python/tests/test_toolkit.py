from __future__ import annotations

import asyncio
import os
import sys
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest.mock import patch

import tny

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/fixtures"))
from toolkit_provider import (  # noqa: E402
    ACCOUNT,
    MP3,
    OPTIMISE_TOKEN,
    PNG,
    READ_TOOLS,
    REQUEST_ID,
    SEED,
    TEXT,
    TOKEN,
    Provider,
    fake_audio,
    workspace,
)

LIBRARY = os.environ.get(
    "TNY_TEST_LIBRARY",
    str(
        ROOT
        / "build/lib"
        / ("libtny.1.dylib" if sys.platform == "darwin" else "libtny.so.1")
    ),
)


class ToolkitTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name).resolve()
        workspace(self.path)
        self.provider = Provider()
        self.addCleanup(self.provider.close)
        self.config = tny.ToolkitConfig(
            workspace=self.path,
            settings_path="settings.json",
            chatgpt_token=TOKEN,
            chatgpt_account_id=ACCOUNT,
            codex_base_url=self.provider.url + "/backend-api/codex",
        )
        self.toolkit = tny.Toolkit(self.config, library=LIBRARY)

    def optimise_options(self):
        return {
            "provider": "openai",
            "model": "fixture-model",
            "base_url": self.provider.url + "/v1",
            "api_key": OPTIMISE_TOKEN,
            "wire_api": "chat",
        }

    def staging(self, stem):
        """Temporary files left beside an output, which must always be none."""
        return sorted(
            p.name for p in self.path.glob(stem + ".*") if ".tny-image-" not in p.name
        )

    def records(self, stem):
        import json

        return [
            json.loads(p.read_text())
            for p in sorted(self.path.glob(stem + ".tny-image-*.json"))
        ]

    def test_preview_misuse_is_rejected_without_provider_io(self):
        toolkit = self.toolkit
        for preview in (True, False, "true"):
            with self.subTest(preview=preview), self.assertRaises(TypeError):
                toolkit.generate_image("tree", output_file="out.png", preview=preview)
            with self.subTest(edit=preview), self.assertRaises(TypeError):
                toolkit.edit_image(
                    "tree", output_file="out.png", images=["in.png"], preview=preview
                )
        self.assertEqual(self.provider.requests, [])

    def test_images_export_edit_and_options(self):
        cwd = Path.cwd()
        result = self.toolkit.generate_image(
            "A tree",
            output_file="out.png",
            model="fixture-image",
            quality="max",
            size="auto",
        )
        self.assertEqual(result.path, self.path / "out.png")
        self.assertEqual(result.path.read_bytes(), PNG)
        self.assertEqual(result.byte_count, len(PNG))
        self.assertEqual(result.mime_type, "image/png")
        self.assertEqual(result.provider, "codex")
        # Dimensions are read from the returned bytes, not from the request.
        self.assertEqual((result.width, result.height), (1, 1))
        self.assertEqual(result.requested_size, "auto")
        self.assertEqual(result.effective_size, "auto")
        self.assertEqual(result.size_status, "auto")
        self.toolkit.edit_image(
            b"Make it blue", images=[result.path], output_file="out.png"
        )
        request = self.provider.requests[-1]
        self.assertEqual(request["path"], "/backend-api/codex/images/edits")
        self.assertTrue(
            request["body"]["images"][0]["image_url"].startswith(
                "data:image/png;base64,"
            )
        )
        self.assertEqual(Path.cwd(), cwd)
        self.assertFalse((self.path / ".tny").exists())
        # No staging copy survives; the two per-operation records do, and are
        # the only `out.png.*` files (docs/images.md).
        self.assertEqual(
            sorted(
                p.name
                for p in self.path.glob("out.png.*")
                if ".tny-image-" not in p.name
            ),
            [],
        )
        self.assertEqual(len(list(self.path.glob("out.png.tny-image-*.json"))), 2)

    def test_image_manifests_carry_lineage_and_honour_the_opt_out(self):
        import hashlib
        import json
        import stat

        result = self.toolkit.generate_image("A tree", output_file="out.png")
        self.assertIsNotNone(result.manifest_path)
        self.assertEqual(result.manifest_path.parent, self.path)
        self.assertRegex(result.operation_id, r"^[0-9a-f]{16}$")
        record = json.loads(result.manifest_path.read_text())
        self.assertEqual(record["version"], 1)
        self.assertEqual(record["kind"], "image_manifest")
        self.assertEqual(record["status"], "succeeded")
        self.assertEqual(record["prompt"], "A tree")
        self.assertEqual(record["operation_id"], result.operation_id)
        self.assertEqual(
            record["artifacts"][0]["sha256"], hashlib.sha256(PNG).hexdigest()
        )
        self.assertEqual(stat.S_IMODE(result.manifest_path.stat().st_mode), 0o600)
        # Absent provider identifiers stay absent.
        self.assertIsNone(result.seed)
        self.assertIsNone(result.request_id)
        self.assertIsNone(record["actual"]["seed"])
        self.assertIsNone(record["actual"]["request_id"])
        # An earlier artifact becomes a verified reference through the shared
        # resolver, with its source recorded.
        edited = self.toolkit.edit_image(
            "Make it blue", artifact=str(result.manifest_path), output_file="edit.png"
        )
        request = self.provider.requests[-1]
        self.assertEqual(request["path"], "/backend-api/codex/images/edits")
        reference = json.loads(edited.manifest_path.read_text())["references"][0]
        self.assertEqual(reference["sha256"], hashlib.sha256(PNG).hexdigest())
        self.assertEqual(reference["source_manifest"], str(result.manifest_path))
        self.assertEqual(reference["source_operation"], result.operation_id)
        # A rerun reuses the recorded prompt without one being supplied.
        rerun = self.toolkit.generate_image(
            from_manifest=str(result.manifest_path), output_file="again.png"
        )
        self.assertEqual(self.provider.requests[-1]["body"]["prompt"], "A tree")
        self.assertEqual(
            json.loads(rerun.manifest_path.read_text())["source"]["operation_id"],
            result.operation_id,
        )
        # The opt-out records nothing at all, not even the prompt.
        quiet = self.toolkit.generate_image(
            "A private tree", output_file="quiet.png", persist_manifest=False
        )
        self.assertIsNone(quiet.manifest_path)
        self.assertEqual(list(self.path.glob("quiet.png.*")), [])
        for path in self.path.rglob("*"):
            if path.is_file():
                self.assertNotIn(b"A private tree", path.read_bytes(), path)

    def test_provider_identifiers_are_preserved_only_when_returned(self):
        import json

        self.provider.mode = "identified"
        result = self.toolkit.generate_image("A tree", output_file="out.png")
        self.assertEqual(result.seed, SEED)
        self.assertEqual(result.request_id, REQUEST_ID)
        record = json.loads(result.manifest_path.read_text())
        self.assertEqual(record["actual"], {"seed": SEED, "request_id": REQUEST_ID})

    def test_speech_export_and_file_transcription(self):
        result = self.toolkit.speak("Hello", output_file="speech.mp3", voice="cove")
        self.assertFalse(result.played)
        self.assertEqual(result.path.read_bytes(), MP3)
        transcript = self.toolkit.transcribe("input.wav", provider="codex")
        self.assertEqual(transcript.text, TEXT.encode())
        self.assertNotIn(TEXT, repr(transcript))
        self.assertTrue(self.provider.requests[-1]["body"]["wav"])

    def test_microphone_and_playback_with_fake_host_devices(self):
        binaries = self.path / "bin"
        fake_audio(binaries)
        with patch.dict(os.environ, {"PATH": str(binaries)}):
            result = self.toolkit.dictate(seconds=1, provider="codex")
            self.assertEqual(result.text, TEXT.encode())
            speech = self.toolkit.speak("Hello")
            self.assertTrue(speech.played)
            self.assertIsNone(speech.path)

    def test_optimise_reads_workspace_without_submitting_or_writing(self):
        self.provider.mode = "explore"
        result = self.toolkit.optimise(
            "Improve the context file", **self.optimise_options()
        )
        self.assertEqual(result.text, TEXT.encode())
        self.assertEqual(result.provider, "openai")
        self.assertEqual(result.model, "fixture-model")
        self.assertEqual(len(self.provider.requests), 2)
        bodies = [r["body"] for r in self.provider.requests]
        self.assertEqual(
            {tool["function"]["name"] for tool in bodies[0]["tools"]}, READ_TOOLS
        )
        self.assertIn("UTF-8 fixture context", str(bodies[1]["messages"]))
        self.assertNotIn(TEXT, repr(result))
        self.provider.mode = "write"
        self.toolkit.optimize("Improve the context file", **self.optimise_options())
        self.assertEqual(
            (self.path / "src/context.txt").read_text(), "UTF-8 fixture context\n"
        )

    def test_errors_preserve_files_and_do_not_echo_credentials(self):
        output = self.path / "out.png"
        output.write_bytes(b"preserve")
        for mode in ("error", "invalid"):
            self.provider.mode = mode
            with self.assertRaises(tny.TnyError) as raised:
                self.toolkit.generate_image("private prompt", output_file=output)
            self.assertNotIn(TOKEN, str(raised.exception))
            self.assertNotIn(TOKEN.encode(), raised.exception.message)
            self.assertEqual(output.read_bytes(), b"preserve")
            # Provider failures carry no structured detail at all.
            self.assertIsNone(raised.exception.image_detail)
        self.assertNotIn(TOKEN, repr(self.config))
        self.assertNotIn(TOKEN, repr(self.toolkit))

    def test_strict_size_mismatch_keeps_the_previous_image(self):
        output = self.path / "out.png"
        output.write_bytes(b"preserve")
        # The fixture image is 1x1, so an exact 1024x1024 request cannot be met.
        with self.assertRaises(tny.ProtocolError) as raised:
            self.toolkit.generate_image(
                "tree", output_file="out.png", size="1024x1024", strict_size=True
            )
        # The same typed exception carries the locally decided reason; the
        # generic text, repr and traceback stay exactly as they were.
        detail = raised.exception.image_detail
        self.assertIsInstance(detail, tny.ImageFailureDetail)
        self.assertEqual(detail.code, "IMAGE_SIZE_MISMATCH")
        self.assertEqual(detail.operation, "generate")
        self.assertEqual(detail.requested_size, "1024x1024")
        self.assertEqual(detail.effective_size, "1024x1024")
        self.assertEqual((detail.width, detail.height), (1, 1))
        self.assertEqual(detail.size_status, "mismatch")
        self.assertEqual(detail.mime_type, "image/png")
        self.assertIsNone(detail.path)
        self.assertFalse(detail.committed)
        printed = str(raised.exception) + repr(raised.exception)
        for value in ("1024x1024", "IMAGE_SIZE_MISMATCH", TOKEN, "tree"):
            self.assertNotIn(value, printed)
        for value in (TOKEN, ACCOUNT, self.provider.url, str(output), "tree"):
            self.assertNotIn(value, repr(detail))
        with self.assertRaises(AttributeError):  # read-only
            raised.exception.image_detail = None
        self.assertEqual(output.read_bytes(), b"preserve")
        self.assertEqual(len(self.provider.requests), 1)  # paid once, never retried
        self.assertEqual(self.staging("out.png"), [])
        # The paid attempt is recorded as a failure that claims no artifact.
        record = self.records("out.png")[0]
        self.assertEqual(len(self.records("out.png")), 1)
        self.assertEqual(record["status"], "failed")
        self.assertFalse(record["committed"])
        self.assertEqual(record["artifacts"], [])
        result = self.toolkit.generate_image(
            "tree", output_file="out.png", size="1x1", strict_size=True
        )
        self.assertEqual(result.size_status, "match")
        self.assertEqual((result.width, result.height), (1, 1))
        self.assertEqual(output.read_bytes(), PNG)

    def test_strict_size_preflight_detail_costs_nothing(self):
        with self.assertRaises(tny.InvalidArgumentError) as raised:
            self.toolkit.generate_image(
                "tree", output_file="out.png", size="portrait", strict_size=True
            )
        detail = raised.exception.image_detail
        self.assertEqual(detail.code, "IMAGE_STRICT_SIZE_INVALID")
        self.assertEqual(detail.requested_size, "portrait")
        # Nothing was sent, so no wire size and no dimensions are invented.
        self.assertIsNone(detail.effective_size)
        self.assertIsNone(detail.width)
        self.assertIsNone(detail.height)
        self.assertIsNone(detail.mime_type)
        self.assertFalse(detail.committed)
        self.assertEqual(self.provider.requests, [])
        self.assertFalse((self.path / "out.png").exists())

    def test_validation_happens_before_requests(self):
        calls = [
            lambda: self.toolkit.generate_image("", output_file="out.png"),
            lambda: self.toolkit.generate_image("bad\0text", output_file="out.png"),
            lambda: self.toolkit.generate_image(b"\xff", output_file="out.png"),
            lambda: self.toolkit.edit_image("edit", images=[], output_file="out.png"),
            # Strict size needs an exact WIDTHxHEIGHT before anything is paid for.
            lambda: self.toolkit.generate_image(
                "tree", output_file="out.png", strict_size=True
            ),
            lambda: self.toolkit.generate_image(
                "tree", output_file="out.png", size="portrait", strict_size=True
            ),
            lambda: self.toolkit.dictate(seconds=True),
            lambda: self.toolkit.dictate(seconds=0),
            lambda: self.toolkit.dictate(seconds=301),
            lambda: self.toolkit.optimise("text", timeout_seconds=0),
        ]
        for call in calls:
            with self.assertRaises(tny.InvalidArgumentError):
                call()
        self.assertEqual(self.provider.requests, [])

    def test_cancellation_before_and_during_io(self):
        token = tny.CancellationToken()
        token.cancel()
        with self.assertRaises(tny.CancelledError):
            self.toolkit.speak("hello", output_file="speech.mp3", cancellation=token)
        self.assertEqual(self.provider.requests, [])
        self.provider.mode = "stall"
        token = tny.CancellationToken()
        with ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(
                self.toolkit.generate_image,
                "tree",
                output_file="out.png",
                cancellation=token,
            )
            self.assertTrue(self.provider.arrived.wait(5))
            token.cancel()
            with self.assertRaises(tny.CancelledError) as raised:
                future.result(timeout=5)
        self.assertIsNone(raised.exception.image_detail)
        self.assertFalse((self.path / "out.png").exists())
        self.assertEqual(self.staging("out.png"), [])
        # A cancelled operation leaves a terminal record, never a live one.
        for record in self.records("out.png"):
            self.assertIn(record["status"], ("cancelled", "failed"))
            self.assertFalse(record["committed"])

    def test_image_detail_types_are_exported_and_discriminated(self):
        # Both shapes are public names, and the union is too.
        for name in ("ImageFailureDetail", "RetainedImageDetail", "ImageDetail"):
            self.assertIn(name, tny.__all__)
            self.assertIs(getattr(tny, name), getattr(tny.toolkit, name))
        self.assertEqual(
            set(tny.ImageDetail.__args__),
            {tny.ImageFailureDetail, tny.RetainedImageDetail},
        )
        self.assertEqual(
            tny.toolkit.RETAINED_IMAGE_CODE, "IMAGE_MANIFEST_FINALIZE_FAILED"
        )
        # A retained object is never accepted as a strict rejection, and the
        # strict shape is never read as a retained artifact.
        parse = tny.toolkit._image_failure_detail
        retained = {
            "kind": "image",
            "ok": False,
            "operation": "generate",
            "code": "IMAGE_MANIFEST_FINALIZE_FAILED",
            "error": "kept",
            "mime_type": "image/png",
            "requested_size": "auto",
            "effective_size": "auto",
            "width": 1,
            "height": 1,
            "size_status": "auto",
            "path": "/w/out.png",
            "committed": True,
            "bytes": 70,
            "operation_id": "1234abcd1234abcd",
            "manifest_path": "/w/out.png.tny-image-1234abcd1234abcd.json",
        }
        import json

        self.assertIsInstance(
            parse(json.dumps(retained).encode()), tny.RetainedImageDetail
        )
        from dataclasses import FrozenInstanceError, fields, replace
        from typing import Literal, get_type_hints

        strict = {
            **retained,
            "code": "IMAGE_SIZE_MISMATCH",
            "committed": False,
            "path": None,
        }
        for value, cls, literal in (
            (retained, tny.RetainedImageDetail, Literal[True]),
            (strict, tny.ImageFailureDetail, Literal[False]),
        ):
            detail = parse(json.dumps(value).encode())
            self.assertIsInstance(detail, cls)
            self.assertIs(detail.committed, value["committed"])
            self.assertEqual(get_type_hints(cls)["committed"], literal)
            self.assertFalse(next(f for f in fields(cls) if f.name == "committed").init)
            kwargs = {f.name: getattr(detail, f.name) for f in fields(cls) if f.init}
            for opposite in (True, False):
                with self.assertRaises(TypeError):
                    cls(**kwargs, committed=opposite)
            # Python 3.14 uses TypeError here; older supported versions use ValueError.
            with self.assertRaises((ValueError, TypeError)):
                replace(detail, committed=not detail.committed)
            with self.assertRaises(FrozenInstanceError):
                detail.committed = not detail.committed
        # A retained shape that claims no file, or a strict code with a
        # committed path, is not a valid detail at all.
        for bad in (
            {**retained, "path": None},
            {**retained, "committed": False},
            {**retained, "code": "IMAGE_SIZE_MISMATCH"},
            {**retained, "bytes": "70"},
            {k: v for k, v in retained.items() if k != "bytes"},
        ):
            self.assertIsNone(parse(json.dumps(bad).encode()))

    def obstruct_manifest(self, stem):
        """Turn this operation's live record into a directory, mid-request.

        The running record already exists and the output is not yet committed,
        so only the finalizing rename can fail. Nothing in the product is
        mocked: this is a real filesystem failure on a real path.
        """
        self.assertTrue(self.provider.arrived.wait(5))
        running = sorted(self.path.glob(stem + ".tny-image-*.json"))
        self.assertEqual(len(running), 1, running)
        running[0].unlink()
        running[0].mkdir()
        self.provider.release.set()
        return running[0]

    def test_manifest_finalization_failure_keeps_the_paid_image(self):
        self.provider.mode = "stall"
        self.provider.arrived.clear()
        with ThreadPoolExecutor(max_workers=1) as executor:
            call = executor.submit(
                self.toolkit.generate_image, "tree", output_file="out.png"
            )
            record = self.obstruct_manifest("out.png")
            with self.assertRaises(tny.TnyIOError) as raised:
                call.result(timeout=15)
        detail = raised.exception.image_detail
        # A distinct type, never the strict shape and never an ImageResult.
        self.assertIsInstance(detail, tny.RetainedImageDetail)
        self.assertNotIsInstance(detail, tny.ImageFailureDetail)
        self.assertEqual(detail.code, "IMAGE_MANIFEST_FINALIZE_FAILED")
        self.assertEqual(detail.operation, "generate")
        self.assertTrue(detail.committed)
        self.assertEqual(detail.path, self.path / "out.png")
        self.assertEqual(detail.byte_count, len(PNG))
        self.assertEqual((detail.width, detail.height), (1, 1))
        self.assertEqual(detail.mime_type, "image/png")
        self.assertEqual(detail.size_status, "auto")
        self.assertRegex(detail.operation_id, r"^[0-9a-f]{16}$")
        self.assertEqual(detail.manifest_path, record)
        # The paid artifact is exactly the provider's bytes, and kept.
        self.assertEqual((self.path / "out.png").read_bytes(), PNG)
        self.assertEqual(self.staging("out.png"), [])
        printed = str(raised.exception) + repr(raised.exception)
        for value in ("IMAGE_MANIFEST_FINALIZE_FAILED", TOKEN, "tree", str(record)):
            self.assertNotIn(value, printed)
        for value in (TOKEN, ACCOUNT, self.provider.url, "tree"):
            self.assertNotIn(value, repr(detail))
        with self.assertRaises(AttributeError):  # read-only
            raised.exception.image_detail = None

    def test_concurrent_jobs_use_independent_native_state(self):
        with ThreadPoolExecutor(max_workers=3) as executor:
            calls = [
                executor.submit(
                    self.toolkit.generate_image, "tree", output_file=f"out-{i}.png"
                )
                for i in range(3)
            ]
            results = [f.result(timeout=5) for f in calls]
        self.assertEqual(len({r.path for r in results}), 3)
        self.assertTrue(all(r.path.read_bytes() == PNG for r in results))

    def test_abi_gate_keeps_legacy_agent_library_usable(self):
        library = tny.Library(LIBRARY)
        library.abi_minor = 1
        with self.assertRaises(tny.UnsupportedError):
            tny.Toolkit(self.config, library=library)

    def test_async_results_and_cancellation_join(self):
        async def exercise():
            toolkit = tny.AsyncToolkit(self.config, library=LIBRARY)
            image, speech, transcript = await asyncio.gather(
                toolkit.generate_image("tree", output_file="async.png"),
                toolkit.speak("hi", output_file="async.mp3"),
                toolkit.transcribe("input.wav", provider="codex"),
            )
            self.assertEqual(image.path.read_bytes(), PNG)
            self.assertEqual(speech.path.read_bytes(), MP3)
            self.assertEqual(transcript.text, TEXT.encode())
            optimised = await toolkit.optimise(
                "Improve context", **self.optimise_options()
            )
            self.assertEqual(optimised.text, TEXT.encode())
            self.provider.mode = "stall"
            self.provider.arrived.clear()
            task = asyncio.create_task(
                toolkit.generate_image("tree", output_file="cancel.png")
            )
            arrived = await asyncio.to_thread(self.provider.arrived.wait, 5)
            self.assertTrue(arrived)
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
            self.assertFalse((self.path / "cancel.png").exists())
            self.assertEqual(self.staging("cancel.png"), [])
            for record in self.records("cancel.png"):
                self.assertEqual(record["status"], "cancelled")

        asyncio.run(exercise())


if __name__ == "__main__":
    unittest.main()
