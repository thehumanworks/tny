"""Native toolkit lifecycle/schema and HTTP boundary, without either SDK."""

import ctypes as c
import hashlib
import json
import os
import subprocess
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
        cls.lib.tny_error_code.argtypes = [c.c_void_p]
        cls.lib.tny_error_code.restype = c.c_int32
        cls.lib.tny_error_message.argtypes = [c.c_void_p]
        cls.lib.tny_error_message.restype = Bytes
        cls.lib.tny_error_free.argtypes = [c.c_void_p]
        cls.lib.tny_error_free.restype = None

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name).resolve()
        workspace(self.path)
        self.provider = Provider()
        self.addCleanup(self.provider.close)

    def staging(self, stem="out.png"):
        """Temporary files beside an output, which must always be none. The
        per-operation record is the documented new default artifact and is
        asserted separately (docs/images.md)."""
        return sorted(
            p.name for p in self.path.glob(stem + ".*") if ".tny-image-" not in p.name
        )

    def records(self, stem="out.png"):
        return [
            json.loads(p.read_text())
            for p in sorted(self.path.glob(stem + ".tny-image-*.json"))
        ]

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

    def result_json(self, job):
        result = self.lib.tny_toolkit_job_result(job)
        self.assertTrue(result.len)
        return json.loads(c.string_at(result.ptr, result.len))

    def run_job(self, job, expected=None):
        """Run once, returning the native diagnostic text.

        ``expected`` pins the status; ``None`` only requires a failure, for the
        provider-decided categories this suite deliberately does not fix.
        """
        error = c.c_void_p()
        status = self.lib.tny_toolkit_job_run(job, c.byref(error))
        if expected is None:
            self.assertNotEqual(status, 0)
        else:
            self.assertEqual(status, expected)
        message = b""
        if error.value:
            self.assertEqual(self.lib.tny_error_code(error), status)
            native = self.lib.tny_error_message(error)
            message = c.string_at(native.ptr, native.len) if native.len else b""
            self.lib.tny_error_free(error)
        return message

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
        # Additive dimension metadata (#122) from the returned bytes.
        self.assertEqual((value["width"], value["height"]), (1, 1))
        self.assertEqual(value["requested_size"], "auto")
        self.assertEqual(value["effective_size"], "auto")
        self.assertEqual(value["size_status"], "auto")
        self.assertTrue(value["native"])
        self.assertIsNone(value["transform"])
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
            # strict_size is an allowed boolean, not a string or a new spelling.
            lambda p: p["request"].update(strict_size="yes"),
            lambda p: p["request"].update(strict_size=1),
            lambda p: p["request"].update(strictSize=True),
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

    def test_strict_size_is_accepted_and_settled_before_any_request(self):
        payload = self.payload()
        payload["request"].update(size="1x1", strict_size=True)
        job = self.create(payload)
        self.assertEqual(self.lib.tny_toolkit_job_run(job, None), 0)
        result = self.lib.tny_toolkit_job_result(job)
        value = json.loads(c.string_at(result.ptr, result.len))
        self.assertEqual(value["size_status"], "match")
        self.assertEqual(len(self.provider.requests), 1)
        # An unusable strict request is an argument error with no provider call.
        # Errors have no result, except for exactly these locally decided
        # strict-size rejections (docs/sdk-toolkit.md).
        payload = self.payload()
        payload["request"].update(strict_size=True)
        job = self.create(payload)
        message = self.run_job(job, -1)
        detail = self.result_json(job)
        self.assertEqual(detail["code"], "IMAGE_STRICT_SIZE_INVALID")
        self.assertEqual(detail["kind"], "image")
        self.assertEqual(detail["operation"], "generate")
        self.assertFalse(detail["ok"])
        self.assertFalse(detail["committed"])
        self.assertIsNone(detail["path"])
        self.assertEqual(detail["requested_size"], "auto")
        # Nothing was sent and nothing was returned, so nothing is invented.
        self.assertIsNone(detail["effective_size"])
        self.assertIsNone(detail["width"])
        self.assertIsNone(detail["height"])
        self.assertIsNone(detail["mime_type"])
        self.assertEqual(detail["size_status"], "auto")  # no size was requested
        self.assertEqual(message, b"toolkit operation failed")
        self.assertEqual(len(self.provider.requests), 1)

    def test_manifest_provenance_rides_the_existing_result(self):
        """No new symbol or record: provenance is additive result JSON."""
        job = self.create(self.payload())
        self.assertEqual(self.lib.tny_toolkit_job_run(job, None), 0)
        value = self.result_json(job)
        self.assertRegex(value["operation_id"], r"^[0-9a-f]{16}$")
        record = self.records()[0]
        self.assertEqual(len(self.records()), 1)
        self.assertEqual(
            value["manifest_path"],
            str(self.path / f"out.png.tny-image-{value['operation_id']}.json"),
        )
        self.assertEqual(record["operation_id"], value["operation_id"])
        self.assertEqual(record["status"], "succeeded")
        self.assertTrue(record["committed"])
        self.assertEqual(
            record["artifacts"][0]["sha256"],
            hashlib.sha256((self.path / "out.png").read_bytes()).hexdigest(),
        )
        # The fixture returns no seed or request id, and none is invented.
        self.assertIsNone(value["seed"])
        self.assertIsNone(value["request_id"])
        for secret in (TOKEN, ACCOUNT, self.provider.url):
            self.assertNotIn(secret, json.dumps(record))
        # The opt-out records nothing and says so.
        payload = self.payload()
        payload["request"].update(output_file="quiet.png", persist_manifest=False)
        job = self.create(payload)
        self.assertEqual(self.lib.tny_toolkit_job_run(job, None), 0)
        quiet = self.result_json(job)
        self.assertIsNone(quiet["manifest_path"])
        self.assertEqual(self.records("quiet.png"), [])
        # An unknown option is still rejected rather than silently discarded.
        payload = self.payload()
        payload["request"]["persist_manifests"] = False
        self.create(payload, -1)

    def test_strict_mismatch_detail_is_local_and_commits_nothing(self):
        output = self.path / "out.png"
        output.write_bytes(b"keep-existing")
        payload = self.payload()
        # The fixture image is 1x1, so an exact 2x2 request cannot be met.
        payload["request"].update(size="2x2", strict_size=True)
        job = self.create(payload)
        message = self.run_job(job, -10)
        detail = self.result_json(job)
        self.assertEqual(detail["code"], "IMAGE_SIZE_MISMATCH")
        self.assertEqual(detail["requested_size"], "2x2")
        self.assertEqual(detail["effective_size"], "2x2")  # exactly what was sent
        self.assertEqual((detail["width"], detail["height"]), (1, 1))
        self.assertEqual(detail["size_status"], "mismatch")
        self.assertEqual(detail["mime_type"], "image/png")
        self.assertIsNone(detail["path"])
        self.assertFalse(detail["committed"])
        # The paid bytes are discarded; the old file and the run status stand.
        self.assertEqual(output.read_bytes(), b"keep-existing")
        self.assertEqual(self.staging(), [])
        # The record of the paid attempt is a failure, never a committed one,
        # and it stays out of the ABI result entirely.
        record = self.records()[0]
        self.assertEqual(len(self.records()), 1)
        self.assertEqual(record["status"], "failed")
        self.assertFalse(record["committed"])
        self.assertEqual(record["error"]["code"], "IMAGE_SIZE_MISMATCH")
        self.assertEqual(len(self.provider.requests), 1)  # never retried
        self.assertEqual(message, b"toolkit operation failed")
        raw = json.dumps(detail)
        for secret in (TOKEN, ACCOUNT, self.provider.url, str(output), "a tree"):
            self.assertNotIn(secret, raw)
            self.assertNotIn(secret.encode(), message)

    def test_retained_artifact_detail_is_io_and_keeps_the_file(self):
        """A real finalization failure, produced by the filesystem itself.

        The fixture holds the response while this operation's running record
        already exists and the output is not yet committed, so replacing that
        record with a directory makes only the finalizing rename fail.
        """
        job = self.create(self.payload())
        self.provider.mode = "stall"
        self.provider.arrived.clear()
        status, error = [], c.c_void_p()

        def run():
            status.append(self.lib.tny_toolkit_job_run(job, c.byref(error)))

        worker = threading.Thread(target=run)
        worker.start()
        self.assertTrue(self.provider.arrived.wait(5))
        running = sorted(self.path.glob("out.png.tny-image-*.json"))
        self.assertEqual(len(running), 1, running)
        running[0].unlink()
        running[0].mkdir()
        self.provider.release.set()
        worker.join(15)
        self.assertFalse(worker.is_alive())
        # I/O, not cancellation and not a strict rejection.
        self.assertEqual(status, [-7])
        self.assertEqual(self.lib.tny_error_code(error), -7)
        message = self.lib.tny_error_message(error)
        text = c.string_at(message.ptr, message.len) if message.len else b""
        self.lib.tny_error_free(error)
        self.assertEqual(text, b"toolkit operation failed")
        detail = self.result_json(job)
        self.assertEqual(detail["code"], "IMAGE_MANIFEST_FINALIZE_FAILED")
        self.assertEqual(detail["kind"], "image")
        self.assertFalse(detail["ok"])
        self.assertTrue(detail["committed"])
        self.assertEqual(detail["path"], str(self.path / "out.png"))
        self.assertEqual(detail["bytes"], len(PNG))
        self.assertEqual((detail["width"], detail["height"]), (1, 1))
        self.assertEqual(detail["mime_type"], "image/png")
        self.assertEqual(detail["manifest_path"], str(running[0]))
        self.assertRegex(detail["operation_id"], r"^[0-9a-f]{16}$")
        # The paid artifact is kept exactly as the provider returned it.
        self.assertEqual((self.path / "out.png").read_bytes(), PNG)
        self.assertEqual(self.staging(), [])
        raw = json.dumps(detail)
        for secret in (TOKEN, ACCOUNT, self.provider.url, "a tree"):
            self.assertNotIn(secret, raw)
            self.assertNotIn(secret.encode(), text)

    def test_generic_and_cancelled_failures_have_no_detail(self):
        self.provider.mode = "error"  # a provider body quoting the fake token
        job = self.create(self.payload())
        message = self.run_job(job)
        self.assertEqual(self.lib.tny_toolkit_job_result(job).len, 0)
        self.assertNotIn(TOKEN.encode(), message)
        self.assertNotIn(b"private prompt", message)
        # A bad workspace and a pre-run cancellation are equally empty.
        payload = self.payload()
        payload["config"]["workspace"] = str(self.path / "does-not-exist")
        job = self.create(payload)
        self.assertEqual(
            self.run_job(job, -5), b"toolkit workspace or settings are unavailable"
        )
        self.assertEqual(self.lib.tny_toolkit_job_result(job).len, 0)
        payload = self.payload()
        payload["request"].update(size="2x2", strict_size=True)
        job = self.create(payload)
        self.assertEqual(self.lib.tny_toolkit_job_cancel(job), 0)
        self.assertEqual(self.run_job(job, -12), b"toolkit operation cancelled")
        self.assertEqual(self.lib.tny_toolkit_job_result(job).len, 0)

    def test_strict_detail_is_borrowed_until_destroy(self):
        payload = self.payload()
        payload["request"].update(size="2x2", strict_size=True)
        job = self.create(payload)
        self.run_job(job, -10)
        before = self.lib.tny_toolkit_job_result(job)
        self.assertTrue(before.len)
        copied = c.string_at(before.ptr, before.len)
        # Re-running is refused and must not disturb the borrowed buffer.
        self.assertEqual(self.lib.tny_toolkit_job_run(job, None), -2)
        after = self.lib.tny_toolkit_job_result(job)
        self.assertEqual(c.string_at(after.ptr, after.len), copied)
        self.assertEqual(self.lib.tny_toolkit_job_destroy(c.byref(job)), 0)
        self.assertFalse(job.value)
        # After destroy the accessor fails closed rather than reading freed
        # memory; the caller's own copy is all that remains.
        self.assertEqual(self.lib.tny_toolkit_job_result(job).len, 0)
        self.assertEqual(json.loads(copied)["code"], "IMAGE_SIZE_MISMATCH")

    def test_strict_detail_survives_or_disappears_under_allocator_faults(self):
        """Every exhaustion point either keeps a complete object or none.

        The test-only fault library injects one failure per allocation index
        of the `toolkit_run` scope. A partially formatted object must never be
        exposed, and memory pressure must never invent a success.
        """
        library = (
            ROOT
            / "build/lib-fault"
            / ("libtny.1.dylib" if sys.platform == "darwin" else "libtny.so.1")
        )
        if not library.exists():
            self.skipTest("fault library not built (make lib-shared-fault)")
        payload = self.payload()
        payload["request"].update(size="2x2", strict_size=True)
        clean = self.fault_child(library, payload, 0)
        self.assertEqual(clean["status"], -10)
        self.assertEqual(json.loads(clean["result"])["code"], "IMAGE_SIZE_MISMATCH")
        allocations = clean["allocations"]
        self.assertGreater(allocations, 0)
        injected = 0
        for index in range(1, allocations + 1):
            with self.subTest(fail_at=index):
                child = self.fault_child(library, payload, index)
                self.assertNotEqual(child["status"], 0)  # never a success
                if not child["injected"]:
                    continue
                injected += 1
                if child["result"]:
                    # Only a complete, locally coded object may survive, and
                    # only with the strict-failure status.
                    self.assertEqual(child["status"], -10)
                    detail = json.loads(child["result"])
                    self.assertEqual(detail["code"], "IMAGE_SIZE_MISMATCH")
                    self.assertFalse(detail["committed"])
                    self.assertIsNone(detail["path"])
                else:
                    self.assertIn(child["status"], (-4, -10))
                self.assertFalse((self.path / "out.png").exists())
                self.assertEqual(self.staging(), [])
        self.assertGreater(injected, 0)

    def fault_child(self, library, payload, fail_at):
        """One isolated run of the fault library, with at most one injection."""
        source = """
import ctypes as c, json, sys
class Bytes(c.Structure):
    _fields_ = [("ptr", c.c_void_p), ("len", c.c_uint64)]
lib = c.CDLL(sys.argv[1])
lib.tny_toolkit_job_create.argtypes = [Bytes, c.POINTER(c.c_void_p), c.c_void_p]
lib.tny_toolkit_job_create.restype = c.c_int32
lib.tny_toolkit_job_run.argtypes = [c.c_void_p, c.c_void_p]
lib.tny_toolkit_job_run.restype = c.c_int32
lib.tny_toolkit_job_result.argtypes = [c.c_void_p]
lib.tny_toolkit_job_result.restype = Bytes
lib.tny_toolkit_job_destroy.argtypes = [c.POINTER(c.c_void_p)]
lib.tny_toolkit_job_destroy.restype = c.c_int32
lib.tny_error_free.argtypes = [c.c_void_p]
lib.tny_error_free.restype = None
lib.tny_alloc_test_scope_count.restype = c.c_size_t
lib.tny_alloc_test_scope_injected.restype = c.c_bool
raw = sys.argv[2].encode()
buffer = c.create_string_buffer(raw)
job = c.c_void_p()
if lib.tny_toolkit_job_create(Bytes(c.addressof(buffer), len(raw)), c.byref(job), None):
    raise SystemExit("create failed")
# Ask for the diagnostic too: building it allocates after the failure object
# is complete, which is exactly where a late exhaustion must still win.
error = c.c_void_p()
status = lib.tny_toolkit_job_run(job, c.byref(error))
result = lib.tny_toolkit_job_result(job)
print(json.dumps({
    "status": status,
    "result": c.string_at(result.ptr, result.len).decode() if result.len else "",
    "allocations": lib.tny_alloc_test_scope_count(),
    "injected": lib.tny_alloc_test_scope_injected(),
}))
if error.value:
    lib.tny_error_free(error)
lib.tny_toolkit_job_destroy(c.byref(job))
"""
        environment = dict(os.environ)
        environment.pop("TNY_TEST_ALLOC_SCOPE", None)
        environment.pop("TNY_TEST_ALLOC_FAIL_AT", None)
        if fail_at:
            environment["TNY_TEST_ALLOC_SCOPE"] = "toolkit_run"
            environment["TNY_TEST_ALLOC_FAIL_AT"] = str(fail_at)
        child = subprocess.run(
            [sys.executable, "-c", source, str(library), json.dumps(payload)],
            capture_output=True,
            env=environment,
            timeout=60,
        )
        self.assertEqual(child.returncode, 0, child.stderr)
        return json.loads(child.stdout)

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
        self.assertEqual(self.staging(), [])
        for record in self.records():
            self.assertEqual(record["status"], "cancelled")
            self.assertFalse(record["committed"])

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
