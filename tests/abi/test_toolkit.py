"""Native toolkit lifecycle/schema and HTTP boundary, without either SDK."""

import ctypes as c
import json
import os
import sys
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/fixtures"))
from toolkit_provider import ACCOUNT, PNG, TOKEN, Provider, workspace  # noqa: E402


class Bytes(c.Structure):
    _fields_ = [("ptr", c.c_void_p), ("len", c.c_uint64)]


class ToolkitABI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        name = "libtny.1.dylib" if sys.platform == "darwin" else "libtny.so.1"
        cls.lib = c.CDLL(str(ROOT / "build/lib" / name))
        signatures = {
            "create": ([Bytes, c.POINTER(c.c_void_p), c.c_void_p], c.c_int32),
            "run": ([c.c_void_p, c.c_void_p], c.c_int32),
            "cancel": ([c.c_void_p], c.c_int32),
            "result": ([c.c_void_p], Bytes),
            "destroy": ([c.POINTER(c.c_void_p)], c.c_int32),
        }
        for name, (args, result) in signatures.items():
            fn = getattr(cls.lib, "tny_toolkit_job_" + name)
            fn.argtypes, fn.restype = args, result

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name).resolve()
        workspace(self.path)
        self.provider = Provider()
        self.addCleanup(self.provider.close)

    def payload(self):
        return {
            "version": 1,
            "operation": "generate_image",
            "config": {
                "workspace": str(self.path),
                "settings_path": "settings.json",
                "chatgpt_token": TOKEN,
                "chatgpt_account_id": ACCOUNT,
                "codex_base_url": self.provider.url + "/backend-api/codex",
            },
            "request": {"prompt": "a tree", "output_file": "out.png"},
        }

    def create(self, payload, expected=0):
        raw = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
        buffer = c.create_string_buffer(raw)
        job = c.c_void_p()
        status = self.lib.tny_toolkit_job_create(
            Bytes(c.addressof(buffer), len(raw)), c.byref(job), None
        )
        self.assertEqual(status, expected)
        if expected:
            self.assertFalse(job.value)
        else:
            # Caller storage can be reused immediately after create.
            c.memset(buffer, 0, len(raw))
            self.addCleanup(self.lib.tny_toolkit_job_destroy, c.byref(job))
        return job

    def test_single_use_copied_input_and_borrowed_result(self):
        job = self.create(self.payload())
        self.assertEqual(self.lib.tny_toolkit_job_result(job).len, 0)
        self.assertEqual(self.provider.requests, [])
        self.assertEqual(self.lib.tny_toolkit_job_run(job, None), 0)
        result = self.lib.tny_toolkit_job_result(job)
        value = json.loads(c.string_at(result.ptr, result.len))
        self.assertEqual(value["bytes"], len(PNG))
        self.assertEqual(Path(value["path"]).read_bytes(), PNG)
        self.assertEqual(self.lib.tny_toolkit_job_run(job, None), -2)
        self.assertEqual(self.lib.tny_toolkit_job_destroy(c.byref(job)), 0)
        self.assertFalse(job.value)
        self.assertEqual(self.lib.tny_toolkit_job_destroy(c.byref(job)), 0)

    def test_invalid_schema_no_io(self):
        examples = [b"{}", b"[]", b"{}\0", b"{", b"\xff"]
        for raw in examples:
            with self.subTest(raw=raw):
                self.create(raw, -1)
        for mutate in (
            lambda p: p.update(version=2),
            lambda p: p.update(unknown=True),
            lambda p: p["config"].update(workspace="relative"),
            lambda p: p["config"].update(chatgpt_token=""),
            lambda p: p["config"].update(codex_base_url="ftp://bad"),
            lambda p: p["request"].update(prompt="bad\0text"),
            lambda p: p["request"].update(prompt="x" * 16385),
            lambda p: p["request"].update(quality="impossible"),
            lambda p: p["request"].update(images=["reference.png"]),
        ):
            payload = self.payload()
            mutate(payload)
            self.create(payload, -1)
        raw = (
            json.dumps(self.payload())
            .encode()
            .replace(b'"version": 1', b'"version": 1, "version": 1')
        )
        self.create(raw, -1)
        self.assertEqual(self.provider.requests, [])

    def test_create_defers_workspace_io_and_precancel(self):
        payload = self.payload()
        payload["config"]["workspace"] = str(self.path / "does-not-exist")
        job = self.create(payload)
        self.assertEqual(self.lib.tny_toolkit_job_run(job, None), -5)
        job = self.create(self.payload())
        self.assertEqual(self.lib.tny_toolkit_job_cancel(job), 0)
        self.assertEqual(self.lib.tny_toolkit_job_run(job, None), -12)
        self.assertEqual(self.provider.requests, [])

    def test_running_destroy_refused_and_cross_thread_cancel(self):
        self.provider.mode = "stall"
        output = self.path / "out.png"
        output.write_bytes(b"keep-existing")
        job = self.create(self.payload())
        result = []
        thread = threading.Thread(
            target=lambda: result.append(self.lib.tny_toolkit_job_run(job, None))
        )
        thread.start()
        try:
            self.assertTrue(self.provider.arrived.wait(5))
            self.assertEqual(self.lib.tny_toolkit_job_destroy(c.byref(job)), -3)
            self.assertEqual(self.lib.tny_toolkit_job_cancel(job), 0)
        finally:
            self.lib.tny_toolkit_job_cancel(job)
            thread.join(5)
        self.assertFalse(thread.is_alive())
        self.assertEqual(result, [-12])
        self.assertEqual(self.lib.tny_toolkit_job_result(job).len, 0)
        self.assertEqual(output.read_bytes(), b"keep-existing")
        self.assertEqual(list(self.path.glob("out.png.*")), [])

    @unittest.skipUnless(hasattr(os, "fork"), "requires fork")
    def test_forked_handles_fail_closed(self):
        job = self.create(self.payload())
        pid = os.fork()
        if not pid:
            ok = self.lib.tny_toolkit_job_run(job, None) == -2
            ok &= self.lib.tny_toolkit_job_cancel(job) == -2
            ok &= self.lib.tny_toolkit_job_destroy(c.byref(job)) == -2
            os._exit(0 if ok else 1)
        self.assertEqual(os.waitpid(pid, 0)[1], 0)


if __name__ == "__main__":
    unittest.main()
