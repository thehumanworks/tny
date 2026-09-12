#!/usr/bin/env python3
"""Requested versus actual image dimensions (#122), end to end.

The exact C122 command is

    env -u TNY_TOOLS python3 tests/integration/test_image_workflow.py -k Dimensions

and `tests/integration/run.sh` may append the binary path instead of setting
$TNY; both forms work. Every provider call is a local HTTP fixture with fake
credentials, and the images are generated here, so every asserted width and
height comes from returned bytes rather than from the request.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TNY = str(Path(os.environ.get("TNY", ROOT / "build/tny")).resolve())
WASM = "wasm" in TNY
WINDOWS = sys.platform in ("win32", "cygwin", "msys")
TOKEN = "fixture-image-token"
ACCOUNT = "fixture-image-account"
# The documented adjacent record name, matched here independently of the C
# implementation so a renamed artifact would fail rather than hide.
MANIFEST_NAME = re.compile(r".*\.tny-image-[0-9a-f]+\.json")
UTC_STAMP = re.compile(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ")


def sha(data: bytes) -> str:
    """Independent hash oracle; it never calls into tny."""
    return hashlib.sha256(data).hexdigest()


def png(width: int, height: int) -> bytes:
    """A complete PNG: signature, IHDR, one zlib IDAT and IEND."""

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    rows = b"".join(b"\0" + b"\x20\x60\xa0" * width for _ in range(height))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">II5B", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows, 1))
        + chunk(b"IEND", b"")
    )


def broken_png(
    width: int, height: int, *, truncate: int = 0, ihdr_length: int = 13
) -> bytes:
    """PNG magic bytes with a header that cannot be trusted."""
    payload = struct.pack(">II5B", width, height, 8, 2, 0, 0, 0)
    data = (
        b"\x89PNG\r\n\x1a\n"
        + struct.pack(">I", ihdr_length)
        + b"IHDR"
        + payload
        + b"\0\0\0\0"
    )
    return data[: len(data) - truncate] if truncate else data


def jpeg(width: int, height: int, *, frame: int = 0xC0) -> bytes:
    """Structurally complete JPEG segments; the scan data is filler.

    Only the frame header is inspected, and this fixture exists to prove that
    the header — not the request — decides the reported dimensions.
    """
    app0 = b"\xff\xe0" + struct.pack(">H", 16) + b"JFIF\0\x01\x01\0\0\x01\0\x01\0\0"
    dqt = b"\xff\xdb" + struct.pack(">H", 67) + b"\0" + bytes(range(1, 65))
    sof = (
        bytes([0xFF, frame])
        + struct.pack(">HBHHB", 17, 8, height, width, 3)
        + b"\x01\x11\x00\x02\x11\x01\x03\x11\x01"
    )
    dht = b"\xff\xc4" + struct.pack(">H", 21) + b"\0" + bytes(16) + b"\0\0"
    sos = (
        b"\xff\xda"
        + struct.pack(">H", 12)
        + b"\x03\x01\x00\x02\x11\x03\x11\x00\x3f\x00"
    )
    return b"\xff\xd8" + app0 + dqt + sof + dht + sos + b"\x00" * 16 + b"\xff\xd9"


def webp(kind: str, width: int, height: int) -> bytes:
    """Lossy, lossless or extended WebP containers with real header fields."""

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (
            tag
            + struct.pack("<I", len(payload))
            + payload
            + (b"\0" if len(payload) % 2 else b"")
        )

    if kind == "lossy":
        body = chunk(
            b"VP8 ",
            b"\x10\x00\x00\x9d\x01\x2a"
            + struct.pack("<HH", width & 0x3FFF, height & 0x3FFF)
            + b"\x00" * 8,
        )
    elif kind == "lossless":
        bits = (width - 1) | ((height - 1) << 14)
        body = chunk(b"VP8L", b"\x2f" + struct.pack("<I", bits) + b"\x00" * 6)
    else:
        # The canvas in VP8X is authoritative; the trailing frame deliberately
        # declares different dimensions.
        canvas = struct.pack("<I", 0x10)[:4]
        body = chunk(
            b"VP8X",
            canvas
            + (width - 1).to_bytes(3, "little")
            + (height - 1).to_bytes(3, "little"),
        ) + chunk(
            b"VP8 ",
            b"\x10\x00\x00\x9d\x01\x2a" + struct.pack("<HH", 7, 7) + b"\x00" * 8,
        )
    return b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WEBP" + body


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, status, content_type, body):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        state = self.server.state
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        state["requests"].append((self.path, body))
        if self.path == "/v1/chat/completions":
            self.chat(body)
            return
        if self.path not in (
            "/backend-api/codex/images/generations",
            "/backend-api/codex/images/edits",
        ):
            self.reply(404, "text/plain", b"bad route")
            return
        if (
            self.headers.get("Authorization") != f"Bearer {TOKEN}"
            or self.headers.get("chatgpt-account-id") != ACCOUNT
        ):
            self.reply(401, "application/json", b"{}")
            return
        # A hook runs while the operation is mid-flight: its running record
        # already exists and its output is not yet committed, which is the
        # only moment a controlled provenance fault can be injected without
        # adding a test-only hook to the product.
        hook = state.get("on_image")
        if hook:
            hook(body)
        payload = base64.b64encode(state["image"]).decode()
        reply = {"created": 1, "data": [{"b64_json": payload, **state.get("item", {})}]}
        reply.update(state.get("envelope", {}))
        self.reply(200, "application/json", json.dumps(reply).encode())

    def chat(self, body):
        state = self.server.state
        state["chat"].append(body)
        if any(message.get("role") == "tool" for message in body["messages"]):
            delta, reason = {"content": "done"}, "stop"
        else:
            arguments = dict(state["tool_args"])
            name = state.get("tool_name", "image_generate")
            if state.get("command"):
                name, arguments = "terminal", {"command": state["command"]}
            elif state.get("tool") == "terminal":
                flags = "".join(
                    f" --{key.replace('_', '-')}"
                    if value is True
                    else f" --{key} {value}"
                    for key, value in arguments.items()
                    if key not in ("prompt", "output_file")
                )
                name = "terminal"
                json_flag = " --json" if state.get("json", True) else ""
                arguments = {
                    "command": "printf 'A blue robot' | tny image generate"
                    f"{flags} --output-file {arguments['output_file']}{json_flag}"
                }
            delta = {
                "tool_calls": [
                    {
                        "index": 0,
                        "id": "image-call",
                        "type": "function",
                        "function": {"name": name, "arguments": json.dumps(arguments)},
                    }
                ]
            }
            reason = "tool_calls"
        frames = [
            {"choices": [{"index": 0, "delta": delta, "finish_reason": None}]},
            {"choices": [{"index": 0, "delta": {}, "finish_reason": reason}]},
        ]
        self.reply(
            200,
            "text/event-stream",
            (
                "".join(f"data: {json.dumps(f)}\n\n" for f in frames)
                + "data: [DONE]\n\n"
            ).encode(),
        )


class ImageFixture(unittest.TestCase):
    """Local HTTP provider, fake credentials and a throwaway HOME.

    Every asserted width, height, hash and file mode below is observed here in
    Python, from returned bytes and the filesystem, never from tny's own
    report of what it did.
    """

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="tny-dimensions-")
        self.home = Path(self.tmp.name)
        # Records store canonical paths, and macOS resolves /var to
        # /private/var, so keep the resolved form for path comparisons.
        self.root = Path(os.path.realpath(self.home))
        (self.home / "codex").mkdir()
        self.out = self.home / "result.png"
        self.state = {
            "image": png(1, 1),
            "requests": [],
            "chat": [],
            "tool_args": {},
        }
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.state = self.state
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = f"http://127.0.0.1:{self.server.server_port}"
        self.env = {
            "HOME": str(self.home),
            "CODEX_HOME": str(self.home / "codex"),
            "PATH": os.environ["PATH"],
            "LANG": "C.UTF-8",
            "CHATGPT_ACCESS_TOKEN": TOKEN,
            "CHATGPT_ACCOUNT_ID": ACCOUNT,
            "TNY_CODEX_BASE_URL": self.url + "/backend-api/codex",
            "TNY_ISOLATE": "0",
        }

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.tmp.cleanup()

    def generate(self, *args, prompt=b"An orange robot", output=None):
        return subprocess.run(
            [TNY, "image", "generate", "--output-file", str(output or self.out), *args],
            input=prompt,
            cwd=self.home,
            env=self.env,
            capture_output=True,
            timeout=60,
        )

    def image_requests(self):
        return [r for r in self.state["requests"] if "/images/" in r[0]]

    def result(self, run):
        self.assertEqual(run.returncode, 0, run.stderr)
        return json.loads(run.stdout)

    def failure(self, run):
        self.assertEqual(run.returncode, 1, run.stderr)
        return json.loads(run.stdout)

    def leftovers(self):
        """Staging files left behind, which must always be none.

        Per-operation manifests are the documented new default artifact
        (docs/images.md), so they are listed separately by `manifests()`
        rather than counted as temporary debris. Everything this assertion
        originally caught — `.XXXXXX` staging copies and unrenamed temporaries
        — still fails it.
        """
        return sorted(
            p.name
            for p in self.home.glob("result.png.*")
            if not MANIFEST_NAME.fullmatch(p.name)
        )

    def manifests(self, stem="result.png"):
        return sorted(
            p for p in self.home.glob(stem + ".*") if MANIFEST_NAME.fullmatch(p.name)
        )

    def record(self, stem="result.png"):
        """The single manifest for one destination, parsed independently."""
        found = self.manifests(stem)
        self.assertEqual(len(found), 1, found)
        return json.loads(found[0].read_text())

    def cli(self, *args, prompt=b"", cwd=None, stdin=None):
        return subprocess.run(
            [TNY, "image", *args],
            input=None if stdin is not None else prompt,
            stdin=stdin,
            cwd=str(cwd or self.home),
            env=self.env,
            capture_output=True,
            timeout=60,
        )

    def uploaded(self, index=0):
        """The exact reference bytes the provider received."""
        sent = self.image_requests()[-1][1]
        url = sent["images"][index]["image_url"]
        return base64.b64decode(url.split(",", 1)[1])

    def canonical(self, path):
        """Records store canonical paths; the fixture's own may not be."""
        return str(Path(os.path.realpath(path)))

    # --- other result callers --------------------------------------------

    def agent(
        self,
        profile,
        arguments,
        *,
        tool="typed",
        json_flag=True,
        mode="yolo",
        tool_name="image_generate",
        command=None,
    ):
        self.state["chat"].clear()
        self.state["requests"].clear()
        self.state["tool_args"] = arguments
        self.state["tool"] = tool
        self.state["json"] = json_flag
        self.state["tool_name"] = tool_name
        self.state["command"] = command
        env = dict(self.env)
        env.update(
            {
                "OPENAI_API_KEY": "fixture-chat-key",
                "OPENAI_BASE_URL": self.url + "/v1",
                "OPENAI_WIRE_API": "chat",
                "TNY_TOOLS": profile,
            }
        )
        return subprocess.run(
            [
                TNY,
                "--provider",
                "openai",
                "--cwd",
                str(self.home),
                "--permission-mode",
                mode,
                "ask",
                "--ephemeral",
                "fixture image",
            ],
            env=env,
            capture_output=True,
            timeout=60,
        )

    def tool_result(self):
        return next(
            m["content"]
            for m in self.state["chat"][1]["messages"]
            if m.get("role") == "tool"
        )

    def toolkit(self):
        library = (
            ROOT
            / "build/lib"
            / ("libtny.1.dylib" if sys.platform == "darwin" else "libtny.so.1")
        )
        if WASM or WINDOWS or not library.exists():
            self.skipTest("libtny shared build not present (make lib-shared)")
        sys.path.insert(0, str(ROOT / "sdk/python/src"))
        try:
            import tny  # noqa: PLC0415
        except ImportError as error:  # cffi is an SDK-only dependency
            self.skipTest(f"python SDK unavailable: {error}")
        config = tny.ToolkitConfig(
            workspace=self.home,
            chatgpt_token=TOKEN,
            chatgpt_account_id=ACCOUNT,
            codex_base_url=self.url + "/backend-api/codex",
        )
        return tny, tny.Toolkit(config, library=str(library))


