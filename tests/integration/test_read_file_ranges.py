#!/usr/bin/env python3
"""Ranged reads preserve the historical byte/line contract on both provider wires."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import test_execution_code_mode as fixture
from code_mode_fixture import python_string


@unittest.skipIf(
    fixture.WASM, "native execution; Wasm has an explicit unsupported seam"
)
class ReadFileRanges(unittest.TestCase):
    setUp = fixture.ExecutionCodeMode.setUp
    close_server = fixture.ExecutionCodeMode.close_server
    start = fixture.ExecutionCodeMode.start
    run_code = fixture.ExecutionCodeMode.run_code

    def test_range_contract_on_both_wires(self):
        files = {
            "lines": b"first\nsecond\nthird",
            "empty": b"",
            "trailing": b"a\n",
            "unicode": "héllo\r\n世界\r\nlast".encode(),
            "nul": b"before\0after\nnext\n",
            "invalid": b"ok\n\xffbad\n",
            "long": b"x" * 100000 + b"\nselected\n",
            "selected": b"z" * 9000 + b"\ntail",
        }
        for name, data in files.items():
            (self.workspace / name).write_bytes(data)
        cases = [
            ("lines", 1, 1, "first\n"),
            ("lines", 2, 1, "second\n"),
            ("lines", 3, 1, "third\n"),
            ("lines", 4, 1, ""),
            ("lines", 2, 2, "second\nthird\n"),
            ("lines", 0, 2, "first\nsecond\n"),
            ("lines", -1, 1, "first\n"),
            ("lines", 0, 0, "first\nsecond\nthird"),
            ("lines", 2, -1, "second\nthird\n"),
            ("empty", 1, 1, "\n"),
            ("empty", 2, 1, ""),
            ("trailing", 1, 0, "a\n\n"),
            ("trailing", 2, 1, "\n"),
            ("trailing", 3, 1, ""),
            ("unicode", 2, 2, "世界\r\nlast\n"),
            # The existing tool string boundary ends at NUL, not at file EOF.
            ("nul", 1, 1, "before"),
            ("nul", 2, 1, "next\n"),
            ("invalid", 1, 1, "ok\n"),
            ("long", 2, 1, "selected\n"),
            ("selected", 1, 1, "z" * 9000 + "\n"),
        ]
        # Assert inside code mode to avoid its aggregate output preview limit.
        code = "import json\n"
        for name, offset, limit, expected in cases:
            args = json.dumps({"path": name, "offset": offset, "limit": limit})
            code += (
                f"actual = tools.call('read_file', {python_string(args)})\n"
                f"assert actual == {expected!r}, ({name!r}, {offset}, {limit}, repr(actual))\n"
            )
        code += "print('all range cases passed')"
        for wire in ("responses", "chat"):
            with self.subTest(wire=wire):
                self.assertIn("all range cases passed", self.run_code(code, wire=wire))

    def test_size_guard_and_scoped_recovery_on_both_wires(self):
        threshold = 32768
        path = self.workspace / "large"
        original = b"first\n" + b"x" * (threshold * 4) + b"\nlast\n"
        path.write_bytes(original)
        for name, size in (
            ("below", threshold - 1),
            ("equal", threshold),
            ("above", threshold + 1),
        ):
            (self.workspace / name).write_bytes(b"z" * size)
        (self.workspace / "image").write_bytes(b"\x89PNG\r\n\x1a\n" + b"x" * threshold)
        code = "import json\n"
        code += "def read(**args): return tools.call('read_file', json.dumps(args))\n"
        code += "assert read(path='below') == 'z' * 32767\n"
        code += "assert read(path='equal') == 'z' * 32768\n"
        code += "for args in ({'path':'above'}, {'path':'large'}, {'path':'large','offset':2}, {'path':'large','offset':2,'limit':1}):\n"
        code += "    result = read(**args)\n    assert 'No contents returned or copied' in result, result\n    assert 'grep_files/rg' in result and len(result) < 1024, result\n"
        code += "assert read(path='large', offset=1, limit=1) == 'first\\n'\n"
        code += "assert read(path='large', offset=3, limit=1) == 'last\\n'\n"
        code += "assert 'use read_image' in read(path='image')\n"
        code += "print('bounded file recovery passed')"
        for wire in ("responses", "chat"):
            with self.subTest(wire=wire):
                self.assertIn(
                    "bounded file recovery passed",
                    self.run_code(code, wire=wire, no_save=False),
                )
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual(list((self.home / ".tny").glob("sessions/*/*/results/*")), [])

    @unittest.skipUnless(sys.platform.startswith("linux"), "fopencookie is a GNU seam")
    def test_read_errors_do_not_require_scanning_unselected_tail(self):
        compiler = os.environ.get("CC", "cc")
        if not shutil.which(compiler):
            self.skipTest("C compiler unavailable")
        root = Path(__file__).resolve().parents[2]
        source = (root / "src/core/tools_fs.c").read_text()
        start = source.index("static bool read_file_range(")
        end = source.index("\nstatic char *t_read_file(", start)
        fixture_source = (root / "tests/fixtures/read_file_range_faults.c").read_text()
        with tempfile.TemporaryDirectory(prefix="tny-range-faults-") as temporary:
            path = Path(temporary) / "faults.c"
            binary = Path(temporary) / "faults"
            path.write_text(
                fixture_source.replace(
                    "/* PRODUCTION_READ_FILE_RANGE */", source[start:end]
                )
            )
            subprocess.run(
                [
                    compiler,
                    "-Os",
                    "-std=c11",
                    "-D_GNU_SOURCE",
                    "-ffunction-sections",
                    "-fdata-sections",
                    "-Iinclude",
                    "-Isrc",
                    "-Ithird_party/yyjson",
                    str(path),
                    "src/util/util.c",
                    "src/util/alloc.c",
                    "src/core/image.c",
                    "-Wl,--gc-sections",
                    "-pthread",
                    "-lm",
                    "-o",
                    str(binary),
                ],
                cwd=root,
                check=True,
                capture_output=True,
            )
            subprocess.run([str(binary)], check=True, capture_output=True)

    def test_failures_images_and_symlinks_on_both_wires(self):
        (self.workspace / "directory").mkdir()
        (self.workspace / "image").write_bytes(b"\x89PNG\r\n\x1a\n" + b"x" * 20)
        (self.workspace / "target").write_text("first\nsecond\n")
        (self.workspace / "link").symlink_to("target")
        code = ""
        for path, fragment in (
            ("missing", "cannot read"),
            ("directory", "cannot read"),
            ("image", "use read_image"),
        ):
            args = json.dumps({"path": path, "offset": 1, "limit": 1})
            code += f"assert {fragment!r} in tools.call('read_file', {python_string(args)})\n"
        args = json.dumps({"path": "link", "offset": 2, "limit": 1})
        code += (
            f"assert tools.call('read_file', {python_string(args)}) == 'second\\n'\n"
        )
        code += "print('errors and symlink passed')"
        for wire in ("responses", "chat"):
            with self.subTest(wire=wire):
                self.assertIn(
                    "errors and symlink passed", self.run_code(code, wire=wire)
                )


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
