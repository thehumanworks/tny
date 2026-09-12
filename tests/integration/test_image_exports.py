#!/usr/bin/env python3
"""Explicit local image exports and contact sheets (#125), end to end.

The stable checks are

    python3 tests/integration/test_image_exports.py <absolute tny> -k Export
    python3 tests/integration/test_image_exports.py <absolute tny> -k ContactSheet

and `tests/integration/run.sh` appends the binary path for the whole file.

Every transform case runs the real optional ImageMagick 7 executable: there is
no mocked converter, because the thing under test is what a real decoder and
encoder actually produce. The pixels are then decoded here, in Python, with
nothing but zlib — tny's own report of what it wrote is never the evidence.
Cases that do not need the converter (validation, permissions, aliases, the
missing-dependency path) run everywhere.

Controlled faults use disposable copies of the real executable: a wrapper
script that fails, stalls, or breaks the manifest mid-run. No fault hook exists
in the product.
"""

from __future__ import annotations

import json
import os
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import textwrap
import time
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def _binary_from_argv() -> None:
    """run.sh appends $TNY; make it visible to the shared fixture import."""
    for argument in sys.argv[1:]:
        if not argument.startswith("-") and os.path.isfile(argument):
            if os.access(argument, os.X_OK):
                os.environ["TNY"] = str(Path(argument).resolve())
                return


_binary_from_argv()
sys.path.insert(0, str(HERE))

# The generation fixture (throwaway HOME, fake provider, private records) is
# the prerequisite for artifact lineage; exports reuse it rather than inventing
# a second harness.
from test_image_workflow import (  # noqa: E402
    TNY,
    WASM,
    ImageFixture,
    argv_without_runner_binary,
    sha,
)

MAGICK = os.environ.get("TNY_TEST_MAGICK") or shutil.which("magick")


def magick_version() -> str | None:
    """The real tool's own version, read independently of tny."""
    if not MAGICK:
        return None
    try:
        out = subprocess.run(
            [MAGICK, "-version"], capture_output=True, text=True, timeout=60
        )
    except OSError:
        return None
    for token in out.stdout.split():
        if token[:2] in ("7.", "6."):
            return token
    return None


VERSION = magick_version()
HAVE_MAGICK7 = bool(VERSION and VERSION.startswith("7."))


def png(width: int, height: int, pixel) -> bytes:
    """A complete truecolour PNG whose pixels come from `pixel(x, y)`."""

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    rows = b"".join(
        b"\0" + b"".join(bytes(pixel(x, y)) for x in range(width))
        for y in range(height)
    )
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">II5B", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows, 1))
        + chunk(b"IEND", b"")
    )


def solid(width: int, height: int, colour) -> bytes:
    return png(width, height, lambda x, y: colour)