class Dimensions(ImageFixture):
    # --- reported dimensions come from the bytes -------------------------

    def test_reported_dimensions_ignore_the_requested_size(self):
        self.state["image"] = png(1935, 811)
        run = self.generate("--size", "3440x1440", "--json")
        result = self.result(run)
        self.assertEqual((result["width"], result["height"]), (1935, 811))
        self.assertEqual(result["requested_size"], "3440x1440")
        self.assertEqual(result["effective_size"], "3440x1440")
        self.assertEqual(result["size_status"], "mismatch")
        self.assertEqual(result["mime_type"], "image/png")
        self.assertTrue(result["native"])
        self.assertIsNone(result["transform"])
        self.assertEqual(result["bytes"], len(self.state["image"]))
        self.assertEqual(self.out.read_bytes(), self.state["image"])
        # The literal sent on the wire is reported, not a guessed provider size.
        self.assertEqual(self.image_requests()[-1][1]["size"], "3440x1440")
        self.assertIn(b"3440x1440", run.stderr)
        self.assertIn(b"1935x811", run.stderr)
        self.assertIn(b"warning", run.stderr)
        # Ordinary output stays the path; the warning stays on stderr.
        plain = self.generate("--size", "3440x1440")
        self.assertEqual(plain.returncode, 0, plain.stderr)
        self.assertEqual(plain.stdout.decode().strip(), str(self.out))
        self.assertIn(b"warning", plain.stderr)

    def test_exact_match_and_one_pixel_mismatch(self):
        self.state["image"] = png(64, 64)
        result = self.result(self.generate("--size", "64x64", "--json"))
        self.assertEqual(result["size_status"], "match")
        self.assertEqual((result["width"], result["height"]), (64, 64))
        self.state["image"] = png(64, 63)
        run = self.generate("--size", "64x64", "--json")
        result = self.result(run)
        self.assertEqual(result["size_status"], "mismatch")
        self.assertEqual((result["width"], result["height"]), (64, 63))
        self.assertIn(b"warning", run.stderr)
        self.state["image"] = png(63, 64)
        self.assertEqual(
            self.result(self.generate("--size", "64x64", "--json"))["size_status"],
            "mismatch",
        )
        # Same aspect ratio, different pixels, is still a mismatch.
        self.state["image"] = png(32, 32)
        self.assertEqual(
            self.result(self.generate("--size", "64x64", "--json"))["size_status"],
            "mismatch",
        )

    def test_auto_and_opaque_requests_never_claim_a_match(self):
        self.state["image"] = png(48, 24)
        for args, expected, requested in (
            ((), "auto", "auto"),
            (("--size", "auto"), "auto", "auto"),
            (("--size", "portrait"), "unverifiable", "portrait"),
            (("--size", "1024"), "unverifiable", "1024"),
        ):
            with self.subTest(args=args):
                run = self.generate(*args, "--json")
                result = self.result(run)
                self.assertEqual(result["size_status"], expected)
                self.assertEqual(result["requested_size"], requested)
                self.assertEqual(result["effective_size"], requested)
                # Dimensions are still reported truthfully.
                self.assertEqual((result["width"], result["height"]), (48, 24))
                self.assertNotIn(b"warning", run.stderr)

    def test_every_supported_format_reports_its_own_header(self):
        cases = (
            ("png", png(300, 200), 300, 200, "image/png", "match"),
            ("jpeg-baseline", jpeg(300, 200), 300, 200, "image/jpeg", "match"),
            (
                "jpeg-progressive",
                jpeg(300, 200, frame=0xC2),
                300,
                200,
                "image/jpeg",
                "match",
            ),
            ("webp-lossy", webp("lossy", 300, 200), 300, 200, "image/webp", "match"),
            (
                "webp-lossless",
                webp("lossless", 300, 200),
                300,
                200,
                "image/webp",
                "match",
            ),
            (
                "webp-extended",
                webp("extended", 300, 200),
                300,
                200,
                "image/webp",
                "match",
            ),
        )
        for name, data, width, height, mime, status in cases:
            with self.subTest(format=name):
                self.state["image"] = data
                result = self.result(self.generate("--size", "300x200", "--json"))
                self.assertEqual((result["width"], result["height"]), (width, height))
                self.assertEqual(result["mime_type"], mime)
                self.assertEqual(result["size_status"], status)
                self.assertEqual(self.out.read_bytes(), data)

    def test_unsupported_encoding_is_not_called_unverifiable(self):
        # A lossless JPEG frame is well formed but not a raster this build reads.
        self.state["image"] = jpeg(300, 200, frame=0xC3)
        result = self.result(self.generate("--size", "300x200", "--json"))
        self.assertEqual(result["size_status"], "unsupported")
        self.assertIsNone(result["width"])
        self.assertIsNone(result["height"])
        self.assertEqual(result["mime_type"], "image/jpeg")
        self.assertEqual(self.out.read_bytes(), self.state["image"])

    def test_malformed_truncated_and_impossible_headers(self):
        cases = (
            ("truncated", broken_png(64, 64, truncate=6)),
            ("zero-width", broken_png(0, 64)),
            ("overflowing-edge", broken_png(0xFFFFFFFF, 64)),
            ("wrong-header-length", broken_png(64, 64, ihdr_length=12)),
        )
        for name, data in cases:
            with self.subTest(case=name):
                self.state["image"] = data
                run = self.generate("--size", "64x64", "--json")
                # Magic-byte acceptance is unchanged: the file is still written.
                result = self.result(run)
                self.assertEqual(result["size_status"], "unverifiable")
                self.assertIsNone(result["width"])
                self.assertIsNone(result["height"])
                self.assertEqual(self.out.read_bytes(), data)
                self.assertIn(b"warning", run.stderr)

    # --- strict size ------------------------------------------------------

    def test_strict_size_rejects_before_replacing_the_destination(self):
        self.out.write_bytes(b"previous image bytes")
        self.state["image"] = png(1935, 811)
        run = self.generate("--size", "3440x1440", "--strict-size", "--json")
        failure = self.failure(run)
        self.assertEqual(failure["code"], "IMAGE_SIZE_MISMATCH")
        self.assertEqual(failure["requested_size"], "3440x1440")
        self.assertEqual(failure["effective_size"], "3440x1440")
        self.assertEqual((failure["width"], failure["height"]), (1935, 811))
        self.assertEqual(failure["size_status"], "mismatch")
        self.assertEqual(failure["mime_type"], "image/png")
        self.assertIsNone(failure["path"])
        self.assertFalse(failure["committed"])
        self.assertFalse(failure["ok"])
        # The paid bytes are discarded, the old file stands, nothing is retried.
        self.assertEqual(self.out.read_bytes(), b"previous image bytes")
        self.assertEqual(self.leftovers(), [])
        # The paid attempt is still recorded — as a failure that claims no
        # artifact, never as a success or a committed path.
        records = self.manifests()
        self.assertEqual(len(records), 1)
        record = json.loads(records[0].read_text())
        self.assertEqual(record["status"], "failed")
        self.assertFalse(record["committed"])
        self.assertEqual(record["artifacts"], [])
        self.assertEqual(record["error"]["code"], "IMAGE_SIZE_MISMATCH")
        self.assertEqual(len(self.image_requests()), 1)
        self.assertIn(b"IMAGE_SIZE_MISMATCH", run.stderr)
        self.assertNotIn(str(self.out).encode(), run.stdout.split(b'"path"')[0])

    def test_strict_size_requires_a_concrete_request_before_paying(self):
        self.out.write_bytes(b"previous image bytes")
        for args in ((), ("--size", "auto"), ("--size", "portrait"), ("--size", "0x0")):
            with self.subTest(args=args):
                run = self.generate(*args, "--strict-size", "--json")
                failure = self.failure(run)
                self.assertEqual(failure["code"], "IMAGE_STRICT_SIZE_INVALID")
                self.assertIsNone(failure["width"])
                self.assertIsNone(failure["path"])
                self.assertFalse(failure["committed"])
                self.assertIsNone(failure["effective_size"])
        self.assertEqual(self.image_requests(), [])
        self.assertEqual(self.out.read_bytes(), b"previous image bytes")
        self.assertEqual(self.leftovers(), [])

    def test_strict_size_rejects_unreadable_dimensions(self):
        self.out.write_bytes(b"previous image bytes")
        for name, data, code in (
            ("truncated", broken_png(64, 64, truncate=6), "IMAGE_SIZE_UNVERIFIABLE"),
            ("unsupported", jpeg(64, 64, frame=0xC3), "IMAGE_SIZE_UNSUPPORTED"),
        ):
            with self.subTest(case=name):
                self.state["image"] = data
                failure = self.failure(
                    self.generate("--size", "64x64", "--strict-size", "--json")
                )
                self.assertEqual(failure["code"], code)
                self.assertIsNone(failure["width"])
                self.assertFalse(failure["committed"])
                self.assertEqual(self.out.read_bytes(), b"previous image bytes")
        self.assertEqual(len(self.image_requests()), 2)  # one request per attempt
        self.assertEqual(self.leftovers(), [])

    def test_strict_size_accepts_an_exact_image(self):
        self.out.write_bytes(b"previous image bytes")
        self.state["image"] = png(256, 128)
        run = self.generate("--size", "256x128", "--strict-size", "--json")
        result = self.result(run)
        self.assertEqual(result["size_status"], "match")
        self.assertEqual(result["path"], str(self.out))
        self.assertEqual(self.out.read_bytes(), self.state["image"])
        self.assertNotIn(b"warning", run.stderr)
        self.assertEqual(self.leftovers(), [])

    def test_strict_size_is_documented_in_help(self):
        run = subprocess.run(
            [TNY, "image", "generate", "--help"],
            env=self.env,
            capture_output=True,
            timeout=30,
        )
        self.assertEqual(run.returncode, 0)
        self.assertIn(b"--strict-size", run.stdout)
        self.assertIn(b"size_status", run.stdout)

    def test_typed_tool_exposes_strict_size_and_dimensions(self):
        self.state["image"] = png(1935, 811)
        run = self.agent(
            "all",
            {
                "prompt": "A blue robot",
                "output_file": "agent.png",
                "size": "3440x1440",
            },
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        schema = {
            tool["function"]["name"]: tool["function"]
            for tool in self.state["chat"][0]["tools"]
        }
        for name in ("image_generate", "image_edit"):
            properties = schema[name]["parameters"]["properties"]
            self.assertEqual(properties["strict_size"]["type"], "boolean")
            self.assertNotIn("strict_size", schema[name]["parameters"]["required"])
        result = json.loads(self.tool_result())
        self.assertEqual((result["width"], result["height"]), (1935, 811))
        self.assertEqual(result["size_status"], "mismatch")
        self.assertEqual(result["requested_size"], "3440x1440")
        self.assertEqual((self.home / "agent.png").read_bytes(), self.state["image"])

    def test_typed_tool_strict_size_fails_without_writing(self):
        target = self.home / "agent.png"
        target.write_bytes(b"previous image bytes")
        self.state["image"] = png(1935, 811)
        run = self.agent(
            "all",
            {
                "prompt": "A blue robot",
                "output_file": "agent.png",
                "size": "3440x1440",
                "strict_size": True,
            },
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        # The established failure marker is unchanged; the bytes after it are
        # the same safe object the CLI prints with --json.
        result = self.tool_result()
        self.assertTrue(result.startswith("error: "), result)
        failure = json.loads(result[len("error: ") :])
        self.assertEqual(failure["code"], "IMAGE_SIZE_MISMATCH")
        self.assertEqual(failure["requested_size"], "3440x1440")
        self.assertEqual(failure["effective_size"], "3440x1440")
        self.assertEqual((failure["width"], failure["height"]), (1935, 811))
        self.assertEqual(failure["size_status"], "mismatch")
        self.assertEqual(failure["mime_type"], "image/png")
        self.assertIsNone(failure["path"])
        self.assertFalse(failure["committed"])
        self.assertFalse(failure["ok"])
        for secret in (TOKEN, ACCOUNT, self.url, str(target), "A blue robot"):
            self.assertNotIn(secret, result)
        self.assertEqual(target.read_bytes(), b"previous image bytes")
        self.assertEqual(len(self.image_requests()), 1)
        self.assertEqual(self.state["requests"][-2][1]["size"], "3440x1440")

    def test_typed_tool_generic_failure_has_no_structured_detail(self):
        # An unusable prompt is an ordinary failure, not a strict-size
        # decision, so it carries no JSON object at all.
        run = self.agent("all", {"prompt": "   ", "output_file": "agent.png"})
        self.assertEqual(run.returncode, 0, run.stderr)
        result = self.tool_result()
        self.assertTrue(result.startswith("error: "), result)
        self.assertNotIn("{", result)
        self.assertNotIn("committed", result)
        self.assertEqual(self.image_requests(), [])

    @unittest.skipIf(WASM, "shell profiles have no terminal tool in wasm")
    def test_shell_interception_carries_the_same_metadata(self):
        self.state["image"] = png(1935, 811)
        run = self.agent(
            "terminal",
            {"prompt": "A blue robot", "output_file": "agent.png", "size": "3440x1440"},
            tool="terminal",
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        result = self.tool_result()
        self.assertIn('"size_status":"mismatch"', result)
        self.assertIn('"width":1935,"height":811', result)
        self.assertIn("warning", result)
        self.assertIn("3440x1440", result)
        self.assertEqual(len(self.image_requests()), 1)

    @unittest.skipIf(WASM, "shell profiles have no terminal tool in wasm")
    def test_shell_interception_strict_size_preserves_output(self):
        target = self.home / "agent.png"
        target.write_bytes(b"previous image bytes")
        self.state["image"] = png(1935, 811)
        run = self.agent(
            "terminal",
            {
                "prompt": "A blue robot",
                "output_file": "agent.png",
                "size": "3440x1440",
                "strict_size": True,
            },
            tool="terminal",
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        result = self.tool_result()
        self.assertIn("IMAGE_SIZE_MISMATCH", result)
        self.assertIn("exit: 1", result)
        # --json was explicitly requested, so the object is on stdout, between
        # the exit line and the stderr diagnostic.
        body = result.split("\n")[1]
        failure = json.loads(body)
        self.assertEqual(failure["code"], "IMAGE_SIZE_MISMATCH")
        self.assertEqual((failure["width"], failure["height"]), (1935, 811))
        self.assertEqual(failure["requested_size"], "3440x1440")
        self.assertEqual(failure["effective_size"], "3440x1440")
        self.assertEqual(failure["size_status"], "mismatch")
        self.assertIsNone(failure["path"])
        self.assertFalse(failure["committed"])
        for secret in (TOKEN, ACCOUNT, self.url):
            self.assertNotIn(secret, result)
        self.assertEqual(target.read_bytes(), b"previous image bytes")
        self.assertEqual(len(self.image_requests()), 1)

    @unittest.skipIf(WASM, "shell profiles have no terminal tool in wasm")
    def test_shell_interception_without_json_keeps_stdout_empty(self):
        target = self.home / "agent.png"
        target.write_bytes(b"previous image bytes")
        self.state["image"] = png(1935, 811)
        run = self.agent(
            "terminal",
            {
                "prompt": "A blue robot",
                "output_file": "agent.png",
                "size": "3440x1440",
                "strict_size": True,
            },
            tool="terminal",
            json_flag=False,
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        result = self.tool_result()
        # Exit status and the coded diagnostic are unchanged; nothing extra is
        # printed on stdout for a plain-text command.
        self.assertTrue(result.startswith("exit: 1\n"), result)
        self.assertIn("tny: image: IMAGE_SIZE_MISMATCH", result)
        self.assertNotIn("committed", result)
        self.assertNotIn('"kind":"image"', result)
        self.assertEqual(target.read_bytes(), b"previous image bytes")
        self.assertEqual(len(self.image_requests()), 1)

    def test_python_sdk_shares_the_options_and_results(self):
        tny, kit = self.toolkit()
        self.state["image"] = png(1935, 811)
        result = kit.generate_image("A robot", output_file="sdk.png", size="3440x1440")
        self.assertEqual((result.width, result.height), (1935, 811))
        self.assertEqual(result.size_status, "mismatch")
        self.assertEqual(result.requested_size, "3440x1440")
        self.assertEqual(result.effective_size, "3440x1440")
        target = self.home / "sdk.png"
        self.assertEqual(target.read_bytes(), self.state["image"])
        target.write_bytes(b"previous image bytes")
        # libtny deliberately exposes stable categories, not message text, so a
        # strict rejection is a protocol error and the old file is untouched.
        with self.assertRaises(tny.ProtocolError) as raised:
            kit.generate_image(
                "A robot", output_file="sdk.png", size="3440x1440", strict_size=True
            )
        # The exact code is still readable, as a distinct failure object.
        detail = raised.exception.image_detail
        self.assertIsInstance(detail, tny.ImageFailureDetail)
        self.assertNotIsInstance(detail, tny.ImageResult)
        self.assertEqual(detail.code, "IMAGE_SIZE_MISMATCH")
        self.assertEqual((detail.width, detail.height), (1935, 811))
        self.assertEqual(detail.requested_size, "3440x1440")
        self.assertEqual(detail.effective_size, "3440x1440")
        self.assertIsNone(detail.path)
        self.assertFalse(detail.committed)
        printed = str(raised.exception) + repr(raised.exception)
        for secret in (TOKEN, ACCOUNT, self.url, "3440x1440", "A robot"):
            self.assertNotIn(secret, printed)
        self.assertEqual(target.read_bytes(), b"previous image bytes")
        self.assertEqual(len(self.image_requests()), 2)
        # An unusable strict request never reaches the provider at all.
        with self.assertRaises(tny.InvalidArgumentError) as raised:
            kit.generate_image("A robot", output_file="sdk.png", strict_size=True)
        self.assertEqual(
            raised.exception.image_detail.code, "IMAGE_STRICT_SIZE_INVALID"
        )
        self.assertIsNone(raised.exception.image_detail.effective_size)
        self.assertEqual(len(self.image_requests()), 2)
        self.assertEqual(target.read_bytes(), b"previous image bytes")


class Manifest(ImageFixture):
    """Private per-operation records, replay and reference lineage (#127).

    The exact C127 command is

        env -u TNY_TOOLS python3 tests/integration/test_image_workflow.py -k Manifest

    Hashes, schema shape, file modes and concurrency are all observed from
    Python and the filesystem. Export (#125) and job (#124) lineage are
    deliberately absent: those slices do not exist yet.
    """

    def written(self, name, data=b"seed bytes"):
        path = self.home / name
        path.write_bytes(data)
        return path

    def handwritten(self, name, **overrides):
        """A record tny never wrote, for cases only a crafted file reaches."""
        body = {
            "version": 1,
            "kind": "image_manifest",
            "operation_id": "00112233445566aa",
            "operation": "generate",
            "status": "succeeded",
            "workspace": str(self.root),
            "started": "2026-09-12T00:00:00Z",
            "finished": "2026-09-12T00:00:01Z",
            "prompt": "a crafted prompt",
            "output": str(self.root / "crafted.png"),
            "committed": False,
            "references": [],
            "requested": {
                "provider": "codex",
                "model": None,
                "quality": None,
                "size": "auto",
            },
            "effective": {"provider": "codex", "model": None, "size": "auto"},
            "result": {
                "width": None,
                "height": None,
                "mime_type": None,
                "bytes": None,
                "size_status": "auto",
            },
            "actual": {"seed": None, "request_id": None},
            "error": None,
            "source": None,
            "artifacts": [],
        }
        body.update(overrides)
        path = self.home / name
        path.write_text(json.dumps(body))
        return path

    def replay(self, record, output, *args, **kwargs):
        return self.cli(
            "replay",
            "--manifest",
            str(record),
            "--output-file",
            str(output),
            "--json",
            *args,
            **kwargs,
        )

    def test_edit_replay_serializes_resolved_operation_in_all_outcomes(self):
        reference = self.written("reference.png", png(3, 3))
        self.result(
            self.cli(
                "edit",
                "--image",
                str(reference),
                "--output-file",
                str(self.out),
                "--json",
                prompt=b"recorded edit",
            )
        )
        source = self.manifests()[0]
        for outcome in ("success", "strict", "strict-invalid", "retained"):
            with self.subTest(outcome=outcome):
                output = self.home / (outcome + ".png")
                before = len(self.image_requests())
                self.state["on_image"] = (
                    self.obstruct(output.name) if outcome == "retained" else None
                )
                flags = ()
                if outcome.startswith("strict"):
                    flags = (
                        "--strict-size",
                        "--size",
                        "auto" if outcome == "strict-invalid" else "2x2",
                    )
                run = self.replay(source, output, *flags)
                detail = self.result(run) if outcome == "success" else self.failure(run)
                self.assertEqual(detail["operation"], "edit")
                self.assertEqual(
                    len(self.image_requests()) - before,
                    0 if outcome == "strict-invalid" else 1,
                )
                if outcome.startswith("strict"):
                    self.assertFalse(detail["committed"])
                    self.assertIsNone(detail["path"])
                    self.assertFalse(output.exists())
                else:
                    self.assertEqual(output.read_bytes(), self.state["image"])
                    self.assertEqual(self.uploaded(), reference.read_bytes())
                    self.assertEqual(
                        self.image_requests()[-1][0], "/backend-api/codex/images/edits"
                    )
                    if outcome == "retained":
                        self.assertTrue(detail["committed"])
                        self.assertEqual(
                            detail["code"], "IMAGE_MANIFEST_FINALIZE_FAILED"
                        )

    def test_replay_rejects_inherited_invalid_quality_before_http(self):
        record = self.handwritten(
            "invalid-quality.json",
            requested={"provider": "codex", "quality": "not-a-quality", "size": "auto"},
        )
        before = record.read_bytes()
        run = self.replay(record, self.out)
        self.assertEqual(run.returncode, 1, run.stderr)
        self.assertIn(b"valid options", run.stderr)
        self.assertEqual(self.image_requests(), [])
        self.assertEqual(list(self.home.glob("result.png*")), [])
        self.assertEqual(record.read_bytes(), before)
        # A valid explicit override still wins over the recorded option.
        self.result(self.replay(record, self.out, "--quality", "low"))
        self.assertEqual(self.image_requests()[0][1]["quality"], "low")

    # --- the default record ----------------------------------------------

    def test_default_generation_records_a_complete_operation(self):
        self.state["image"] = png(64, 48)
        run = self.generate("--size", "64x48", "--json")
        result = self.result(run)
        records = self.manifests()
        self.assertEqual(len(records), 1)
        self.assertEqual(result["manifest_path"], self.canonical(records[0]))
        # Provenance is guidance on stderr; stdout keeps its existing content.
        self.assertIn(self.canonical(records[0]).encode(), run.stderr)
        record = json.loads(records[0].read_text())
        self.assertEqual(record["version"], 1)
        self.assertEqual(record["kind"], "image_manifest")
        self.assertEqual(record["operation"], "generate")
        self.assertEqual(record["status"], "succeeded")
        self.assertTrue(record["committed"])
        self.assertEqual(record["operation_id"], result["operation_id"])
        self.assertRegex(record["operation_id"], r"^[0-9a-f]{16}$")
        self.assertEqual(record["prompt"], "An orange robot")
        self.assertEqual(record["references"], [])
        self.assertEqual(record["workspace"], str(self.root))
        self.assertEqual(record["output"], str(self.root / "result.png"))
        self.assertRegex(record["started"], UTC_STAMP)
        self.assertRegex(record["finished"], UTC_STAMP)
        self.assertEqual(record["requested"]["provider"], "codex")
        self.assertEqual(record["requested"]["size"], "64x48")
        self.assertIsNone(record["requested"]["model"])
        self.assertEqual(record["effective"]["size"], "64x48")
        self.assertEqual(record["effective"]["model"], "gpt-image-2.5-sunburst")
        self.assertEqual(record["result"]["width"], 64)
        self.assertEqual(record["result"]["height"], 48)
        self.assertEqual(record["result"]["mime_type"], "image/png")
        self.assertEqual(record["result"]["bytes"], len(self.state["image"]))
        self.assertEqual(record["result"]["size_status"], "match")
        self.assertIsNone(record["error"])
        self.assertIsNone(record["source"])
        artifact = record["artifacts"][0]
        self.assertEqual(len(record["artifacts"]), 1)
        self.assertEqual(artifact["role"], "native")
        self.assertEqual(artifact["path"], str(self.root / "result.png"))
        self.assertEqual(artifact["sha256"], sha(self.out.read_bytes()))
        self.assertIsNone(artifact["transform"])
        # Credentials and endpoints are never persisted.
        raw = records[0].read_text()
        for secret in (TOKEN, ACCOUNT, self.url):
            self.assertNotIn(secret, raw)
        self.assertEqual(self.leftovers(), [])

    def test_records_and_artifacts_are_private_to_their_owner(self):
        self.generate("--json")
        record = self.manifests()[0]
        self.assertEqual(stat.S_IMODE(record.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(self.out.stat().st_mode), 0o600)

    def test_repeated_output_creates_new_records_and_stales_the_old_hash(self):
        self.state["image"] = png(8, 8)
        first = self.result(self.generate("--json"))
        original = self.out.read_bytes()
        self.state["image"] = png(9, 9)
        second = self.result(self.generate("--json"))
        self.assertNotEqual(first["operation_id"], second["operation_id"])
        records = {
            json.loads(p.read_text())["operation_id"]: json.loads(p.read_text())
            for p in self.manifests()
        }
        self.assertEqual(len(records), 2)
        old = records[first["operation_id"]]
        # The earlier lineage survives byte for byte, and its recorded hash no
        # longer matches the file at that path.
        self.assertEqual(old["artifacts"][0]["sha256"], sha(original))
        self.assertNotEqual(old["artifacts"][0]["sha256"], sha(self.out.read_bytes()))
        self.assertEqual(
            records[second["operation_id"]]["artifacts"][0]["sha256"],
            sha(self.out.read_bytes()),
        )
        # Reusing the stale record refuses to pass the new file off as its
        # output rather than uploading whatever now sits there.
        stale = next(
            p
            for p in self.manifests()
            if json.loads(p.read_text())["operation_id"] == first["operation_id"]
        )
        self.state["requests"].clear()
        run = self.cli(
            "edit",
            "--artifact",
            str(stale),
            "--output-file",
            str(self.home / "derived.png"),
            "--json",
            prompt=b"make it blue",
        )
        self.assertEqual(run.returncode, 1, run.stderr)
        self.assertIn(b"hash", run.stderr)
        self.assertEqual(self.image_requests(), [])
        self.assertFalse((self.home / "derived.png").exists())

    def test_opt_out_writes_no_record_prompt_or_reference(self):
        canary = b"sentinel-prompt-canary-127"
        run = self.generate("--no-manifest", "--json", prompt=canary)
        result = self.result(run)
        # The operation still has an identity; it simply leaves no record.
        self.assertIsNone(result["manifest_path"])
        self.assertRegex(result["operation_id"], r"^[0-9a-f]{16}$")
        self.assertEqual(self.manifests(), [])
        self.assertNotIn(b"manifest:", run.stderr)
        self.assertEqual(self.leftovers(), [])
        # Nothing under the throwaway HOME — including the writer guard, which
        # may legitimately remain — holds the prompt.
        for path in self.home.rglob("*"):
            if path.is_file():
                self.assertNotIn(canary, path.read_bytes(), path)

    # --- provider supplied metadata ---------------------------------------

    def test_provider_identifiers_are_recorded_only_when_returned(self):
        self.state["envelope"] = {"request_id": "req_fixture_123"}
        self.state["item"] = {"seed": 42}
        present = self.result(self.generate("--json"))
        self.assertEqual(present["seed"], 42)
        self.assertEqual(present["request_id"], "req_fixture_123")
        self.assertEqual(
            self.record()["actual"], {"seed": 42, "request_id": "req_fixture_123"}
        )
        # An unrecognized provider field is never copied into the record.
        self.state["envelope"] = {"internal_trace": "must-not-be-copied"}
        self.state["item"] = {}
        absent = self.result(self.generate("--json", output=self.home / "second.png"))
        self.assertIsNone(absent["seed"])
        self.assertIsNone(absent["request_id"])
        second = self.record("second.png")
        self.assertIsNone(second["actual"]["seed"])
        self.assertIsNone(second["actual"]["request_id"])
        raw = self.manifests("second.png")[0].read_text()
        self.assertNotIn("internal_trace", raw)
        self.assertNotIn("must-not-be-copied", raw)
        # Absence is absence: the local operation id is not promoted into one.
        self.assertNotIn(second["operation_id"], json.dumps(second["actual"]))

    # --- replay -------------------------------------------------------------

    def test_replay_reuses_the_record_and_applies_overrides(self):
        self.state["image"] = png(32, 16)
        self.result(
            self.generate(
                "--size",
                "32x16",
                "--quality",
                "low",
                "--json",
                prompt=b"a recorded prompt",
            )
        )
        source = self.manifests()[0]
        recorded = json.loads(source.read_text())
        elsewhere = tempfile.TemporaryDirectory(prefix="tny-unrelated-")
        self.addCleanup(elsewhere.cleanup)
        again = self.home / "again.png"
        self.state["requests"].clear()
        run = self.replay(source, again, cwd=elsewhere.name)
        result = json.loads(run.stdout)
        self.assertEqual(run.returncode, 0, run.stderr)
        sent = self.image_requests()[-1][1]
        self.assertEqual(sent["prompt"], "a recorded prompt")
        self.assertEqual(sent["size"], "32x16")
        self.assertEqual(sent["quality"], "low")
        self.assertEqual(again.read_bytes(), self.state["image"])
        rerun = self.record("again.png")
        self.assertEqual(rerun["source"]["manifest"], self.canonical(source))
        self.assertEqual(rerun["source"]["operation_id"], recorded["operation_id"])
        self.assertEqual(rerun["prompt"], "a recorded prompt")
        self.assertNotEqual(rerun["operation_id"], recorded["operation_id"])
        self.assertEqual(result["operation_id"], rerun["operation_id"])
        # Explicit options and piped text replace their recorded counterparts.
        third = self.home / "third.png"
        self.state["requests"].clear()
        run = self.replay(
            source,
            third,
            "--quality",
            "high",
            "--size",
            "32x16",
            prompt=b"an explicit prompt",
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        sent = self.image_requests()[-1][1]
        self.assertEqual(sent["prompt"], "an explicit prompt")
        self.assertEqual(sent["quality"], "high")
        self.assertEqual(self.record("third.png")["prompt"], "an explicit prompt")

    def test_pure_replay_needs_no_stdin_on_a_terminal(self):
        self.state["image"] = png(6, 6)
        self.result(self.generate("--json", prompt=b"terminal replay prompt"))
        source = self.manifests()[0]
        self.state["requests"].clear()
        primary, secondary = os.openpty()
        try:
            run = self.replay(source, self.home / "tty.png", stdin=secondary)
        finally:
            os.close(secondary)
            os.close(primary)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(
            self.image_requests()[-1][1]["prompt"], "terminal replay prompt"
        )

    def test_replay_refuses_unusable_records_before_paying(self):
        self.state["image"] = png(4, 4)
        self.result(self.generate("--json"))
        good = json.loads(self.manifests()[0].read_text())
        cases = {
            "missing": self.home / "absent.json",
            "blank": self.written("blank.json", b""),
            "malformed": self.written("bad.json", b"{ not json"),
            "future": self.handwritten("future.json", version=2),
            "failed": self.handwritten("failed.json", status="failed"),
            "wrong-kind": self.handwritten("kind.json", kind="something_else"),
            "oversized": self.written("huge.json", b"{" + b"x" * (256 * 1024)),
            "bad-type": self.handwritten("type.json", prompt=17),
            "bad-workspace": self.handwritten("rel.json", workspace="relative"),
        }
        self.state["requests"].clear()
        for name, record in cases.items():
            with self.subTest(case=name):
                target = self.home / f"{name}.png"
                run = self.replay(record, target)
                self.assertEqual(run.returncode, 1, run.stderr)
                self.assertFalse(target.exists())
        self.assertEqual(self.image_requests(), [])
        self.assertIn(
            "version", self.replay(cases["future"], self.home / "v.png").stderr.decode()
        )
        # The good record still works, so the refusals are about the records.
        self.assertEqual(good["status"], "succeeded")
        self.assertEqual(
            self.replay(self.manifests()[0], self.home / "ok.png").returncode, 0
        )

    def test_replay_output_may_not_alias_the_recorded_artifact(self):
        self.state["image"] = png(5, 5)
        self.result(self.generate("--json"))
        source = self.manifests()[0]
        original = self.out.read_bytes()
        self.state["requests"].clear()
        run = self.replay(source, self.out)
        self.assertEqual(run.returncode, 1, run.stderr)
        self.assertIn(b"new path", run.stderr)
        self.assertEqual(self.out.read_bytes(), original)
        # Nor may it replace the record being rerun.
        run = self.replay(source, source)
        self.assertEqual(run.returncode, 1, run.stderr)
        self.assertEqual(self.image_requests(), [])

    def test_replay_detects_changed_and_missing_references(self):
        base = self.written("base.png", png(10, 10))
        original = base.read_bytes()
        self.state["image"] = png(11, 11)
        edited = self.home / "edited.png"
        run = self.cli(
            "edit",
            "--image",
            str(base),
            "--output-file",
            str(edited),
            "--json",
            prompt=b"make it blue",
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        record = self.manifests("edited.png")[0]
        reference = json.loads(record.read_text())["references"][0]
        self.assertEqual(reference["sha256"], sha(original))
        self.assertEqual(reference["sha256"], sha(self.uploaded()))
        self.assertIsNone(reference["source_manifest"])
        # A replaced reference is refused rather than uploaded silently.
        base.write_bytes(png(12, 12))
        self.state["requests"].clear()
        run = self.replay(record, self.home / "changed.png")
        self.assertEqual(run.returncode, 1, run.stderr)
        self.assertIn(b"hash", run.stderr)
        self.assertEqual(self.image_requests(), [])
        # So is a missing one.
        base.unlink()
        run = self.replay(record, self.home / "gone.png")
        self.assertEqual(run.returncode, 1, run.stderr)
        self.assertIn(b"reference", run.stderr)
        self.assertEqual(self.image_requests(), [])
        # Restoring the exact bytes makes it usable again.
        base.write_bytes(original)
        run = self.replay(record, self.home / "restored.png")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(self.uploaded(), original)

    def test_relative_recorded_paths_resolve_against_the_workspace(self):
        assets = self.home / "assets"
        assets.mkdir()
        reference = assets / "ref.png"
        reference.write_bytes(png(7, 7))
        record = self.handwritten(
            "relative.json",
            operation="edit",
            references=[
                {
                    "path": "assets/ref.png",
                    "sha256": sha(reference.read_bytes()),
                    "source_manifest": None,
                    "source_operation": None,
                }
            ],
        )
        # A decoy at the same relative path under the caller's own directory
        # must never be the one that is read.
        elsewhere = tempfile.TemporaryDirectory(prefix="tny-unrelated-")
        self.addCleanup(elsewhere.cleanup)
        decoy = Path(elsewhere.name) / "assets"
        decoy.mkdir()
        (decoy / "ref.png").write_bytes(png(99, 99))
        self.state["requests"].clear()
        run = self.replay(record, self.home / "relative.png", cwd=elsewhere.name)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(self.uploaded(), reference.read_bytes())

    def test_reading_a_record_never_opens_its_references(self):
        (self.home / "refdir").mkdir()
        record = self.handwritten(
            "unread.json",
            version=2,
            operation="edit",
            references=[
                {
                    "path": str(self.root / "refdir"),
                    "sha256": "0" * 64,
                    "source_manifest": None,
                    "source_operation": None,
                }
            ],
        )
        run = self.replay(record, self.home / "unread.png")
        self.assertEqual(run.returncode, 1, run.stderr)
        # The version decides it; the reference is never opened or reported.
        self.assertIn(b"version", run.stderr)
        self.assertNotIn(b"reference image", run.stderr)
        self.assertEqual(self.image_requests(), [])

    # --- artifact lineage ---------------------------------------------------

    def test_artifact_edit_links_lineage_and_uploads_verified_bytes(self):
        self.state["image"] = png(20, 10)
        self.result(self.generate("--json"))
        base = self.out.read_bytes()
        source = self.manifests()[0]
        recorded = json.loads(source.read_text())
        self.state["image"] = png(21, 11)
        self.state["requests"].clear()
        derived = self.home / "derived.png"
        run = self.cli(
            "edit",
            "--artifact",
            str(source),
            "--output-file",
            str(derived),
            "--json",
            prompt=b"make it blue",
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        # The provider received exactly the recorded artifact's bytes.
        self.assertEqual(self.uploaded(), base)
        record = self.record("derived.png")
        self.assertEqual(record["operation"], "edit")
        reference = record["references"][0]
        self.assertEqual(len(record["references"]), 1)
        self.assertEqual(reference["sha256"], sha(base))
        self.assertEqual(reference["sha256"], recorded["artifacts"][0]["sha256"])
        self.assertEqual(reference["source_manifest"], self.canonical(source))
        self.assertEqual(reference["source_operation"], recorded["operation_id"])
        # Explicit file paths remain supported and record no source.
        self.state["requests"].clear()
        plain = self.home / "plain.png"
        run = self.cli(
            "edit",
            "--image",
            str(derived),
            "--output-file",
            str(plain),
            "--json",
            prompt=b"make it green",
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        reference = self.record("plain.png")["references"][0]
        self.assertIsNone(reference["source_manifest"])
        self.assertEqual(reference["sha256"], sha(derived.read_bytes()))

    def test_legacy_in_place_edit_still_replaces_its_own_reference(self):
        base = self.written("place.png", png(3, 3))
        original = base.read_bytes()
        self.state["image"] = png(4, 4)
        run = self.cli(
            "edit",
            "--image",
            str(base),
            "--output-file",
            str(base),
            "--json",
            prompt=b"in place",
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        # Every reference is loaded before anything is written, so the upload
        # is the old file and the destination now holds the new one.
        self.assertEqual(self.uploaded(), original)
        self.assertEqual(base.read_bytes(), self.state["image"])
        self.assertEqual(
            self.record("place.png")["references"][0]["sha256"], sha(original)
        )

    # --- destinations, aliases and concurrency ------------------------------

    @unittest.skipIf(WASM, "native symlink/hard-link inode semantics")
    def test_alias_and_reserved_destinations_are_refused(self):
        target = self.written("target.png", b"target bytes")
        link = self.home / "link.png"
        link.symlink_to(target)
        hardlink = self.home / "hard.png"
        os.link(target, hardlink)
        (self.home / "adir").mkdir()
        cases = {
            "symlink": link,
            "hard-link": hardlink,
            "directory": self.home / "adir",
            "reserved-name": self.home / "x.png.tny-image-abc123.json",
            "missing-parent": self.home / "nope" / "x.png",
        }
        self.state["requests"].clear()
        for name, destination in cases.items():
            with self.subTest(case=name):
                run = self.generate("--json", output=destination)
                self.assertEqual(run.returncode, 1, run.stderr)
        self.assertEqual(self.image_requests(), [])
        self.assertEqual(target.read_bytes(), b"target bytes")
        self.assertEqual(self.manifests("target.png"), [])

    @unittest.skipIf(
        WASM,
        "wasm guards one instance, not two OS processes, by design (docs/images.md)",
    )
    def test_concurrent_writers_share_one_destination(self):
        arrived, release = threading.Event(), threading.Event()
        self.state["on_image"] = lambda _body: (arrived.set(), release.wait(30))
        self.state["image"] = png(16, 16)
        first = subprocess.Popen(
            [TNY, "image", "generate", "--output-file", str(self.out), "--json"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=str(self.home),
            env=self.env,
        )
        first.stdin.write(b"first writer")
        first.stdin.close()
        self.assertTrue(arrived.wait(30))
        second = self.generate("--json", prompt=b"second writer")
        self.assertEqual(second.returncode, 1, second.stderr)
        self.assertIn(b"already writing", second.stderr)
        release.set()
        self.assertEqual(first.wait(timeout=60), 0)
        first.stdout.close()
        first.stderr.close()
        # Exactly one paid request, and only the winner left a record.
        self.assertEqual(len(self.image_requests()), 1)
        self.assertEqual(self.record()["prompt"], "first writer")
        self.assertEqual(self.out.read_bytes(), self.state["image"])

    def test_concurrent_writers_of_independent_outputs_both_succeed(self):
        started = threading.Barrier(3, timeout=60)
        self.state["on_image"] = lambda _body: started.wait()
        self.state["image"] = png(18, 18)
        runs = [
            subprocess.Popen(
                [
                    TNY,
                    "image",
                    "generate",
                    "--output-file",
                    str(self.home / name),
                    "--json",
                ],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                cwd=str(self.home),
                env=self.env,
            )
            for name in ("a.png", "b.png")
        ]
        for run in runs:
            run.stdin.write(b"independent")
            run.stdin.close()
        # Both requests are in flight at once before either is answered.
        started.wait()
        for run in runs:
            output, problem = run.communicate(timeout=60)
            self.assertEqual(run.returncode, 0, problem)
            self.assertTrue(output)
        self.assertEqual(len(self.image_requests()), 2)
        ids = {self.record(name)["operation_id"] for name in ("a.png", "b.png")}
        self.assertEqual(len(ids), 2)

    def test_killed_writer_is_observed_interrupted(self):
        arrived, release = threading.Event(), threading.Event()
        self.state["on_image"] = lambda _body: (arrived.set(), release.wait(30))
        writer = subprocess.Popen(
            [TNY, "image", "generate", "--output-file", str(self.out), "--json"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=str(self.home),
            env=self.env,
        )
        writer.stdin.write(b"doomed writer")
        writer.stdin.close()
        self.assertTrue(arrived.wait(30))
        writer.kill()
        writer.wait(timeout=30)
        writer.stdout.close()
        writer.stderr.close()
        release.set()
        record = self.record()
        # The unfinished intent is as private as a finished record.
        self.assertEqual(stat.S_IMODE(self.manifests()[0].stat().st_mode), 0o600)
        # On disk the intent is still nonterminal, and no output exists.
        self.assertEqual(record["status"], "running")
        self.assertFalse(record["committed"])
        self.assertEqual(record["artifacts"], [])
        self.assertFalse(self.out.exists())
        # A reader must classify it as interrupted, not succeeded, and must
        # not rerun it as though it had produced anything.
        self.state["on_image"] = None
        run = self.replay(self.manifests()[0], self.home / "resumed.png")
        self.assertEqual(run.returncode, 1, run.stderr)
        self.assertIn(b"interrupted", run.stderr)
        self.assertEqual(len(self.image_requests()), 1)

    # --- controlled persistence faults --------------------------------------

    @unittest.skipIf(WASM, "host directory permissions are not enforced in wasm")
    @unittest.skipIf(os.geteuid() == 0, "root ignores directory permissions")
    def test_initial_record_failure_spends_nothing(self):
        locked = self.home / "locked"
        locked.mkdir()
        destination = locked / "x.png"
        destination.write_bytes(b"previous image bytes")
        os.chmod(locked, 0o500)
        try:
            run = self.generate("--json", output=destination)
        finally:
            os.chmod(locked, 0o700)
        self.assertEqual(run.returncode, 1, run.stderr)
        self.assertIn(b"no request was made", run.stderr)
        self.assertEqual(self.image_requests(), [])
        self.assertEqual(destination.read_bytes(), b"previous image bytes")

    def provider_metadata(self):
        self.state["envelope"] = {"request_id": "provider-retained-sentinel"}
        self.state["item"] = {"seed": 4242}

    def assert_local_retained(self, failure, raw):
        self.assertNotIn("seed", failure)
        self.assertNotIn("request_id", failure)
        self.assertNotIn("provider-retained-sentinel", raw)

    def test_final_record_failure_keeps_the_paid_artifact(self):
        self.provider_metadata()

        def obstruct(_body):
            # The running record exists by now; replacing it with a directory
            # makes only the finalizing rename fail.
            record = self.manifests()[0]
            record.unlink()
            record.mkdir()

        self.state["on_image"] = obstruct
        self.state["image"] = png(12, 12)
        run = self.generate("--json")
        self.assertEqual(run.returncode, 1, run.stderr)
        failure = json.loads(run.stdout)
        self.assert_local_retained(failure, run.stdout.decode() + run.stderr.decode())
        self.assertEqual(failure["code"], "IMAGE_MANIFEST_FINALIZE_FAILED")
        self.assertFalse(failure["ok"])
        # It is not a strict-size failure and does not pretend nothing ran.
        self.assertTrue(failure["committed"])
        self.assertEqual(failure["path"], str(self.out))
        self.assertEqual(failure["bytes"], len(self.state["image"]))
        self.assertIn(b"kept", run.stderr)
        self.assertEqual(self.out.read_bytes(), self.state["image"])
        self.assertEqual(self.leftovers(), [])

    def obstruct(self, stem):
        """Make only this operation's finalizing rename fail, mid-request."""

        def hook(_body):
            record = self.manifests(stem)[0]
            record.unlink()
            record.mkdir()

        return hook

    def test_typed_tool_reports_the_retained_artifact(self):
        self.provider_metadata()
        self.state["image"] = png(9, 9)
        self.state["on_image"] = self.obstruct("agent.png")
        run = self.agent("all", {"prompt": "A blue robot", "output_file": "agent.png"})
        self.assertEqual(run.returncode, 0, run.stderr)
        result = self.tool_result()
        # The established failure marker, then the distinct retained object.
        self.assertTrue(result.startswith("error: "), result)
        failure = json.loads(result[len("error: ") :])
        self.assert_local_retained(failure, result)
        self.assertEqual(failure["code"], "IMAGE_MANIFEST_FINALIZE_FAILED")
        self.assertFalse(failure["ok"])
        self.assertTrue(failure["committed"])
        self.assertEqual(failure["path"], self.canonical(self.home / "agent.png"))
        self.assertEqual(failure["bytes"], len(self.state["image"]))
        self.assertEqual((failure["width"], failure["height"]), (9, 9))
        self.assertRegex(failure["operation_id"], r"^[0-9a-f]{16}$")
        # The paid artifact is kept, and nothing is requested again.
        self.assertEqual((self.home / "agent.png").read_bytes(), self.state["image"])
        self.assertEqual(len(self.image_requests()), 1)
        self.assertEqual(self.leftovers(), [])
        for secret in (TOKEN, ACCOUNT, self.url):
            self.assertNotIn(secret, result)

    @unittest.skipIf(WASM, "shell profiles have no terminal tool in wasm")
    def test_shell_interception_reports_the_retained_artifact(self):
        self.provider_metadata()
        self.state["image"] = png(10, 10)
        self.state["on_image"] = self.obstruct("agent.png")
        run = self.agent(
            "terminal",
            {"prompt": "A blue robot", "output_file": "agent.png"},
            tool="terminal",
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        result = self.tool_result()
        self.assertIn("exit: 1", result)
        failure = json.loads(result.split("\n")[1])
        self.assert_local_retained(failure, result)
        self.assertEqual(failure["code"], "IMAGE_MANIFEST_FINALIZE_FAILED")
        self.assertTrue(failure["committed"])
        self.assertEqual(failure["path"], self.canonical(self.home / "agent.png"))
        self.assertIn("kept", result)
        self.assertEqual((self.home / "agent.png").read_bytes(), self.state["image"])
        self.assertEqual(len(self.image_requests()), 1)

    @unittest.skipIf(WASM, "shell profiles have no terminal tool in wasm")
    def test_shell_interception_retains_without_json_on_stdout(self):
        self.state["image"] = png(10, 10)
        self.state["on_image"] = self.obstruct("agent.png")
        run = self.agent(
            "terminal",
            {"prompt": "A blue robot", "output_file": "agent.png"},
            tool="terminal",
            json_flag=False,
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        result = self.tool_result()
        # Plain-text commands keep an empty stdout; the diagnostic still says
        # the artifact was kept, and the file is still there.
        self.assertTrue(result.startswith("exit: 1\n"), result)
        self.assertIn("tny: image: ", result)
        self.assertIn("kept", result)
        self.assertNotIn('"kind":"image"', result)
        self.assertNotIn("committed", result)
        self.assertEqual((self.home / "agent.png").read_bytes(), self.state["image"])

    # --- the other callers ---------------------------------------------------

    def test_typed_tool_shares_the_manifest_options(self):
        self.state["image"] = png(24, 24)
        run = self.agent("all", {"prompt": "A blue robot", "output_file": "agent.png"})
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(self.tool_result())
        record = self.record("agent.png")
        self.assertEqual(
            result["manifest_path"], self.canonical(self.manifests("agent.png")[0])
        )
        self.assertEqual(record["operation_id"], result["operation_id"])
        self.assertEqual(record["prompt"], "A blue robot")
        schema = {
            tool["function"]["name"]: tool["function"]["parameters"]["properties"]
            for tool in self.state["chat"][0]["tools"]
        }
        self.assertEqual(
            schema["image_generate"]["persist_manifest"]["type"], "boolean"
        )
        self.assertEqual(schema["image_generate"]["from_manifest"]["type"], "string")
        self.assertEqual(schema["image_edit"]["artifact"]["type"], "string")

    @unittest.skipIf(WASM, "includes the native terminal interception")
    def test_successful_tools_keep_actual_provider_metadata(self):
        self.provider_metadata()
        for profile in ("all", "terminal"):
            with self.subTest(profile=profile):
                self.state["chat"].clear()
                output = f"{profile}-identified.png"
                run = self.agent(
                    profile,
                    {"prompt": "fixture", "output_file": output},
                    tool="terminal" if profile == "terminal" else "typed",
                )
                self.assertEqual(run.returncode, 0, run.stderr)
                raw = self.tool_result()
                result = json.loads(
                    raw.split("\n")[1] if profile == "terminal" else raw
                )
                actual = {"seed": 4242, "request_id": "provider-retained-sentinel"}
                self.assertEqual({key: result[key] for key in actual}, actual)
                self.assertEqual(self.record(output)["actual"], actual)
                self.assertEqual(len(self.image_requests()), 1)

    def test_typed_tool_honours_the_persistence_opt_out(self):
        run = self.agent(
            "all",
            {
                "prompt": "A blue robot",
                "output_file": "agent.png",
                "persist_manifest": False,
            },
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(self.tool_result())
        self.assertIsNone(result["manifest_path"])
        self.assertEqual(self.manifests("agent.png"), [])
        self.assertTrue((self.home / "agent.png").exists())

    def test_typed_tool_artifact_lineage_stays_inside_the_workspace(self):
        self.state["image"] = png(14, 14)
        self.result(self.generate("--json"))
        source = self.manifests()[0]
        outside = Path(tempfile.mkdtemp(prefix="tny-outside-"))
        self.addCleanup(
            lambda: subprocess.run(["rm", "-rf", str(outside)], check=False)
        )
        stolen = outside / "secret.png"
        stolen.write_bytes(png(2, 2))
        hostile = self.handwritten(
            "hostile.json",
            committed=True,
            artifacts=[
                {
                    "role": "native",
                    "path": str(stolen),
                    "sha256": sha(stolen.read_bytes()),
                    "width": 2,
                    "height": 2,
                    "mime_type": "image/png",
                    "bytes": stolen.stat().st_size,
                    "transform": None,
                    "source_operation": None,
                }
            ],
        )
        run = self.agent(
            "all",
            {
                "prompt": "A blue robot",
                "output_file": "lineage.png",
                "artifact": str(hostile),
            },
            tool_name="image_edit",
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        result = self.tool_result()
        self.assertTrue(result.startswith("error: "), result)
        self.assertIn("outside the allowed directories", result)
        self.assertEqual(self.image_requests(), [])
        self.assertEqual(stolen.read_bytes(), png(2, 2))
        # An in-workspace record is accepted through the same path.
        run = self.agent(
            "all",
            {
                "prompt": "A blue robot",
                "output_file": "lineage.png",
                "artifact": self.canonical(source),
            },
            tool_name="image_edit",
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertNotIn("error: ", self.tool_result())
        self.assertEqual(self.uploaded(), self.out.read_bytes())

    def test_unresolved_permission_writes_nothing_and_pays_nothing(self):
        # Ask mode with no interactive owner cannot resolve the prompt, so the
        # call never runs: no request, no record, no destination, no guard.
        run = self.agent(
            "all",
            {"prompt": "A blue robot", "output_file": "denied.png"},
            mode="ask",
        )
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual(self.image_requests(), [])
        self.assertEqual(list(self.home.glob("denied.png*")), [])

    @unittest.skipIf(WASM, "shell profiles have no terminal tool in wasm")
    def test_shell_interception_replays_under_the_recorded_identity(self):
        self.state["image"] = png(26, 13)
        self.result(self.generate("--json", prompt=b"intercepted source"))
        source = self.manifests()[0]
        run = self.agent(
            "terminal",
            {},
            tool="terminal",
            command=(
                f"tny image replay --manifest {source} "
                f"--output-file {self.home / 'intercepted.png'} --json"
            ),
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        result = self.tool_result()
        self.assertIn('"kind":"image"', result)
        self.assertIn("manifest:", result)
        self.assertEqual(self.image_requests()[-1][1]["prompt"], "intercepted source")
        record = self.record("intercepted.png")
        self.assertEqual(record["source"]["manifest"], self.canonical(source))
        self.assertEqual(record["operation"], "generate")

    def test_python_sdk_shares_the_manifest_options_and_lineage(self):
        tny, kit = self.toolkit()
        self.state["image"] = png(30, 15)
        result = kit.generate_image("A robot", output_file="sdk.png")
        self.assertEqual(result.operation_id, self.record("sdk.png")["operation_id"])
        self.assertEqual(
            self.canonical(result.manifest_path),
            self.canonical(self.manifests("sdk.png")[0]),
        )
        self.assertIsNone(result.seed)
        self.assertIsNone(result.request_id)
        quiet = kit.generate_image(
            "A robot", output_file="quiet.png", persist_manifest=False
        )
        self.assertIsNone(quiet.manifest_path)
        self.assertRegex(quiet.operation_id, r"^[0-9a-f]{16}$")
        self.assertEqual(self.manifests("quiet.png"), [])
        # Lineage through the same shared resolver.
        edited = kit.edit_image(
            "Make it blue",
            artifact=str(self.manifests("sdk.png")[0]),
            output_file="sdk-edit.png",
        )
        self.assertEqual(self.uploaded(), (self.home / "sdk.png").read_bytes())
        reference = self.record("sdk-edit.png")["references"][0]
        self.assertEqual(reference["sha256"], sha((self.home / "sdk.png").read_bytes()))
        self.assertEqual(
            edited.operation_id, self.record("sdk-edit.png")["operation_id"]
        )
        rerun = kit.generate_image(
            from_manifest=str(self.manifests("sdk.png")[0]),
            output_file="sdk-again.png",
        )
        self.assertEqual(self.record("sdk-again.png")["prompt"], "A robot")
        self.assertIsNotNone(rerun.manifest_path)


def argv_without_runner_binary():
    """run.sh appends $TNY; unittest must not read it as a test name."""
    kept = [sys.argv[0]]
    for argument in sys.argv[1:]:
        if (
            not argument.startswith("-")
            and os.path.isfile(argument)
            and os.access(argument, os.X_OK)
        ):
            continue
        kept.append(argument)
    return kept


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary())
