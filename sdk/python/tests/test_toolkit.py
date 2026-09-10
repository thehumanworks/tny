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
        self.assertEqual(list(self.path.glob("out.png.*")), [])

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
        self.assertNotIn(TOKEN, repr(self.config))
        self.assertNotIn(TOKEN, repr(self.toolkit))

    def test_validation_happens_before_requests(self):
        calls = [
            lambda: self.toolkit.generate_image("", output_file="out.png"),
            lambda: self.toolkit.generate_image("bad\0text", output_file="out.png"),
            lambda: self.toolkit.generate_image(b"\xff", output_file="out.png"),
            lambda: self.toolkit.edit_image("edit", images=[], output_file="out.png"),
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
            with self.assertRaises(tny.CancelledError):
                future.result(timeout=5)
        self.assertFalse((self.path / "out.png").exists())
        self.assertEqual(list(self.path.glob("out.png.*")), [])

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
            self.assertEqual(list(self.path.glob("cancel.png.*")), [])

        asyncio.run(exercise())


if __name__ == "__main__":
    unittest.main()