class Decoded:
    """An independently decoded 8-bit PNG: no ImageMagick, no tny."""

    def __init__(self, data: bytes):
        assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
        position, idat, header = 8, b"", None
        while position < len(data):
            length = struct.unpack(">I", data[position : position + 4])[0]
            kind = data[position + 4 : position + 8]
            payload = data[position + 8 : position + 8 + length]
            if kind == b"IHDR":
                header = struct.unpack(">IIBBBBB", payload)
            elif kind == b"IDAT":
                idat += payload
            position += 12 + length
        assert header, "no IHDR"
        self.width, self.height, depth, colour, _c, _f, interlace = header
        assert depth == 8 and colour in (2, 6) and interlace == 0, header
        self.channels = 3 if colour == 2 else 4
        stride = self.width * self.channels
        raw = zlib.decompress(idat)
        self.rows: list[bytes] = []
        previous = bytearray(stride)
        offset = 0
        for _ in range(self.height):
            filter_type = raw[offset]
            offset += 1
            line = bytearray(raw[offset : offset + stride])
            offset += stride
            for index in range(stride):
                left = line[index - self.channels] if index >= self.channels else 0
                up = previous[index]
                up_left = (
                    previous[index - self.channels] if index >= self.channels else 0
                )
                if filter_type == 1:
                    line[index] = (line[index] + left) & 0xFF
                elif filter_type == 2:
                    line[index] = (line[index] + up) & 0xFF
                elif filter_type == 3:
                    line[index] = (line[index] + (left + up) // 2) & 0xFF
                elif filter_type == 4:
                    estimate = left + up - up_left
                    da, db, dc = (
                        abs(estimate - left),
                        abs(estimate - up),
                        abs(estimate - up_left),
                    )
                    predictor = (
                        left if da <= db and da <= dc else up if db <= dc else up_left
                    )
                    line[index] = (line[index] + predictor) & 0xFF
            self.rows.append(bytes(line))
            previous = line

    def pixel(self, x: int, y: int) -> tuple[int, ...]:
        start = x * self.channels
        return tuple(self.rows[y][start : start + self.channels])

    def rgb(self, x: int, y: int) -> tuple[int, int, int]:
        return self.pixel(x, y)[:3]

    def alpha(self, x: int, y: int) -> int:
        return self.pixel(x, y)[3] if self.channels == 4 else 255

    def colours(self) -> set[tuple[int, ...]]:
        return {self.pixel(x, y) for y in range(self.height) for x in range(self.width)}


RED, GREEN, BLUE, YELLOW = (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)


class ExportFixture(ImageFixture):
    """The generation fixture plus the optional real converter on PATH."""

    def setUp(self):
        super().setUp()
        if WASM:
            self.skipTest("wasm rejects external transforms; covered by its own case")
        self.env["PATH"] = (
            f"{Path(MAGICK).parent}{os.pathsep}{os.environ['PATH']}"
            if MAGICK
            else os.environ["PATH"]
        )
        self.bin = self.home / "fakebin"
        self.bin.mkdir()

    # --- helpers ----------------------------------------------------------

    def needs_magick(self):
        if not HAVE_MAGICK7:
            self.skipTest(
                "optional ImageMagick 7 `magick` not on PATH (set TNY_TEST_MAGICK)"
            )

    def write(self, name: str, data: bytes) -> Path:
        path = self.home / name
        path.write_bytes(data)
        return path

    def run_cli(self, *args: str, env=None, timeout=120, check=False):
        run = subprocess.run(
            [TNY, "image", *args],
            cwd=self.home,
            env=env or self.env,
            capture_output=True,
            timeout=timeout,
        )
        if check:
            self.assertEqual(run.returncode, 0, run.stderr)
        return run

    def export(self, *args: str, **kwargs):
        return self.run_cli("export", *args, **kwargs)

    def sheet(self, *args: str, **kwargs):
        return self.run_cli("contact-sheet", *args, **kwargs)

    def json_result(self, run):
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(run.stdout)
        self.assertIs(result["ok"], True, result)
        self.assertNotIn("code", result)
        self.assertNotIn("error", result)
        return result

    def decode(self, path) -> Decoded:
        return Decoded(Path(path).read_bytes())

    def identify(self, path) -> str:
        """The real tool's own opinion of a file, read independently."""
        out = subprocess.run(
            [MAGICK, "identify", "-format", "%m %w %h", str(path)],
            capture_output=True,
            text=True,
            timeout=60,
        )
        return out.stdout.strip()

    def identity(self, path) -> tuple:
        info = os.stat(path)
        return (info.st_dev, info.st_ino, info.st_size, sha(Path(path).read_bytes()))

    def quoted_tool(self, name: str) -> str:
        """Resolve fixture helpers before the converter receives its clean PATH."""
        path = shutil.which(name)
        if not path:
            self.fail(f"required fixture tool {name!r} is not on the test-runner PATH")
        return shlex.quote(path)

    def fake_magick(self, body: str, name: str = "magick") -> dict:
        """A disposable wrapper used only to inject one failure mode."""
        script = self.bin / name
        script.write_text(body)
        script.chmod(0o755)
        env = dict(self.env)
        env["PATH"] = f"{self.bin}{os.pathsep}{env['PATH']}"
        return env

    def without_magick(self) -> dict:
        env = dict(self.env)
        env["PATH"] = "/nonexistent-for-tny-tests"
        return env


class Export(ExportFixture):
    """Single-image exports: policies, formats, guards and lineage."""

    def test_fit_contains_then_pads_to_the_exact_canvas(self):
        self.needs_magick()
        # Left half red, right half blue: an exactly halved source makes a
        # dropped or mirrored column impossible to miss.
        self.write("wide.png", png(40, 20, lambda x, y: RED if x < 20 else BLUE))
        result = self.json_result(
            self.export(
                "--image",
                "wide.png",
                "--output-file",
                "fit.png",
                "--size",
                "60x60",
                "--json",
            )
        )
        self.assertEqual(result["operation"], "export")
        self.assertFalse(result["native"])
        self.assertEqual(result["transform"]["policy"], "fit")
        self.assertEqual(result["mime_type"], "image/png")
        image = self.decode(self.home / "fit.png")
        self.assertEqual((image.width, image.height), (60, 60))
        self.assertEqual(self.identify(self.home / "fit.png"), "PNG 60 60")
        # 40x20 contained in 60x60 is 60x30, centred vertically.
        self.assertEqual(image.rgb(10, 30), RED)
        self.assertEqual(image.rgb(50, 30), BLUE)
        # Whole content preserved: the extremes of both halves survive.
        self.assertEqual(image.rgb(0, 16), RED)
        self.assertEqual(image.rgb(59, 43), BLUE)
        # The pad is the default transparent background, not invented content.
        self.assertEqual(image.alpha(30, 2), 0)
        self.assertEqual(image.alpha(30, 57), 0)
        self.assertEqual(result["sha256"], sha((self.home / "fit.png").read_bytes()))
        self.assertEqual(result["width"], 60)
        self.assertEqual(result["height"], 60)

    def test_crop_covers_and_keeps_the_gravity_region(self):
        self.needs_magick()
        # A wide source crops horizontally: left half red, right half blue.
        self.write("wide.png", png(40, 20, lambda x, y: RED if x < 20 else BLUE))
        # A tall source crops vertically: top half green, bottom half yellow.
        self.write("tall.png", png(20, 40, lambda x, y: GREEN if y < 20 else YELLOW))
        cases = [
            ("wide.png", "west", (2, 10), RED),
            ("wide.png", "east", (17, 10), BLUE),
            ("wide.png", "northwest", (2, 10), RED),
            ("wide.png", "southeast", (17, 10), BLUE),
            ("tall.png", "north", (10, 2), GREEN),
            ("tall.png", "south", (10, 17), YELLOW),
            ("tall.png", "center", (10, 2), GREEN),
            ("tall.png", "center", (10, 17), YELLOW),
        ]
        for source, gravity, probe, expected in cases:
            with self.subTest(source=source, gravity=gravity, probe=probe):
                out = f"crop-{source}-{gravity}-{probe[1]}.png"
                result = self.json_result(
                    self.export(
                        "--image",
                        source,
                        "--output-file",
                        out,
                        "--size",
                        "20x20",
                        "--fit",
                        "crop",
                        "--gravity",
                        gravity,
                        "--json",
                    )
                )
                self.assertEqual(result["transform"]["gravity"], gravity)
                image = self.decode(self.home / out)
                self.assertEqual((image.width, image.height), (20, 20))
                self.assertEqual(image.rgb(*probe), expected)
                # A cover crop fills the canvas: no padding anywhere.
                self.assertTrue(all(image.alpha(x, 0) == 255 for x in range(20)))
        # The two extremes of one axis really are different regions.
        west = self.decode(self.home / "crop-wide.png-west-10.png")
        east = self.decode(self.home / "crop-wide.png-east-10.png")
        self.assertNotEqual(west.rgb(17, 10), east.rgb(17, 10))

    def test_pad_shrinks_only_and_never_enlarges(self):
        self.needs_magick()
        self.write("small.png", solid(10, 10, YELLOW))
        self.export(
            "--image",
            "small.png",
            "--output-file",
            "pad.png",
            "--size",
            "40x40",
            "--fit",
            "pad",
            "--background",
            "#102030",
            check=True,
        )
        image = self.decode(self.home / "pad.png")
        self.assertEqual((image.width, image.height), (40, 40))
        # Content is still 10x10, centred: 15..24 on both axes.
        self.assertEqual(image.rgb(20, 20), YELLOW)
        self.assertEqual(image.rgb(15, 15), YELLOW)
        self.assertEqual(image.rgb(14, 20), (16, 32, 48))
        self.assertEqual(image.rgb(25, 20), (16, 32, 48))
        # A larger source is shrunk to fit rather than cropped.
        self.write("big.png", png(80, 40, lambda x, y: RED if x < 40 else BLUE))
        self.export(
            "--image",
            "big.png",
            "--output-file",
            "pad2.png",
            "--size",
            "40x40",
            "--fit",
            "pad",
            check=True,
        )
        shrunk = self.decode(self.home / "pad2.png")
        self.assertEqual((shrunk.width, shrunk.height), (40, 40))
        self.assertEqual(shrunk.rgb(5, 20), RED)
        self.assertEqual(shrunk.rgb(35, 20), BLUE)
        self.assertEqual(shrunk.alpha(20, 2), 0)

    def test_format_is_forced_and_reported_from_the_bytes(self):
        self.needs_magick()
        self.write("wide.png", png(40, 20, lambda x, y: RED if x < 20 else BLUE))
        # A .png name with --format jpeg must produce JPEG, and say so.
        result = self.json_result(
            self.export(
                "--image",
                "wide.png",
                "--output-file",
                "lies.png",
                "--size",
                "32x16",
                "--format",
                "jpeg",
                "--json",
            )
        )
        data = (self.home / "lies.png").read_bytes()
        self.assertEqual(result["mime_type"], "image/jpeg")
        self.assertEqual(data[:3], b"\xff\xd8\xff")
        self.assertEqual(self.identify(self.home / "lies.png"), "JPEG 32 16")
        self.assertEqual(result["transform"]["background"], "#000000")
        webp = self.json_result(
            self.export(
                "--image",
                "wide.png",
                "--output-file",
                "out.webp",
                "--size",
                "24x24",
                "--format",
                "webp",
                "--json",
            )
        )
        blob = (self.home / "out.webp").read_bytes()
        self.assertEqual(webp["mime_type"], "image/webp")
        self.assertEqual(blob[:4], b"RIFF")
        self.assertEqual(blob[8:12], b"WEBP")
        self.assertEqual(self.identify(self.home / "out.webp"), "WEBP 24 24")
        # A JPEG source is accepted too, and its derived size is the canvas,
        # not the source's.
        derived = self.json_result(
            self.export(
                "--image",
                "lies.png",
                "--output-file",
                "again.png",
                "--size",
                "64x8",
                "--json",
            )
        )
        self.assertEqual((derived["width"], derived["height"]), (64, 8))
        self.assertEqual(self.identify(self.home / "again.png"), "PNG 64 8")

    def test_converter_argv_uses_only_private_staged_paths(self):
        """Observe the boundary, not IM7's treatment of caller filenames."""
        self.needs_magick()
        source = self.write("ordinary-source.png", solid(12, 12, RED))
        before = self.identity(source)
        log = self.home / "converter-argv.jsonl"
        env = self.fake_magick(
            textwrap.dedent(f"""\
                #!{sys.executable}
                import json
                import os
                import stat
                import sys
                from pathlib import Path

                args = sys.argv[1:]
                paths = []
                for arg in args:
                    _, separator, value = arg.partition(":")
                    if separator and value.startswith("/"):
                        path = Path(value)
                        paths.append({{
                            "arg": arg,
                            "parent_mode": stat.S_IMODE(path.parent.stat().st_mode),
                            "parent_uid": path.parent.stat().st_uid,
                            "symlink": path.is_symlink(),
                        }})
                with open({str(log)!r}, "a") as stream:
                    stream.write(json.dumps({{"argv": args, "paths": paths}}) + "\\n")
                os.execv({str(MAGICK)!r}, [{str(MAGICK)!r}, *args])
                """)
        )
        self.export(
            "--image",
            str(source),
            "--output-file",
            "copy.png",
            "--size",
            "12x12",
            env=env,
            check=True,
        )
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        operations = [call for call in calls if call["argv"] != ["-version"]]
        self.assertEqual(len(operations), 2, calls)
        producing, decoder = operations
        for call in calls:
            for arg in call["argv"]:
                self.assertNotIn(
                    source.name,
                    arg,
                    "caller source path crossed converter argv boundary",
                )
        # Check both actual invocations, including forced coders and the
        # private directory's permissions while it still exists. The wrapper
        # records and execs the real converter without changing its arguments.
        inputs = producing["paths"]
        self.assertEqual(len(inputs), 2, producing)
        staged_input, staged_output = (entry["arg"] for entry in inputs)
        self.assertRegex(
            staged_input, r"\Apng:/tmp/tny-image-export-[A-Za-z0-9]+/src00\.png\Z"
        )
        stage = staged_input.removeprefix("png:").rsplit("/", 1)[0]
        self.assertEqual(staged_output, f"png32:{stage}/out.png")
        self.assertEqual(producing["argv"][-1], staged_output)
        self.assertEqual([entry["arg"] for entry in decoder["paths"]], [staged_output])
        self.assertEqual(decoder["argv"][-2:], [staged_output, "null:"])
        for call in operations:
            for entry in call["paths"]:
                self.assertEqual(entry["parent_mode"], 0o700, entry)
                self.assertEqual(entry["parent_uid"], os.getuid(), entry)
                self.assertFalse(entry["symlink"], entry)
            # No unforced absolute path can hide alongside the staged inputs.
            self.assertFalse(any(arg.startswith("/") for arg in call["argv"]), call)
        decoded = self.decode(self.home / "copy.png")
        self.assertEqual((decoded.width, decoded.height), (12, 12))
        self.assertEqual(decoded.rgb(6, 6), RED)
        self.assertEqual(self.identity(source), before)

    def test_sources_are_never_modified(self):
        self.needs_magick()
        source = self.write(
            "keep.png", png(40, 20, lambda x, y: RED if x < 20 else BLUE)
        )
        before = self.identity(source)
        self.export(
            "--image",
            "keep.png",
            "--output-file",
            "copy.png",
            "--size",
            "20x20",
            check=True,
        )
        self.assertEqual(self.identity(source), before)
        # A failing export leaves the source identical too.
        failed = self.export(
            "--image", "keep.png", "--output-file", "copy.png", "--size", "20x20"
        )
        self.assertEqual(failed.returncode, 1)
        self.assertEqual(self.identity(source), before)

    def test_existing_destination_needs_the_explicit_flag(self):
        self.needs_magick()
        self.write("wide.png", png(40, 20, lambda x, y: RED if x < 20 else BLUE))
        target = self.write("taken.png", b"not an image, and not to be lost")
        original = self.identity(target)
        run = self.export(
            "--image", "wide.png", "--output-file", "taken.png", "--size", "16x16"
        )
        self.assertEqual(run.returncode, 1)
        self.assertIn(b"--overwrite", run.stderr)
        self.assertEqual(self.identity(target), original)
        self.assertEqual(self.manifests("taken.png"), [])
        run = self.export(
            "--image",
            "wide.png",
            "--output-file",
            "taken.png",
            "--size",
            "16x16",
            "--overwrite",
            "--json",
        )
        result = self.json_result(run)
        self.assertTrue(result["committed"])
        self.assertEqual(self.decode(target).width, 16)

    def test_every_alias_of_a_source_is_refused(self):
        self.needs_magick()
        source = self.write(
            "self.png", png(40, 20, lambda x, y: RED if x < 20 else BLUE)
        )
        before = self.identity(source)
        # An ordinary single-link source first: nothing else about the file is
        # unusual, so only the alias rule itself can refuse this.
        plain = [
            ("--image", "self.png", "--output-file", "self.png"),
            ("--image", "self.png", "--output-file", "./self.png"),
            ("--image", "self.png", "--output-file", "../%s/self.png" % self.home.name),
        ]
        for alias in plain:
            with self.subTest(alias=alias):
                run = self.export(*alias, "--size", "8x8", "--overwrite")
                self.assertEqual(run.returncode, 1, run.stdout)
                self.assertEqual(self.identity(source), before)
        # Then the link forms, which must be refused just as firmly.
        os.symlink(source, self.home / "link.png")
        os.link(source, self.home / "hard.png")
        linked = [
            ("--image", "self.png", "--output-file", "link.png"),
            ("--image", "link.png", "--output-file", "self.png"),
            ("--image", "self.png", "--output-file", "hard.png"),
            ("--image", "hard.png", "--output-file", "self.png"),
        ]
        for alias in linked:
            with self.subTest(alias=alias):
                run = self.export(*alias, "--size", "8x8", "--overwrite")
                self.assertEqual(run.returncode, 1, run.stdout)
                self.assertEqual(self.identity(source), before)
        # A record's own name is refused as a destination as well.
        self.state["image"] = png(8, 8, lambda x, y: GREEN)
        self.generate("--json")
        record = self.manifests()[0]
        run = self.export(
            "--image",
            "self.png",
            "--output-file",
            str(record),
            "--size",
            "8x8",
            "--overwrite",
        )
        self.assertEqual(run.returncode, 1)
        self.assertTrue(json.loads(record.read_text())["committed"])

    def test_expression_like_filenames_are_data_not_syntax(self):
        self.needs_magick()
        tricky = [
            "100%evil.png",
            "@list.png",
            "sel[0].png",
            "png:notacoder.png",
            "a b*c.png",
        ]
        for name in tricky:
            with self.subTest(name=name):
                self.write(name, solid(8, 8, GREEN))
                out = f"out-{name}"
                run = self.export(
                    "--image",
                    name,
                    "--output-file",
                    out,
                    "--size",
                    "12x12",
                    "--no-manifest",
                    "--json",
                )
                result = self.json_result(run)
                self.assertEqual(result["width"], 12)
                image = self.decode(self.home / out)
                self.assertEqual(image.rgb(6, 6), GREEN)
                # The name was data: no sibling file was created from it.
                self.assertFalse((self.home / "list.png").exists())
                self.assertFalse((self.home / "notacoder.png").exists())

    def test_a_truncated_pixel_stream_is_not_committed(self):
        self.needs_magick()
        whole = png(40, 20, lambda x, y: RED if x < 20 else BLUE)
        self.write("cut.png", whole[: len(whole) // 2])
        run = self.export(
            "--image", "cut.png", "--output-file", "cut-out.png", "--size", "20x20"
        )
        self.assertEqual(run.returncode, 1, run.stdout)
        self.assertFalse((self.home / "cut-out.png").exists())
        # The header alone is a valid PNG header; only decoding the pixels
        # settles it, which is why exit status is never the evidence.
        self.assertEqual(whole[:8], (self.home / "cut.png").read_bytes()[:8])

    def test_unusable_inputs_and_canvases_fail_before_any_output(self):
        self.write("text.txt", b"this is not an image at all")
        self.write("wide.png", png(40, 20, lambda x, y: RED if x < 20 else BLUE))
        cases = [
            ("--image", "text.txt", "--output-file", "a.png", "--size", "8x8"),
            ("--image", "missing.png", "--output-file", "b.png", "--size", "8x8"),
            ("--image", "wide.png", "--output-file", "c.png", "--size", "16384x16384"),
            ("--image", "wide.png", "--output-file", "d.png", "--size", "99999x8"),
            ("--image", "wide.png", "--output-file", "e.png", "--size", "0x8"),
            (
                "--image",
                "wide.png",
                "--output-file",
                "f.png",
                "--size",
                "8x8",
                "--fit",
                "squish",
            ),
            (
                "--image",
                "wide.png",
                "--output-file",
                "g.png",
                "--size",
                "8x8",
                "--background",
                "red",
            ),
            (
                "--image",
                "wide.png",
                "--output-file",
                "h.png",
                "--size",
                "8x8",
                "--format",
                "gif",
            ),
            (
                "--image",
                "wide.png",
                "--output-file",
                "i.png",
                "--size",
                "8x8",
                "--columns",
                "2",
            ),
        ]
        for case in cases:
            with self.subTest(case=case):
                run = self.export(*case)
                self.assertEqual(run.returncode, 1, run.stdout)
                self.assertFalse((self.home / case[3]).exists())
                self.assertEqual(self.manifests(case[3]), [])

    def test_a_missing_or_old_converter_is_actionable_and_harmless(self):
        self.write("wide.png", png(40, 20, lambda x, y: RED if x < 20 else BLUE))
        run = self.export(
            "--image",
            "wide.png",
            "--output-file",
            "none.png",
            "--size",
            "8x8",
            env=self.without_magick(),
        )
        self.assertEqual(run.returncode, 1)
        self.assertIn(b"ImageMagick 7", run.stderr)
        self.assertFalse((self.home / "none.png").exists())
        self.assertEqual(self.manifests("none.png"), [])
        # ImageMagick 6 is not a fallback, even when it answers to `magick`.
        six = self.fake_magick(
            "#!/bin/sh\n"
            'if [ "$1" = -version ]; then\n'
            "  echo 'Version: ImageMagick 6.9.13-12 Q16 x86_64'\n"
            "  exit 0\n"
            "fi\n"
            "echo 'IM6 must never be used' >&2\n"
            'touch "$PWD/im6-was-run"\n'
            "exit 0\n"
        )
        run = self.export(
            "--image",
            "wide.png",
            "--output-file",
            "six.png",
            "--size",
            "8x8",
            env=six,
        )
        self.assertEqual(run.returncode, 1)
        self.assertIn(b"6.9.13-12", run.stderr)
        self.assertFalse((self.home / "six.png").exists())
        self.assertFalse((self.home / "im6-was-run").exists())
        # Something that is not an ImageMagick at all is refused too.
        mute = self.fake_magick("#!/bin/sh\nexit 0\n")
        run = self.export(
            "--image",
            "wide.png",
            "--output-file",
            "mute.png",
            "--size",
            "8x8",
            env=mute,
        )
        self.assertEqual(run.returncode, 1)
        self.assertFalse((self.home / "mute.png").exists())

    def test_generation_never_invokes_a_converter(self):
        # The provider fixture supplies the image; a `magick` that would leave
        # a marker proves generation, editing and replay never call it.
        marker = self.home / "converter-ran"
        env = self.fake_magick(
            f"#!/bin/sh\n{self.quoted_tool('touch')} {marker}\nexit 1\n"
        )
        self.state["image"] = png(16, 8, lambda x, y: GREEN)
        run = subprocess.run(
            [TNY, "image", "generate", "--output-file", "gen.png", "--json"],
            input=b"An orange robot",
            cwd=self.home,
            env=env,
            capture_output=True,
            timeout=60,
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertFalse(marker.exists())
        record = self.manifests("gen.png")[0]
        rerun = subprocess.run(
            [
                TNY,
                "image",
                "replay",
                "--manifest",
                str(record),
                "--output-file",
                "again.png",
                "--json",
            ],
            cwd=self.home,
            env=env,
            capture_output=True,
            timeout=60,
        )
        self.assertEqual(rerun.returncode, 0, rerun.stderr)
        self.assertFalse(marker.exists())
        # Generation also works when no converter exists at all.
        plain = subprocess.run(
            [TNY, "image", "generate", "--output-file", "third.png", "--json"],
            input=b"An orange robot",
            cwd=self.home,
            env=self.without_magick(),
            capture_output=True,
            timeout=60,
        )
        self.assertEqual(plain.returncode, 0, plain.stderr)

    def test_derived_records_carry_lineage_and_are_not_provider_operations(self):
        self.needs_magick()
        self.state["image"] = png(32, 16, lambda x, y: GREEN)
        self.generate("--json")
        native = self.manifests()[0]
        native_record = json.loads(native.read_text())
        result = self.json_result(
            self.export(
                "--artifact",
                str(native),
                "--output-file",
                "derived.png",
                "--size",
                "8x8",
                "--fit",
                "crop",
                "--gravity",
                "southeast",
                "--background",
                "#010203",
                "--json",
            )
        )
        record = json.loads(self.manifests("derived.png")[0].read_text())
        self.assertEqual(record["version"], 1)
        self.assertEqual(record["operation"], "export")
        self.assertEqual(record["status"], "succeeded")
        self.assertIsNone(record["prompt"])
        self.assertEqual(record["requested"]["provider"], "local")
        self.assertEqual(record["references"], [])
        transform = record["transform"]
        self.assertEqual(transform["policy"], "crop")
        self.assertEqual(transform["gravity"], "southeast")
        self.assertEqual(transform["background"], "#010203")
        self.assertEqual((transform["width"], transform["height"]), (8, 8))
        self.assertIsNone(transform["grid"])
        self.assertEqual(transform["tool"], "imagemagick")
        self.assertTrue(transform["tool_version"].startswith("7."))
        source = transform["sources"][0]
        self.assertEqual(source["path"], str(self.root / "result.png"))
        self.assertEqual(source["sha256"], sha((self.home / "result.png").read_bytes()))
        self.assertEqual(source["source_manifest"], self.canonical(native))
        self.assertEqual(source["source_operation"], native_record["operation_id"])
        artifact = record["artifacts"][0]
        self.assertEqual(artifact["role"], "derived")
        self.assertFalse(artifact["native"])
        self.assertEqual(
            artifact["sha256"], sha((self.home / "derived.png").read_bytes())
        )
        self.assertEqual(artifact["sha256"], result["sha256"])
        # The original record is untouched by the export.
        self.assertEqual(json.loads(native.read_text()), native_record)
        # A derived record is not a provider operation and may not be rerun.
        derived_record = self.manifests("derived.png")[0]
        rerun = self.cli(
            "replay",
            "--manifest",
            str(derived_record),
            "--output-file",
            "no.png",
            "--json",
        )
        self.assertEqual(rerun.returncode, 1)
        self.assertIn(b"local image export", rerun.stderr)
        self.assertFalse((self.home / "no.png").exists())
        self.assertEqual(len(self.image_requests()), 1)  # only the generate
        # It is still a real artifact, so editing from it uploads those bytes.
        edited = self.cli(
            "edit",
            "--artifact",
            str(derived_record),
            "--output-file",
            "edited.png",
            "--json",
            prompt=b"Make it blue",
        )
        self.assertEqual(edited.returncode, 0, edited.stderr)
        self.assertEqual(self.uploaded(), (self.home / "derived.png").read_bytes())
        lineage = json.loads(self.manifests("edited.png")[0].read_text())
        self.assertEqual(lineage["references"][0]["sha256"], artifact["sha256"])
        self.assertEqual(
            lineage["references"][0]["source_manifest"], self.canonical(derived_record)
        )

    def test_approved_snapshot_survives_source_and_record_changes_during_probe(self):
        self.needs_magick()
        approved = png(16, 8, lambda x, y: RED)
        replacement = png(8, 16, lambda x, y: BLUE)
        for intercepted, artifact in (
            (False, False),
            (True, False),
            (False, True),
            (True, True),
        ):
            with self.subTest(intercepted=intercepted, artifact=artifact):
                self.state["image"] = approved
                source_name = f"snapshot-source-{int(intercepted)}-{int(artifact)}.png"
                self.generate("--json", output=source_name)
                native = self.manifests(source_name)[0]
                native_record = json.loads(native.read_text())
                source = self.home / source_name
                self.write("replacement.png", replacement)
                # Version probing happens after the execution-time grant check.
                # Change both bytes and record there. Only the captured red
                # pixels and original resolved lineage may be consumed.
                self.env = self.fake_magick(
                    f'#!/bin/sh\nif [ "$1" = -version ]; then\n'
                    f'{self.quoted_tool("cp")} "{self.home}/replacement.png" "{source}"\n'
                    f'{self.quoted_tool("rm")} -f "{native}"\nfi\nexec "{MAGICK}" "$@"\n'
                )
                output = f"snapshot-out-{int(intercepted)}-{int(artifact)}.png"
                kind = "artifact" if artifact else "image"
                input_path = native if artifact else source
                run = self.agent(
                    "all",
                    {
                        "sources": [{kind: str(input_path)}],
                        "output_file": output,
                        "size": "8x8",
                    },
                    tool_name="image_export",
                    command=(
                        f'tny image export --{kind} "{input_path}" --output-file '
                        f"{output} --size 8x8 --json"
                    )
                    if intercepted
                    else None,
                )
                self.assertEqual(run.returncode, 0, run.stderr)
                body = self.tool_result()
                payload = json.loads(body.splitlines()[1] if intercepted else body)
                self.assertTrue(payload["ok"], payload)
                pixels = self.decode(self.home / output)
                self.assertEqual(pixels.pixel(4, 4)[:3], RED)
                record = json.loads(self.manifests(output)[0].read_text())
                lineage = record["transform"]["sources"][0]
                self.assertEqual(lineage["sha256"], sha(approved))
                self.assertEqual(
                    lineage["source_operation"],
                    native_record["operation_id"] if artifact else None,
                )
                self.assertEqual((lineage["width"], lineage["height"]), (16, 8))
                self.assertEqual(
                    payload["transform"]["source_dimensions"],
                    [{"width": 16, "height": 8}],
                )
                self.assertEqual(source.read_bytes(), replacement)

    def test_direct_and_mixed_source_dimensions_are_returned_and_recorded(self):
        self.needs_magick()
        direct = self.write("direct.png", png(19, 7, lambda x, y: RED))
        self.state["image"] = png(3, 25, lambda x, y: BLUE)
        self.generate("--json")
        native = self.manifests()[0]
        for sheet in (False, True):
            with self.subTest(sheet=sheet):
                output = "dims-sheet.png" if sheet else "dims-export.png"
                arguments = ["--image", str(direct)]
                expected = [{"width": 19, "height": 7}]
                if sheet:
                    arguments += ["--artifact", str(native), "--columns", "2"]
                    expected.append({"width": 3, "height": 25})
                run = self.run_cli(
                    "contact-sheet" if sheet else "export",
                    *arguments,
                    "--output-file",
                    output,
                    "--size",
                    "40x40",
                    "--json",
                )
                payload = self.json_result(run)
                self.assertEqual(payload["transform"]["source_dimensions"], expected)
                record = json.loads(self.manifests(output)[0].read_text())
                self.assertEqual(
                    [
                        {k: s[k] for k in ("width", "height")}
                        for s in record["transform"]["sources"]
                    ],
                    expected,
                )
                self.assertEqual(direct.read_bytes(), png(19, 7, lambda x, y: RED))

    def test_output_hash_commit_boundary(self):
        self.needs_magick()
        source = self.write("hash-source.png", png(9, 7, lambda x, y: RED))
        fault = os.environ.get("TNY_TEST_EXPORT_HASH_FAULT") == "1"
        for persist in (False, True):
            with self.subTest(persist=persist):
                name = "hash-record.png" if persist else "hash-no-record.png"
                output = self.write(name, b"old destination")
                run = self.export(
                    "--image",
                    str(source),
                    "--output-file",
                    name,
                    "--size",
                    "4x4",
                    "--overwrite",
                    "--json",
                    *([] if persist else ["--no-manifest"]),
                )
                payload = json.loads(run.stdout)
                if fault:
                    self.assertEqual(run.returncode, 1, run.stderr)
                    self.assertIn(b"cannot hash", run.stderr)
                    self.assertFalse(payload["committed"])
                    self.assertIsNone(payload["path"])
                    self.assertEqual(output.read_bytes(), b"old destination")
                else:
                    self.assertEqual(run.returncode, 0, run.stderr)
                    self.assertTrue(payload["committed"])
                    self.assertEqual(payload["sha256"], sha(output.read_bytes()))
                self.assertEqual(source.read_bytes(), png(9, 7, lambda x, y: RED))

    def test_the_persistence_opt_out_records_nothing(self):
        self.needs_magick()
        self.write("wide.png", png(40, 20, lambda x, y: RED if x < 20 else BLUE))
        result = self.json_result(
            self.export(
                "--image",
                "wide.png",
                "--output-file",
                "quiet.png",
                "--size",
                "10x10",
                "--no-manifest",
                "--json",
            )
        )
        self.assertIsNone(result["manifest_path"])
        self.assertEqual(self.manifests("quiet.png"), [])
        self.assertEqual(
            sorted(p.name for p in self.home.glob("quiet.png*")), ["quiet.png"]
        )

    def test_a_failed_record_finalization_keeps_the_derived_artifact(self):
        self.retained_cli_json("export")

    def test_sheet_failed_record_finalization_keeps_the_derived_artifact(self):
        self.retained_cli_json("contact-sheet")

    def retained_cli_json(self, operation):
        self.needs_magick()
        self.write("wide.png", png(40, 20, lambda x, y: RED if x < 20 else BLUE))
        # A disposable wrapper breaks the record while the conversion runs:
        # the intent file exists by then, so finalizing it must fail while the
        # artifact itself still commits.
        env = self.fake_magick(
            textwrap.dedent(
                f"""\
                #!/bin/sh
                for record in {self.home}/retained.png.tny-image-*.json; do
                    [ -e "$record" ] || continue
                    {self.quoted_tool("rm")} -f "$record"
                    {self.quoted_tool("mkdir")} -p "$record"
                done
                exec {MAGICK} "$@"
                """
            )
        )
        run = self.run_cli(
            operation,
            "--image",
            "wide.png",
            "--output-file",
            "retained.png",
            "--size",
            "12x12",
            "--json",
            env=env,
        )
        self.assertEqual(run.returncode, 1)
        payload = json.loads(run.stdout)
        self.assertEqual(payload["code"], "IMAGE_MANIFEST_FINALIZE_FAILED")
        self.assertFalse(payload["ok"])
        self.assertTrue(payload["committed"])
        self.assertEqual(payload["path"], str(self.root / "retained.png"))
        artifact = self.home / "retained.png"
        self.assertTrue(artifact.exists())
        self.assertEqual(payload["sha256"], sha(artifact.read_bytes()))
        self.assertEqual(self.decode(artifact).width, 12)

    def test_a_failing_converter_leaves_the_destination_alone(self):
        self.write("wide.png", png(40, 20, lambda x, y: RED if x < 20 else BLUE))
        existing = self.write("target.png", solid(4, 4, YELLOW))
        before = self.identity(existing)
        env = self.fake_magick(
            "#!/bin/sh\n"
            'if [ "$1" = -version ]; then\n'
            "  echo 'Version: ImageMagick 7.1.2-31 Q16 aarch64'\n"
            "  exit 0\n"
            "fi\n"
            "exit 3\n"
        )
        run = self.export(
            "--image",
            "wide.png",
            "--output-file",
            "target.png",
            "--size",
            "8x8",
            "--overwrite",
            env=env,
        )
        self.assertEqual(run.returncode, 1)
        self.assertEqual(self.identity(existing), before)
        record = json.loads(self.manifests("target.png")[0].read_text())
        self.assertEqual(record["status"], "failed")
        self.assertFalse(record["committed"])
        self.assertEqual(record["artifacts"], [])
        self.assertEqual(record["error"]["code"], "IMAGE_EXPORT_FAILED")
        # A converter that exits 0 without producing anything is not success.
        empty = self.fake_magick(
            "#!/bin/sh\n"
            'if [ "$1" = -version ]; then\n'
            "  echo 'Version: ImageMagick 7.1.2-31 Q16 aarch64'\n"
            "  exit 0\n"
            "fi\n"
            "exit 0\n"
        )
        run = self.export(
            "--image",
            "wide.png",
            "--output-file",
            "target.png",
            "--size",
            "8x8",
            "--overwrite",
            "--no-manifest",
            env=empty,
        )
        self.assertEqual(run.returncode, 1)
        self.assertEqual(self.identity(existing), before)
        # Nor is a child that produced a perfectly good file and then failed:
        # a nonzero exit means the operation did not happen.
        if HAVE_MAGICK7:
            late = self.fake_magick(
                textwrap.dedent(
                    f"""\
                    #!/bin/sh
                    if [ "$1" = -version ]; then
                        echo 'Version: ImageMagick 7.1.2-31 Q16 aarch64'
                        exit 0
                    fi
                    {MAGICK} "$@" || exit 4
                    # Only the producing run fails; the verifying decode of
                    # the file it wrote still succeeds, so the exit status is
                    # the only thing that says this export did not happen.
                    for argument in "$@"; do
                        [ "$argument" = "null:" ] && exit 0
                    done
                    exit 5
                    """
                )
            )
            run = self.export(
                "--image",
                "wide.png",
                "--output-file",
                "target.png",
                "--size",
                "8x8",
                "--overwrite",
                "--no-manifest",
                env=late,
            )
            self.assertEqual(run.returncode, 1, run.stdout)
        self.assertEqual(self.identity(existing), before)

    def test_the_written_bytes_decide_the_format_and_canvas(self):
        self.needs_magick()
        self.write("wide.png", png(40, 20, lambda x, y: RED if x < 20 else BLUE))
        existing = self.write("wrong.png", solid(4, 4, YELLOW))
        before = self.identity(existing)
        # A converter that exits 0 after producing a perfectly valid image of
        # the wrong size: exit status is not evidence, the bytes are.
        wrong_size = self.fake_magick(
            textwrap.dedent(
                f"""\
                #!/bin/sh
                if [ "$1" = -version ]; then
                    echo 'Version: ImageMagick 7.1.2-31 Q16 aarch64'
                    exit 0
                fi
                for argument in "$@"; do
                    case "$argument" in
                        png32:*|jpeg:*|webp:*)
                            exec {MAGICK} -size 9x9 canvas:red "$argument" ;;
                    esac
                done
                exit 0
                """
            )
        )
        run = self.export(
            "--image",
            "wide.png",
            "--output-file",
            "wrong.png",
            "--size",
            "8x8",
            "--overwrite",
            "--no-manifest",
            env=wrong_size,
        )
        self.assertEqual(run.returncode, 1, run.stdout)
        self.assertIn(b"9x9", run.stderr)
        self.assertEqual(self.identity(existing), before)
        # The same holds for a valid image in the wrong encoding.
        wrong_format = self.fake_magick(
            textwrap.dedent(
                f"""\
                #!/bin/sh
                if [ "$1" = -version ]; then
                    echo 'Version: ImageMagick 7.1.2-31 Q16 aarch64'
                    exit 0
                fi
                for argument in "$@"; do
                    case "$argument" in
                        png32:*)
                            exec {MAGICK} -size 8x8 canvas:red "jpeg:${{argument#png32:}}" ;;
                    esac
                done
                exit 0
                """
            )
        )
        run = self.export(
            "--image",
            "wide.png",
            "--output-file",
            "wrong.png",
            "--size",
            "8x8",
            "--overwrite",
            "--no-manifest",
            env=wrong_format,
        )
        self.assertEqual(run.returncode, 1, run.stdout)
        self.assertIn(b"image/jpeg", run.stderr)
        self.assertEqual(self.identity(existing), before)

    def test_cancelling_stops_the_converter_and_writes_nothing(self):
        self.write("wide.png", png(40, 20, lambda x, y: RED if x < 20 else BLUE))
        marker = self.home / "still-running"
        env = self.fake_magick(
            textwrap.dedent(
                f"""\
                #!/bin/sh
                if [ "$1" = -version ]; then
                    echo 'Version: ImageMagick 7.1.2-31 Q16 aarch64'
                    exit 0
                fi
                echo $$ > {marker}
                {self.quoted_tool("sleep")} 120
                """
            )
        )
        # An unrelated sleeper proves the cancellation kills the export's own
        # child tree and nothing else.
        sentinel = subprocess.Popen(["sleep", "60"])
        try:
            process = subprocess.Popen(
                [
                    TNY,
                    "image",
                    "export",
                    "--image",
                    "wide.png",
                    "--output-file",
                    "cancel.png",
                    "--size",
                    "8x8",
                ],
                cwd=self.home,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            deadline = time.time() + 30
            while time.time() < deadline and not marker.exists():
                time.sleep(0.05)
            self.assertTrue(marker.exists(), "the fake converter never started")
            child = int(marker.read_text().strip())
            process.send_signal(signal.SIGINT)
            process.communicate(timeout=60)
            self.assertNotEqual(process.returncode, 0)
            self.assertFalse((self.home / "cancel.png").exists())
            deadline = time.time() + 30
            while time.time() < deadline:
                try:
                    os.kill(child, 0)
                except OSError:
                    break
                time.sleep(0.05)
            with self.assertRaises(OSError):
                os.kill(child, 0)  # the converter was stopped and reaped
            self.assertIsNone(sentinel.poll())  # unrelated processes survive
            record = json.loads(self.manifests("cancel.png")[0].read_text())
            self.assertIn(record["status"], ("cancelled", "failed"))
            self.assertFalse(record["committed"])
        finally:
            sentinel.kill()
            sentinel.wait()

    def test_two_exports_of_one_destination_do_not_interleave(self):
        self.needs_magick()
        self.write("wide.png", png(40, 20, lambda x, y: RED if x < 20 else BLUE))
        processes = [
            subprocess.Popen(
                [
                    TNY,
                    "image",
                    "export",
                    "--image",
                    "wide.png",
                    "--output-file",
                    "shared.png",
                    "--size",
                    "40x40",
                    "--no-manifest",
                    "--json",
                ],
                cwd=self.home,
                env=self.env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            for _ in range(2)
        ]
        results = [process.communicate(timeout=120) for process in processes]
        codes = [process.returncode for process in processes]
        self.assertIn(0, codes)
        self.assertEqual(sorted(codes), [0, 1])
        image = self.decode(self.home / "shared.png")
        self.assertEqual((image.width, image.height), (40, 40))
        self.assertEqual(
            sorted(p.name for p in self.home.glob("shared.png*")), ["shared.png"]
        )
        self.assertTrue(
            any(b"already writing this output" in err for _out, err in results)
        )

    def test_typed_tool_and_interception_share_the_service(self):
        self.needs_magick()
        self.write("wide.png", png(40, 20, lambda x, y: RED if x < 20 else BLUE))
        run = self.agent(
            "all",
            {
                "sources": [{"image": "wide.png"}],
                "output_file": "typed.png",
                "size": "20x10",
                "fit": "crop",
            },
            tool_name="image_export",
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        payload = json.loads(self.tool_result())
        self.assertTrue(payload["ok"])
        self.assertFalse(payload["native"])
        self.assertEqual(payload["operation"], "export")
        image = self.decode(self.home / "typed.png")
        self.assertEqual((image.width, image.height), (20, 10))
        self.assertEqual(
            json.loads(self.manifests("typed.png")[0].read_text())["operation"],
            "export",
        )
        intercepted = self.agent(
            "all",
            {},
            command="tny image export --image wide.png --output-file shell.png "
            "--size 12x6 --json",
        )
        self.assertEqual(intercepted.returncode, 0, intercepted.stderr)
        body = self.tool_result()
        self.assertTrue(body.startswith("exit: 0\n"))
        self.assertIn('"operation":"export"', body)
        self.assertEqual(self.decode(self.home / "shell.png").width, 12)

    def test_a_denied_export_reads_transforms_and_writes_nothing(self):
        self.needs_magick()
        self.write("wide.png", png(40, 20, lambda x, y: RED if x < 20 else BLUE))
        # Ask mode with no interactive owner cannot resolve the prompt, so the
        # call never runs: nothing is converted, written or recorded.
        run = self.agent(
            "all",
            {
                "sources": [{"image": "wide.png"}],
                "output_file": "denied.png",
                "size": "20x10",
            },
            tool_name="image_export",
            mode="ask",
        )
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual(list(self.home.glob("denied.png*")), [])
        self.assertEqual(self.manifests("denied.png"), [])

    def test_help_documents_the_export_surface(self):
        run = self.run_cli("export", "--help")
        self.assertEqual(run.returncode, 0, run.stderr)
        for flag in (
            b"--size",
            b"--fit",
            b"--gravity",
            b"--background",
            b"--format",
            b"--overwrite",
            b"--no-manifest",
            b"--columns",
            b"--labels",
        ):
            self.assertIn(flag, run.stdout)
        self.assertIn(b"ImageMagick 7", run.stdout)
        sheet = self.run_cli("contact-sheet", "--help")
        self.assertEqual(sheet.stdout, run.stdout)


class ContactSheet(ExportFixture):
    """Ordered grids: cell geometry, source order and fixed numeric labels."""

    def four_sources(self):
        for name, colour in (
            ("s0.png", RED),
            ("s1.png", GREEN),
            ("s2.png", BLUE),
            ("s3.png", YELLOW),
        ):
            self.write(name, solid(10, 10, colour))
        return ["s0.png", "s1.png", "s2.png", "s3.png"]

    def test_grid_keeps_source_order_and_leaves_the_remainder_background(self):
        self.needs_magick()
        names = self.four_sources()
        args = []
        for name in names:
            args += ["--image", name]
        result = self.json_result(
            self.sheet(
                *args,
                "--output-file",
                "sheet.png",
                "--size",
                "101x61",
                "--columns",
                "2",
                "--background",
                "#102030",
                "--json",
            )
        )
        grid = result["transform"]["grid"]
        self.assertEqual(
            grid, {"columns": 2, "rows": 2, "cell_width": 50, "cell_height": 30}
        )
        image = self.decode(self.home / "sheet.png")
        self.assertEqual((image.width, image.height), (101, 61))
        centres = {
            RED: (25, 15),
            GREEN: (75, 15),
            BLUE: (25, 45),
            YELLOW: (75, 45),
        }
        for colour, (x, y) in centres.items():
            self.assertEqual(image.rgb(x, y), colour)
        # floor() cells leave a one-pixel remainder, which stays background.
        self.assertEqual(image.rgb(100, 60), (16, 32, 48))
        self.assertEqual(image.rgb(100, 0), (16, 32, 48))
        # Reversing the inputs reverses the cells: order is the caller's.
        reversed_args = []
        for name in reversed(names):
            reversed_args += ["--image", name]
        self.sheet(
            *reversed_args,
            "--output-file",
            "reversed.png",
            "--size",
            "101x61",
            "--columns",
            "2",
            "--background",
            "#102030",
            check=True,
        )
        flipped = self.decode(self.home / "reversed.png")
        self.assertEqual(flipped.rgb(25, 15), YELLOW)
        self.assertEqual(flipped.rgb(75, 45), RED)

    def test_default_columns_follow_the_square_root(self):
        self.needs_magick()
        names = self.four_sources()
        args = []
        for name in names[:3]:
            args += ["--image", name]
        result = self.json_result(
            self.sheet(*args, "--output-file", "three.png", "--size", "60x60", "--json")
        )
        self.assertEqual(result["transform"]["grid"]["columns"], 2)
        self.assertEqual(result["transform"]["grid"]["rows"], 2)
        self.assertEqual(result["transform"]["grid"]["cell_width"], 30)
        image = self.decode(self.home / "three.png")
        self.assertEqual(image.rgb(15, 15), RED)
        self.assertEqual(image.rgb(45, 15), GREEN)
        self.assertEqual(image.rgb(15, 45), BLUE)
        # The fourth cell of a 3-source sheet is empty background.
        self.assertEqual(image.alpha(45, 45), 0)

    def test_numeric_labels_are_fixed_bitmaps_and_deterministic(self):
        self.needs_magick()
        names = self.four_sources()
        args = []
        for name in names[:2]:
            args += ["--image", name]
        self.sheet(
            *args,
            "--output-file",
            "labels.png",
            "--size",
            "128x64",
            "--columns",
            "2",
            "--labels",
            "numbers",
            "--background",
            "#000000",
            check=True,
        )
        image = self.decode(self.home / "labels.png")
        self.assertEqual((image.width, image.height), (128, 64))
        # Cells are 64x64, so the label scale is 1: a 7x9 white box with black
        # numerals at the cell's top-left corner.
        white, black = (255, 255, 255), (0, 0, 0)
        self.assertEqual(image.rgb(0, 0), white)  # box padding
        self.assertEqual(image.rgb(6, 8), white)
        self.assertEqual(image.rgb(7, 0), RED)  # content resumes past the box
        # "1": the 5x7 glyph's stem sits in the middle column.
        self.assertEqual(image.rgb(3, 2), black)
        self.assertEqual(image.rgb(3, 7), black)
        self.assertEqual(image.rgb(1, 2), white)
        # "2" in the second cell has a filled bottom row instead.
        self.assertEqual(image.rgb(64 + 1, 7), black)
        self.assertEqual(image.rgb(64 + 5, 7), black)
        self.assertEqual(image.rgb(64 + 3, 2), white)
        first = (self.home / "labels.png").read_bytes()
        # Reject volatile encoder metadata even when both exports happen in
        # the same second. Parse chunk boundaries instead of searching pixels.
        offset = 8
        while offset < len(first):
            length = int.from_bytes(first[offset : offset + 4], "big")
            kind = first[offset + 4 : offset + 8]
            body = first[offset + 8 : offset + 8 + length]
            self.assertNotEqual(kind, b"tIME")
            if kind == b"tEXt":
                self.assertNotIn(
                    body.split(b"\0", 1)[0].lower(),
                    (b"date:create", b"date:modify", b"date:timestamp"),
                )
            offset += length + 12
        self.assertEqual(offset, len(first))
        self.sheet(
            *args,
            "--output-file",
            "labels2.png",
            "--size",
            "128x64",
            "--columns",
            "2",
            "--labels",
            "numbers",
            "--background",
            "#000000",
            check=True,
        )
        self.assertEqual(first, (self.home / "labels2.png").read_bytes())
        self.assertEqual(
            json.loads(self.manifests("labels.png")[0].read_text())["transform"][
                "labels"
            ],
            "numbers",
        )

    def test_cells_too_small_for_a_label_are_a_validation_error(self):
        names = self.four_sources()
        args = []
        for name in names:
            args += ["--image", name]
        run = self.sheet(
            *args, "--output-file", "tiny.png", "--size", "12x12", "--labels", "numbers"
        )
        self.assertEqual(run.returncode, 1)
        self.assertIn(b"too small", run.stderr)
        self.assertFalse((self.home / "tiny.png").exists())
        self.assertEqual(self.manifests("tiny.png"), [])
        # Without labels the same grid is fine.
        run = self.sheet(
            *args, "--output-file", "tiny.png", "--size", "12x12", "--no-manifest"
        )
        if HAVE_MAGICK7:
            self.assertEqual(run.returncode, 0, run.stderr)

    def test_one_bad_cell_stops_the_whole_sheet(self):
        self.needs_magick()
        names = self.four_sources()
        whole = solid(10, 10, GREEN)
        self.write("s2.png", whole[: len(whole) // 2])  # truncated pixel stream
        identities = {name: self.identity(self.home / name) for name in names}
        args = []
        for name in names:
            args += ["--image", name]
        run = self.sheet(*args, "--output-file", "partial.png", "--size", "64x64")
        self.assertEqual(run.returncode, 1, run.stdout)
        self.assertFalse((self.home / "partial.png").exists())
        for name in names:
            self.assertEqual(self.identity(self.home / name), identities[name])
        record = json.loads(self.manifests("partial.png")[0].read_text())
        self.assertEqual(record["status"], "failed")
        self.assertFalse(record["committed"])
        self.assertEqual(record["artifacts"], [])

    def test_sources_may_mix_files_and_artifacts(self):
        self.needs_magick()
        self.state["image"] = solid(16, 16, BLUE)
        self.generate("--json")
        record = self.manifests()[0]
        self.write("s0.png", solid(10, 10, RED))
        result = self.json_result(
            self.sheet(
                "--image",
                "s0.png",
                "--artifact",
                str(record),
                "--output-file",
                "mixed.png",
                "--size",
                "64x32",
                "--columns",
                "2",
                "--json",
            )
        )
        self.assertEqual(result["transform"]["sources"], 2)
        image = self.decode(self.home / "mixed.png")
        self.assertEqual(image.rgb(16, 16), RED)
        self.assertEqual(image.rgb(48, 16), BLUE)
        sheet_record = json.loads(self.manifests("mixed.png")[0].read_text())
        sources = sheet_record["transform"]["sources"]
        self.assertEqual(sources[0]["path"], str(self.root / "s0.png"))
        self.assertIsNone(sources[0]["source_manifest"])
        self.assertEqual(sources[1]["source_manifest"], self.canonical(record))
        self.assertEqual(
            sources[1]["sha256"], sha((self.home / "result.png").read_bytes())
        )

    def test_the_source_count_is_bounded(self):
        names = []
        for index in range(65):
            name = f"many{index:02d}.png"
            self.write(name, solid(2, 2, GREEN))
            names.append(name)
        args = []
        for name in names:
            args += ["--image", name]
        run = self.sheet(*args, "--output-file", "many.png", "--size", "64x64")
        self.assertEqual(run.returncode, 1)
        self.assertFalse((self.home / "many.png").exists())

    def test_typed_sheet_tool_and_interception_share_the_service(self):
        self.needs_magick()
        self.four_sources()
        run = self.agent(
            "all",
            {
                "sources": [{"image": "s0.png"}, {"image": "s1.png"}],
                "output_file": "typed-sheet.png",
                "size": "64x32",
                "columns": 2,
                "labels": "numbers",
            },
            tool_name="image_contact_sheet",
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        payload = json.loads(self.tool_result())
        self.assertEqual(payload["operation"], "contact_sheet")
        self.assertEqual(payload["transform"]["grid"]["columns"], 2)
        image = self.decode(self.home / "typed-sheet.png")
        self.assertEqual((image.width, image.height), (64, 32))
        self.assertEqual(image.rgb(16, 24), RED)
        self.assertEqual(image.rgb(48, 24), GREEN)
        intercepted = self.agent(
            "all",
            {},
            command="tny image contact-sheet --image s0.png --image s1.png "
            "--output-file shell-sheet.png --size 40x20 --columns 2 --json",
        )
        self.assertEqual(intercepted.returncode, 0, intercepted.stderr)
        self.assertIn('"operation":"contact_sheet"', self.tool_result())
        self.assertEqual(self.decode(self.home / "shell-sheet.png").width, 40)


class ConverterOwnership(unittest.TestCase):
    """Optional instrumented runner built from the real converter source."""

    def test_exited_child_keeps_signal_authority_through_cleanup(self):
        fixture = os.environ.get("TNY_TEST_IMAGE_OWNERSHIP")
        if not fixture:
            self.skipTest("set TNY_TEST_IMAGE_OWNERSHIP to the compiled C fixture")
        for mode in ("cancel", "deadline"):
            with self.subTest(mode=mode):
                run = subprocess.run(
                    [fixture, mode], capture_output=True, text=True, timeout=5
                )
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn("exited_at_stop=1", run.stdout)
                self.assertIn("invalid_authority=0", run.stdout)
                self.assertIn("fully_reaped=1", run.stdout)


class ConverterBounds(ExportFixture):
    def bounded_child(self, behavior, *, probe=False, cancel=True):
        self.write("input.png", png(8, 8, lambda x, y: RED))
        marker = self.home / "child.pid"
        body = "#!/bin/sh\n"
        if not probe:
            body += (
                'if [ "$1" = -version ]; then\n'
                "echo 'Version: ImageMagick 7.1.2-31 Q16'\nexit 0\nfi\n"
            )
        body += f'echo $$ > "{marker}"\n' + behavior + "\n"
        env = self.fake_magick(body)
        output = self.write("bounded.png", b"original destination")
        process = subprocess.Popen(
            [
                TNY,
                "image",
                "export",
                "--image",
                "input.png",
                "--output-file",
                "bounded.png",
                "--overwrite",
                "--size",
                "8x8",
                "--json",
            ],
            cwd=self.home,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        child = None
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and not marker.exists():
                time.sleep(0.01)
            self.assertTrue(marker.exists(), "converter did not start")
            child = int(marker.read_text())
            # Let the direct child exit (inherited writer case), or fill stdout.
            time.sleep(0.2)
            started = time.monotonic()
            if cancel:
                process.send_signal(signal.SIGINT)
            # Controlled-fault runs can compile a shorter wall constant. This
            # changes only the test's outer assertion, never product behavior.
            wall = float(os.environ.get("TNY_TEST_EXPORT_WALL_SECONDS", "75"))
            stdout, stderr = process.communicate(timeout=5 if cancel else wall + 5)
            elapsed = time.monotonic() - started
            self.assertEqual(process.returncode, 130 if cancel else 1, stderr)
            payload = json.loads(stdout)
            self.assertFalse(payload["committed"])
            self.assertEqual(output.read_bytes(), b"original destination")
            if cancel:
                self.assertLess(elapsed, 5)
                self.assertEqual(payload["code"], "IMAGE_EXPORT_CANCELLED")
            else:
                self.assertLess(elapsed, wall + 5)
                self.assertGreater(elapsed, max(0, wall - 5))
                self.assertIn(b"time limit", stderr)
        finally:
            if child:
                try:
                    os.killpg(child, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=5)

    def test_continuous_stdout_cancellation_is_bounded(self):
        self.bounded_child(f"exec {self.quoted_tool('yes')} fixture")

    def test_inherited_stdout_cancellation_is_bounded(self):
        self.bounded_child(f"{self.quoted_tool('sleep')} 120 &\nexit 0")

    def test_version_probe_cancellation_is_bounded(self):
        self.bounded_child(f"{self.quoted_tool('sleep')} 120", probe=True)

    def test_continuous_stdout_deadline_is_bounded(self):
        self.bounded_child(f"exec {self.quoted_tool('yes')} fixture", cancel=False)

    def test_inherited_stdout_deadline_is_bounded(self):
        self.bounded_child(f"{self.quoted_tool('sleep')} 120 &\nexit 0", cancel=False)


class Platform(unittest.TestCase):
    """What the wasm build must do without any external tool."""

    def test_wasm_rejects_transforms_before_touching_anything(self):
        if not WASM:
            self.skipTest("native build; the wasm gate runs in the wasm CI job")
        run = subprocess.run(
            [
                TNY,
                "image",
                "export",
                "--image",
                "a.png",
                "--output-file",
                "b.png",
                "--size",
                "8x8",
            ],
            capture_output=True,
            timeout=120,
        )
        self.assertEqual(run.returncode, 1)
        self.assertFalse(Path("b.png").exists())


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary())
