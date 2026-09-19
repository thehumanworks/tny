#!/usr/bin/env python3
"""Focused C mailbox tests; NOT public CLI/provider integration for issue #156.

Compiles the actual service and existing host seams into a temporary shared
library, drives its C ABI with ctypes, and checks real private durable records.
No provider accounts, registration, Makefile or Nix changes are needed. Run:
    python3 tests/integration/test_team_mailbox.py
Lead must wire this command into integration/run.sh and Nix's source/test closure.
"""

from __future__ import annotations

import ctypes as c
import errno
import fcntl
import hashlib
import json
import os
import shlex
import signal
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

from test_jobs import argv_without_runner_binary

ROOT = Path(__file__).resolve().parents[2]
RUN = b"0123456789abcdef0123456789abcdef"
OK, INVALID, UNSUPPORTED, DENIED, STALE, TERMINAL, BUSY = range(7)
FULL, HISTORY_FULL, CONFLICT, NOT_FOUND, BAD_STATE, CORRUPT, IO = range(7, 14)
EMPTY, DEADLINE, CANCELLED = range(14, 17)
Cancel = c.CFUNCTYPE(c.c_bool, c.c_void_p)
QUEUED, DELIVERED, ACKED, RETIRED = range(4)
SECRET = b"private-fixture-capability-not-a-real-secret"


class Identity(c.Structure):
    _fields_ = [
        ("run", c.c_char * 33),
        ("job_attempt", c.c_uint32),
        ("task", c.c_int),
        ("task_attempt", c.c_uint32),
    ]


class Recipient(c.Structure):
    _fields_ = [("task", c.c_int), ("task_attempt", c.c_uint32)]


class Message(c.Structure):
    _fields_ = [
        ("id", c.c_char * 65),
        ("sequence", c.c_uint64),
        ("sender", Identity),
        ("recipient", Recipient),
        ("state", c.c_int),
        ("payload_len", c.c_size_t),
        ("payload", c.c_char * 16385),
        ("publication", c.c_char * 65),
    ]


Authorize = c.CFUNCTYPE(
    c.c_bool,
    c.c_void_p,
    c.POINTER(Identity),
    c.c_void_p,
    c.c_size_t,
    c.POINTER(c.c_bool),
)


class Service(c.Structure):
    _fields_ = [
        ("job_dir", c.c_char_p),
        ("native_local", c.c_bool),
        ("authorize", Authorize),
        ("userdata", c.c_void_p),
    ]


def identity(task=-1, attempt=1, task_attempt=None):
    return Identity(
        RUN,
        attempt,
        task,
        (0 if task == -1 else attempt) if task_attempt is None else task_attempt,
    )


def load_library(path):
    lib = c.CDLL(str(path))
    base = [c.POINTER(Service), c.POINTER(Identity)]
    lib.tny_team_mailbox_send.argtypes = base + [
        Recipient,
        c.c_char_p,
        c.c_char_p,
        c.c_size_t,
        c.POINTER(Message),
    ]
    lib.tny_team_mailbox_publish.argtypes = base + [
        c.c_char_p,
        c.c_char_p,
        c.c_size_t,
        c.POINTER(Message),
        c.POINTER(c.c_size_t),
    ]
    lib.tny_team_mailbox_wait.argtypes = base + [
        c.c_int,
        Cancel,
        c.c_void_p,
        c.POINTER(Message),
        c.POINTER(c.c_size_t),
    ]
    lib.tny_team_mailbox_inbox.argtypes = base + [
        c.c_uint64,
        c.POINTER(Message),
        c.c_size_t,
        c.c_size_t,
        c.POINTER(c.c_size_t),
    ]
    lib.tny_team_mailbox_read.argtypes = base + [c.c_char_p, c.POINTER(Message)]
    lib.tny_team_mailbox_ack.argtypes = base + [c.c_char_p]
    lib.tny_team_mailbox_mark_delivered.argtypes = base + [c.c_char_p]
    lib.tny_team_mailbox_retire.argtypes = base + [
        c.c_int,
        c.c_uint32,
        c.POINTER(c.c_size_t),
    ]
    lib.tny_team_mailbox_error.argtypes = [c.c_int]
    lib.tny_team_mailbox_error.restype = c.c_char_p
    return lib


class MailboxTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="tny-mailbox-build-")
        cls.library_path = Path(cls.build.name) / "mailbox.so"
        sources = [
            "src/core/team_mailbox.c",
            "src/util/jobs_host.c",
            "src/util/image_io.c",
            "src/util/util.c",
            "src/util/alloc.c",
            "src/util/process.c",
            "src/util/process_scope.c",
            "src/util/tny_poll.c",
            "src/json/json.c",
            "third_party/yyjson/yyjson.c",
        ]
        flags = [
            "-shared",
            "-fPIC",
            "-std=c11",
            "-D_DARWIN_C_SOURCE",
            "-D_DEFAULT_SOURCE",
            "-D_POSIX_C_SOURCE=200809L",
            "-DYYJSON_DISABLE_NON_STANDARD",
            "-Isrc",
            "-Ithird_party/yyjson",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-deprecated-declarations",
        ]
        subprocess.run(
            shlex.split(os.environ.get("CC", "cc"))
            + flags
            + shlex.split(os.environ.get("MAILBOX_TEST_CFLAGS", ""))
            + sources
            + ["-o", str(cls.library_path)],
            cwd=ROOT,
            check=True,
            timeout=120,
        )
        cls.lib = load_library(cls.library_path)
        cls.fault_path = Path(cls.build.name) / "mailbox-fault.so"
        fault_sources = [
            "tests/fixtures/team_mailbox_faults.c"
            if path == "src/util/jobs_host.c"
            else path
            for path in sources
        ]
        subprocess.run(
            shlex.split(os.environ.get("CC", "cc"))
            + flags
            + shlex.split(os.environ.get("MAILBOX_TEST_CFLAGS", ""))
            + fault_sources
            + ["-o", str(cls.fault_path)],
            cwd=ROOT,
            check=True,
            timeout=120,
        )
        cls.fault_lib = load_library(cls.fault_path)
        cls.fault_lib.tny_mailbox_fault_reset.argtypes = [c.c_char]
        cls.fault_lib.tny_mailbox_fault_reset.restype = None
        cls.fault_lib.tny_mailbox_fault_trace.restype = c.c_char_p
        for name in ("write_private", "write_once", "snapshot"):
            getattr(cls.fault_lib, "tny_jobs_host_" + name).argtypes = [
                c.c_char_p,
                c.c_char_p,
                c.c_size_t,
            ]
        cls.fault_lib.tny_jobs_host_sync_parent.argtypes = [c.c_char_p]

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.fault()
        self.temp = tempfile.TemporaryDirectory(prefix="tny-mailbox-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name).resolve()
        self.secret = c.create_string_buffer(SECRET)
        verifier = hashlib.sha256(SECRET).hexdigest()
        self.job = {
            "version": 1,
            "kind": "job",
            "id": RUN.decode(),
            "attempt": 1,
            "state": "running",
            "cancel_requested": False,
            "items": [
                {
                    "index": i,
                    "attempt": 1,
                    "state": "running",
                    "cancel_requested": False,
                }
                for i in range(5)
            ],
            # Test-only schema: production capability format is lead-owned.
            "fixture_authority": {str(i): verifier for i in range(-1, 5)},
            "fixture_peers": False,
        }
        self.write_job()

        @Authorize
        def authorize(userdata, caller, data, length, peers):
            record = json.loads(c.string_at(data, length))
            peers[0] = record.get("fixture_peers", False) is True
            private_secret = c.string_at(userdata) if userdata else b""
            return (
                record.get("fixture_authority", {}).get(str(caller.contents.task))
                == hashlib.sha256(private_secret).hexdigest()
            )

        self.authorize = authorize  # retain callback for the lifetime of all calls
        self.service = Service(
            os.fsencode(self.directory),
            True,
            authorize,
            c.cast(self.secret, c.c_void_p),
        )

    def publish(self, name=b"proposal", text=b"evidence", sender=None):
        messages, count = (Message * 64)(), c.c_size_t()
        rc = self.lib.tny_team_mailbox_publish(
            c.byref(self.service),
            c.byref(sender or identity()),
            name,
            text,
            len(text),
            messages,
            c.byref(count),
        )
        return rc, list(messages[: count.value])

    def wait_mail(self, caller=None, timeout=100, cancel=None):
        messages, count = (Message * 16)(), c.c_size_t()
        callback = Cancel(cancel or (lambda _: False))
        rc = self.lib.tny_team_mailbox_wait(
            c.byref(self.service),
            c.byref(caller or identity(0)),
            timeout,
            callback,
            None,
            messages,
            c.byref(count),
        )
        return rc, list(messages[: count.value])

    def test_collective_publication_is_atomic_and_recipient_replayable(self):
        self.job["fixture_peers"] = True
        self.write_job()
        rc, receipts = self.publish(sender=identity(0))
        self.assertEqual(rc, OK)
        self.assertEqual([m.recipient.task for m in receipts], [-1, 1, 2, 3, 4])
        self.assertEqual(len({m.id for m in receipts}), 5)
        self.assertTrue(all(m.publication == b"proposal" for m in receipts))
        self.assertEqual(len(self.record()["messages"]), 5)
        self.assertEqual(
            self.publish(text=b"changed!", sender=identity(0))[0], CONFLICT
        )
        self.assertEqual(self.publish(sender=identity(1))[0], CONFLICT)
        self.assertEqual(self.send(b"proposal")[0], CONFLICT)
        self.assertEqual(
            self.send(receipts[0].id, recipient=-1, recipient_attempt=0)[0], CONFLICT
        )
        # Recipient ack does not consume any other member's receipt.
        member = identity(1)
        self.assertEqual(
            self.lib.tny_team_mailbox_mark_delivered(
                c.byref(self.service), c.byref(member), receipts[1].id
            ),
            OK,
        )
        self.assertEqual(
            self.lib.tny_team_mailbox_ack(
                c.byref(self.service), c.byref(member), receipts[1].id
            ),
            OK,
        )
        self.job["state"] = "succeeded"
        for item in self.job["items"]:
            item["state"] = "succeeded"
        self.write_job()
        retry_rc, retry = self.publish(sender=identity(0))
        self.assertEqual(retry_rc, OK)
        self.assertEqual([m.id for m in retry], [m.id for m in receipts])
        self.assertEqual(retry[1].state, ACKED)
        self.assertEqual(self.publish(b"new", sender=identity(0))[0], TERMINAL)
        self.assertEqual(len(self.record()["messages"]), 5)

    def test_collective_full_recipient_aborts_every_receipt(self):
        self.job["fixture_peers"] = True
        self.write_job()
        for i in range(64):
            self.assertEqual(self.send(f"full{i}".encode(), recipient=3)[0], OK)
        before = (self.directory / "mailbox.json").read_bytes()
        self.assertEqual(self.publish()[0], FULL)
        self.assertEqual((self.directory / "mailbox.json").read_bytes(), before)
        self.assertEqual(self.inbox(identity(0))[1], [])

    def test_collective_derived_id_collision_and_peers_permission(self):
        self.assertEqual(self.publish()[0], DENIED)
        self.job["fixture_peers"] = True
        self.write_job()
        self.assertEqual(self.send(b"proposal.p1")[0], OK)
        self.assertEqual(self.publish()[0], CONFLICT)
        self.assertEqual(len(self.record()["messages"]), 1)

    def test_collective_uncertain_publication_reconciles_original_set(self):
        self.lib = self.fault_lib
        self.job["fixture_peers"] = True
        self.write_job()
        self.fault(b"S")
        self.assertEqual(self.publish()[0], IO)
        original = self.record()["messages"]
        self.assertEqual(len(original), 5)
        self.job["items"][2]["state"] = "succeeded"
        self.write_job()
        self.fault()
        rc, receipts = self.publish()
        self.assertEqual(rc, OK)
        self.assertEqual([m.id.decode() for m in receipts], [m["id"] for m in original])
        self.assertEqual(len(self.record()["messages"]), 5)

    def test_event_wait_wakes_for_terminal_and_directory_loss(self):
        for deleted in (False, True):
            with self.subTest(deleted=deleted):
                self.job["state"] = "running"
                self.write_job()
                # Precreate lock so it is not the readiness event under test.
                self.assertEqual(self.wait_mail(timeout=0)[0], EMPTY)
                result = []
                thread = threading.Thread(
                    target=lambda: result.append(self.wait_mail(timeout=2000))
                )
                thread.start()
                time.sleep(0.04)
                if deleted:
                    moved = self.directory.with_name(self.directory.name + "-moved")
                    self.directory.rename(moved)
                else:
                    self.job["state"] = "succeeded"
                    self.write_job()
                thread.join(3)
                if deleted:
                    moved.rename(self.directory)
                self.assertFalse(thread.is_alive())
                self.assertEqual(result[0][0], IO if deleted else TERMINAL)

    def test_event_wait_queued_timeout_cancel_and_terminal(self):
        self.assertEqual(self.wait_mail(timeout=0)[0], EMPTY)
        start = time.monotonic()
        self.assertEqual(self.wait_mail(timeout=80)[0], DEADLINE)
        self.assertGreaterEqual(time.monotonic() - start, 0.06)
        self.assertEqual(self.wait_mail(cancel=lambda _: True)[0], CANCELLED)
        self.assertEqual(self.send()[0], OK)
        self.assertEqual(self.wait_mail()[1][0].id, b"m1")
        self.assertEqual(self.wait_mail()[1][0].id, b"m1")
        self.assertEqual(self.wait_mail(identity(1, attempt=2))[0], STALE)
        self.job["items"][1]["state"] = "succeeded"
        self.write_job()
        self.assertEqual(self.wait_mail(identity(1))[0], TERMINAL)

    def test_event_wait_quiet_has_one_snapshot_and_no_periodic_rescans(self):
        self.assertEqual(self.wait_mail(timeout=0)[0], EMPTY)
        reads = []
        original = self.authorize

        @Authorize
        def counted(*args):
            reads.append(1)
            return original(*args)

        self.service.authorize = counted
        self.assertEqual(self.wait_mail(timeout=220)[0], DEADLINE)
        self.assertEqual(len(reads), 1)

    def test_event_wait_subscribe_race_and_atomic_replacement(self):
        entered = threading.Event()
        original = self.authorize

        @Authorize
        def counted(*args):
            entered.set()
            return original(*args)

        self.service.authorize = counted
        result = []
        thread = threading.Thread(
            target=lambda: result.append(self.wait_mail(timeout=2000))
        )
        thread.start()
        self.assertTrue(entered.wait(1))
        # Sender races the first snapshot/lock release; retry only BUSY.
        end = time.monotonic() + 1
        while True:
            rc, _ = self.send()
            if rc != BUSY or time.monotonic() > end:
                break
            time.sleep(0.001)
        thread.join(3)
        self.assertFalse(thread.is_alive())
        self.assertEqual(rc, OK)
        self.assertEqual(result[0][0], OK)
        self.assertEqual(result[0][1][0].id, b"m1")

    def write_job(self):
        temp = self.directory / "job.next"
        temp.write_text(json.dumps(self.job))
        temp.replace(self.directory / "job.json")

    def record(self):
        return json.loads((self.directory / "mailbox.json").read_text())

    def send(
        self,
        msg_id=b"m1",
        payload=b"hello",
        sender=None,
        recipient=0,
        recipient_attempt=1,
    ):
        out = Message()
        rc = self.lib.tny_team_mailbox_send(
            c.byref(self.service),
            c.byref(sender or identity()),
            Recipient(recipient, recipient_attempt),
            msg_id,
            payload,
            len(payload),
            c.byref(out),
        )
        return rc, out

    def inbox(self, caller=None, after=0, capacity=16, byte_limit=65536):
        out = (Message * 16)()
        count = c.c_size_t(99)
        rc = self.lib.tny_team_mailbox_inbox(
            c.byref(self.service),
            c.byref(caller or identity(0)),
            after,
            out,
            capacity,
            byte_limit,
            c.byref(count),
        )
        return rc, list(out[: count.value])

    def access(self, operation, msg_id=b"m1", caller=None):
        args = [c.byref(self.service), c.byref(caller or identity(0)), msg_id]
        out = Message()
        if operation == "read":
            args.append(c.byref(out))
        return getattr(self.lib, "tny_team_mailbox_" + operation)(*args), out

    def retire(self, recipient=0, before=2, caller=None):
        retired = c.c_size_t(99)
        rc = self.lib.tny_team_mailbox_retire(
            c.byref(self.service),
            c.byref(caller or identity(attempt=2)),
            recipient,
            before,
            c.byref(retired),
        )
        return rc, retired.value

    def retry_job(self, attempt=2):
        self.job["attempt"] = attempt
        for item in self.job["items"]:
            item["attempt"] = attempt
        self.write_job()

    def test_retirement_releases_retry_quota_but_retains_id_tombstones(self):
        for i in range(64):
            self.assertEqual(self.send(f"old{i}".encode())[0], OK)
        self.assertEqual(self.access("mark_delivered", b"old0")[0], OK)
        old = self.record()["messages"]
        self.retry_job()
        self.assertEqual(
            self.send(b"new", sender=identity(attempt=2), recipient_attempt=2)[0], FULL
        )
        self.assertEqual(self.access("ack", b"old0")[0], STALE)
        self.assertEqual(self.access("ack", b"old0", identity(0, 2))[0], DENIED)
        self.assertEqual(self.retire(), (OK, 64))
        # Reopen and repeat: retirement is durable and idempotent.
        self.lib = load_library(self.library_path)
        self.assertEqual(self.retire(), (OK, 0))
        self.assertEqual(self.inbox(identity(0, 2)), (OK, []))
        records = self.record()["messages"]
        self.assertEqual(records, [dict(m, state=RETIRED) for m in old])
        self.assertEqual(
            self.send(b"new", sender=identity(attempt=2), recipient_attempt=2)[0], OK
        )
        self.assertEqual(self.send(b"old0")[0], STALE)
        self.assertEqual(
            self.send(b"old0", sender=identity(attempt=2), recipient_attempt=2)[0],
            CONFLICT,
        )
        # Current-attempt duplicate receipts remain idempotent.
        rc, receipt = self.send(b"new", sender=identity(attempt=2), recipient_attempt=2)
        self.assertEqual((rc, receipt.sequence, receipt.state), (OK, 65, QUEUED))
        self.assertEqual([m.id for m in self.inbox(identity(0, 2))[1]], [b"new"])

    def test_retirement_authority_and_strict_attempt_boundary(self):
        self.assertEqual(self.send()[0], OK)
        # Indexed role metadata never grants lead routing authority.
        self.job["items"][0]["role"] = "lead"
        self.retry_job()
        self.assertEqual(
            self.send(b"current", sender=identity(attempt=2), recipient_attempt=2)[0],
            OK,
        )
        before = (self.directory / "mailbox.json").read_bytes()
        for caller, task, cutoff, expected in (
            (identity(0, 2), 0, 2, DENIED),
            (identity(1, 2), 0, 2, DENIED),
            (identity(), 0, 2, STALE),
            (identity(attempt=2), 6, 2, DENIED),
            (identity(attempt=2), -2, 2, INVALID),
            (identity(attempt=2), 0, 0, INVALID),
            (identity(attempt=2), 0, 3, INVALID),
        ):
            self.assertEqual(self.retire(task, cutoff, caller), (expected, 0))
            self.assertEqual((self.directory / "mailbox.json").read_bytes(), before)
        self.service.userdata = None
        self.assertEqual(self.retire(), (DENIED, 0))
        self.service.userdata = c.cast(self.secret, c.c_void_p)
        self.assertEqual(self.retire(before=1), (OK, 0))
        self.assertEqual(self.retire(before=2), (OK, 1))
        self.assertEqual(
            [m["state"] for m in self.record()["messages"]], [RETIRED, QUEUED]
        )
        self.assertEqual(self.retire(), (OK, 0))
        self.assertEqual(
            self.access("read", b"current", identity(0, 2))[1].state, QUEUED
        )

    def test_retirement_keeps_acked_receipts_and_other_recipients(self):
        self.assertEqual(self.send()[0], OK)
        self.assertEqual(self.access("mark_delivered")[0], OK)
        self.assertEqual(self.access("ack")[0], OK)
        self.assertEqual(self.send(b"other", recipient=1)[0], OK)
        self.assertEqual(
            self.send(
                b"to-lead", sender=identity(0), recipient=-1, recipient_attempt=0
            )[0],
            OK,
        )
        acked = self.record()["messages"][0]
        self.retry_job()
        self.assertEqual(self.retire(), (OK, 0))
        self.assertEqual(self.retire(recipient=-1), (OK, 1))
        self.assertEqual(self.record()["messages"][0], acked)
        self.assertEqual(self.record()["messages"][1]["state"], QUEUED)
        self.assertEqual(self.record()["messages"][2]["state"], RETIRED)

    def test_retirement_partial_cutoff_and_corrupt_current_tombstone(self):
        self.assertEqual(self.send(b"attempt1")[0], OK)
        self.retry_job()
        self.assertEqual(
            self.send(b"attempt2", sender=identity(attempt=2), recipient_attempt=2)[0],
            OK,
        )
        self.retry_job(3)
        self.assertEqual(self.retire(before=2, caller=identity(attempt=3)), (OK, 1))
        self.assertEqual(
            [m["state"] for m in self.record()["messages"]], [RETIRED, QUEUED]
        )
        self.assertEqual(self.retire(before=3, caller=identity(attempt=3)), (OK, 1))
        self.assertEqual(
            self.send(b"current", sender=identity(attempt=3), recipient_attempt=3)[0],
            OK,
        )
        corrupted = self.record()
        corrupted["messages"][-1]["state"] = RETIRED
        (self.directory / "mailbox.json").write_text(json.dumps(corrupted))
        self.assertEqual(
            self.retire(before=3, caller=identity(attempt=3)), (CORRUPT, 0)
        )
        self.assertEqual(self.inbox(identity(0, 3)), (CORRUPT, []))

    def test_retirement_crash_retry_contention_and_unsupported(self):
        self.assertEqual(self.send()[0], OK)
        self.retry_job()
        with (self.directory / "state.lock").open("wb") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.assertEqual(self.retire(), (BUSY, 0))
        self.service.native_local = False
        self.assertEqual(self.retire(), (UNSUPPORTED, 0))
        self.assertEqual(self.record()["messages"][0]["state"], QUEUED)
        self.service.native_local = True
        self.fork_and_kill_after(lambda: self.retire()[0])
        self.assertEqual(self.retire(), (OK, 0))
        self.assertEqual(self.record()["messages"][0]["state"], RETIRED)

    def fault(self, stage=b"\0"):
        self.fault_lib.tny_mailbox_fault_reset(stage)

    def fault_trace(self):
        return self.fault_lib.tny_mailbox_fault_trace()

    def test_host_publication_syscall_order_and_faults(self):
        target = self.directory / "host-record"
        for operation, publication, success in (
            ("write_private", b"R", b"OWFCRDSX"),
            ("write_once", b"L", b"OWFCLUDSX"),
        ):
            for stage in (b"O", b"W", b"F", b"C", publication, b"D", b"S", b"X", b"\0"):
                with self.subTest(operation=operation, fault=stage):
                    target.unlink(missing_ok=True)
                    if operation == "write_private":
                        target.write_bytes(b"original")
                    self.fault(stage)
                    rc = getattr(self.fault_lib, "tny_jobs_host_" + operation)(
                        os.fsencode(target),
                        b"replacement",
                        11,
                    )
                    self.assertEqual(rc, 0 if stage == b"\0" else errno.EIO)
                    post_publish = stage in (b"D", b"S", b"X", b"\0")
                    if operation == "write_private" or post_publish:
                        self.assertEqual(
                            target.read_bytes(),
                            b"replacement" if post_publish else b"original",
                        )
                    else:
                        self.assertFalse(target.exists())
                    trace = self.fault_trace()
                    if stage == b"\0":
                        self.assertEqual(trace, success)
                    elif stage == b"F":
                        self.assertEqual(trace, b"OWFCU")
                    elif stage in (b"D", b"S", b"X"):
                        self.assertEqual(
                            trace,
                            success[: success.index(stage) + 1]
                            + (b"X" if stage == b"S" else b""),
                        )
                    self.assertFalse(list(self.directory.glob("host-record.tmp-*")))

    def test_host_parent_path_handling(self):
        # Change only the child's cwd. Fork retains the loaded sanitizer runtime
        # (macOS strips DYLD_INSERT_LIBRARIES on a fresh Python exec).
        def relative_write():
            os.chdir(self.directory)
            return self.fault_lib.tny_jobs_host_write_private(
                b"relative-record", b"data", 4
            )

        self.fork_and_kill_after(relative_write)
        self.assertEqual((self.directory / "relative-record").read_bytes(), b"data")
        for bad in (None, b"", os.fsencode(self.directory) + b"/"):
            self.assertEqual(
                self.fault_lib.tny_jobs_host_sync_parent(bad), errno.EINVAL
            )
        real_parent = self.directory / "real"
        real_parent.mkdir()
        alias = self.directory / "alias"
        alias.symlink_to(real_parent, target_is_directory=True)
        self.assertNotEqual(
            self.fault_lib.tny_jobs_host_sync_parent(os.fsencode(alias / "record")), 0
        )

    def test_host_write_once_cleanup_and_snapshot_recovery(self):
        target = self.directory / "snapshot"
        name = os.fsencode(target)
        self.fault(b"U")
        self.assertEqual(
            self.fault_lib.tny_jobs_host_write_once(name, b"data", 4), errno.EIO
        )
        self.assertEqual(target.read_bytes(), b"data")
        self.assertEqual(self.fault_trace(), b"OWFCLU")
        for temporary in self.directory.glob("snapshot.tmp-*"):
            temporary.unlink()
        # Matching existing bytes must still sync the parent before success.
        self.fault(b"S")
        self.assertEqual(
            self.fault_lib.tny_jobs_host_snapshot(name, b"data", 4), errno.EIO
        )
        self.assertEqual(self.fault_trace(), b"OWFCLUDSX")
        self.fault()
        self.assertEqual(self.fault_lib.tny_jobs_host_snapshot(name, b"data", 4), 0)
        self.assertEqual(self.fault_trace(), b"OWFCLUDSX")
        self.assertEqual(target.read_bytes(), b"data")
        self.fault()
        self.assertEqual(
            self.fault_lib.tny_jobs_host_snapshot(name, b"else", 4), errno.EINVAL
        )
        self.assertNotIn(b"S", self.fault_trace())
        self.fault()
        self.assertEqual(
            self.fault_lib.tny_jobs_host_write_once(name, b"else", 4), errno.EEXIST
        )
        self.assertEqual(target.read_bytes(), b"data")

    def test_post_rename_failure_and_idempotent_send_delivery_ack_retry(self):
        self.lib = self.fault_lib
        self.fault(b"S")
        self.assertEqual(self.send()[0], IO)
        self.assertEqual(self.record()["messages"][0]["state"], QUEUED)
        self.assertEqual(self.fault_trace(), b"OWFCRDSX")
        self.fault(b"S")
        self.assertEqual(self.send()[0], IO)  # duplicate cannot skip required sync
        self.assertEqual(self.fault_trace(), b"DSX")
        self.fault()
        self.assertEqual(self.send()[0], OK)
        self.assertEqual(self.fault_trace(), b"DSX")
        self.assertEqual(len(self.record()["messages"]), 1)
        for operation, state in (("mark_delivered", DELIVERED), ("ack", ACKED)):
            self.fault(b"S")
            self.assertEqual(self.access(operation)[0], IO)
            self.assertEqual(self.record()["messages"][0]["state"], state)
            self.assertEqual(self.fault_trace(), b"OWFCRDSX")
            self.fault(b"S")
            self.assertEqual(self.access(operation)[0], IO)
            self.assertEqual(self.fault_trace(), b"DSX")
            self.fault()
            self.assertEqual(self.access(operation)[0], OK)
            self.assertEqual(self.fault_trace(), b"DSX")

    def test_retirement_failed_sync_is_uncertain_and_retry_resyncs(self):
        self.assertEqual(self.send()[0], OK)
        self.retry_job()
        self.lib = self.fault_lib
        self.fault(b"S")
        self.assertEqual(self.retire(), (IO, 0))
        self.assertEqual(self.record()["messages"][0]["state"], RETIRED)
        self.assertEqual(self.fault_trace(), b"OWFCRDSX")
        self.fault(b"S")
        self.assertEqual(self.retire(), (IO, 0))
        self.assertEqual(self.fault_trace(), b"DSX")
        self.fault()
        self.assertEqual(self.retire(), (OK, 0))
        self.assertEqual(self.fault_trace(), b"DSX")
        self.assertEqual(
            self.send(b"new", sender=identity(attempt=2), recipient_attempt=2)[0], OK
        )

    def test_order_addressing_state_and_explicit_ack(self):
        for i in range(4):
            rc, receipt = self.send(f"m{i}".encode(), recipient=i % 2)
            self.assertEqual((rc, receipt.sequence, receipt.state), (OK, i + 1, QUEUED))
        self.assertEqual([m.id for m in self.inbox()[1]], [b"m0", b"m2"])
        self.assertEqual([m.id for m in self.inbox(after=1)[1]], [b"m2"])
        self.assertEqual(self.access("read", b"m1")[0], DENIED)
        self.assertEqual(self.access("ack", b"m0")[0], BAD_STATE)
        self.assertEqual(self.access("mark_delivered", b"m0")[0], OK)
        self.assertEqual(self.inbox()[1][0].state, DELIVERED)
        self.assertEqual(self.access("ack", b"m0")[0], OK)
        self.assertEqual(self.access("ack", b"m0")[0], OK)
        self.assertEqual(self.access("mark_delivered", b"m0")[0], OK)
        self.assertEqual(self.access("read", b"m0")[1].state, ACKED)
        self.assertEqual([m.id for m in self.inbox()[1]], [b"m2"])
        self.assertEqual(self.record()["messages"][0]["state"], ACKED)
        self.assertEqual(
            (self.directory / "mailbox.json").stat().st_mode & 0o777, 0o600
        )
        self.assertEqual((self.directory / "state.lock").stat().st_mode & 0o777, 0o600)
        self.assertEqual(
            self.send(b"reply", sender=identity(0), recipient=-1, recipient_attempt=0)[
                0
            ],
            OK,
        )
        self.assertEqual(self.inbox(identity())[1][0].id, b"reply")

    def test_duplicates_and_conflicts_survive_reopen(self):
        self.assertEqual(self.send()[0], OK)
        before = (self.directory / "mailbox.json").read_bytes()
        lib = self.lib
        self.lib = load_library(self.library_path)
        self.addCleanup(setattr, self, "lib", lib)
        rc, receipt = self.send()
        self.assertEqual((rc, receipt.sequence), (OK, 1))
        self.assertEqual((self.directory / "mailbox.json").read_bytes(), before)
        self.assertEqual(self.send(payload=b"changed")[0], CONFLICT)
        self.assertEqual(self.send(payload=b"world")[0], CONFLICT)
        self.assertEqual(self.send(recipient=1)[0], CONFLICT)
        self.job["fixture_peers"] = True
        self.write_job()
        self.assertEqual(self.send(sender=identity(1))[0], CONFLICT)
        self.assertEqual(len(self.record()["messages"]), 1)

    def test_membership_capability_attempts_and_peer_opt_in(self):
        self.service.userdata = None
        self.assertEqual(self.send()[0], DENIED)
        self.assertFalse((self.directory / "mailbox.json").exists())
        self.service.userdata = c.cast(self.secret, c.c_void_p)
        self.assertEqual(self.send(sender=identity(6))[0], DENIED)
        self.assertEqual(self.send(recipient=6)[0], DENIED)
        self.assertEqual(self.send(sender=identity(0, task_attempt=2))[0], STALE)
        self.assertEqual(self.send(recipient_attempt=2)[0], STALE)
        self.assertEqual(self.send(sender=identity(attempt=2))[0], STALE)
        self.assertEqual(self.send(sender=identity(1))[0], DENIED)
        self.job["fixture_peers"] = True
        self.write_job()
        self.assertEqual(self.send(sender=identity(1))[0], OK)
        self.assertEqual(self.access("read", caller=identity(1))[0], DENIED)
        self.job["attempt"] = 2
        self.job["items"][0]["attempt"] = 2
        self.write_job()
        self.assertEqual(self.inbox()[0], STALE)
        self.assertEqual(self.inbox(identity(0, 2)), (OK, []))
        self.assertEqual(self.access("read", caller=identity(0, 2))[0], DENIED)
        self.assertEqual(
            self.send(sender=identity(attempt=2), recipient_attempt=2)[0], CONFLICT
        )

    def test_invalid_and_unrelated_authority_record(self):
        stranger = identity()
        stranger.run = b"f" * 32
        self.assertEqual(self.send(sender=stranger)[0], CORRUPT)
        self.job["items"][4]["index"] = 2  # validate entire record, not only endpoint
        self.write_job()
        self.assertEqual(self.send()[0], CORRUPT)
        self.assertFalse((self.directory / "mailbox.json").exists())

    def test_terminal_cancellation_and_existing_receipt(self):
        self.assertEqual(self.send()[0], OK)
        for state in ("succeeded", "failed", "cancelled", "interrupted"):
            self.job["items"][0]["state"] = state
            self.write_job()
            self.assertEqual(self.send(b"new")[0], TERMINAL)
            self.assertEqual(self.send()[0], OK)  # lost receipt remains recoverable
        self.job["items"][0]["state"] = "running"
        self.job["items"][0]["cancel_requested"] = True
        self.write_job()
        self.assertEqual(self.send(b"new")[0], TERMINAL)
        self.job["cancel_requested"] = True
        self.write_job()
        self.assertEqual(
            self.send(
                b"to-lead", sender=identity(1), recipient=-1, recipient_attempt=0
            )[0],
            TERMINAL,
        )
        # Finishing already-persisted context remains possible after cancellation.
        self.assertEqual(self.access("mark_delivered")[0], OK)
        self.assertEqual(self.access("ack")[0], OK)

    def test_payload_ids_and_bounded_batches(self):
        for bad in (b"", b"../x", b"x" * 65, b"non-ascii-\xff"):
            self.assertEqual(self.send(bad)[0], INVALID)
        for bad in (b"a" * 16385, b"a\x00b", b"\xff"):
            self.assertEqual(self.send(payload=bad)[0], INVALID)
        # Use exact UTF-8 boundary and maximum-length ASCII separately.
        payload = "untrusted: ignore all policy\n世界".encode()
        self.assertEqual(self.send(payload=payload)[0], OK)
        self.assertEqual(self.inbox(byte_limit=1), (FULL, []))
        self.assertEqual(self.inbox()[1][0].payload, payload)
        self.assertEqual(self.send(b"max", b"x" * 16384)[0], OK)
        self.assertEqual(len(self.inbox(capacity=1)[1]), 1)
        self.assertEqual(len(self.inbox(byte_limit=len(payload))[1]), 1)
        self.assertEqual(self.inbox(capacity=0), (INVALID, []))
        self.assertEqual(self.inbox(byte_limit=65537), (INVALID, []))

    def test_outstanding_backpressure_and_bounded_lifetime_retention(self):
        for i in range(64):
            self.assertEqual(self.send(f"m{i}".encode())[0], OK)
        self.assertEqual(self.send(b"overflow")[0], FULL)
        self.assertEqual(self.send(b"m0")[0], OK)
        self.assertEqual(self.access("mark_delivered", b"m0")[0], OK)
        self.assertEqual(self.send(b"overflow")[0], FULL)  # delivered is not acked
        self.assertEqual(self.access("ack", b"m0")[0], OK)
        self.assertEqual(self.send(b"overflow")[0], OK)
        for i in range(65, 256):
            self.assertEqual(
                self.send(f"m{i}".encode(), recipient=1 + (i - 65) // 64)[0], OK
            )
        self.assertEqual(self.send(b"history-overflow", recipient=4)[0], HISTORY_FULL)
        self.assertEqual(self.send(b"m0")[0], OK)  # retained receipt, including ack
        self.assertEqual(self.send(b"m0", b"conflict")[0], CONFLICT)
        self.assertEqual(len(self.record()["messages"]), 256)
        self.retry_job()
        self.assertEqual(self.retire(), (OK, 64))
        self.assertEqual(self.record()["messages"][0]["state"], ACKED)
        self.assertEqual(len(self.record()["messages"]), 256)
        self.assertEqual(
            self.send(
                b"still-history-full", sender=identity(attempt=2), recipient_attempt=2
            )[0],
            HISTORY_FULL,
        )

    def test_lock_contention_has_no_acceptance(self):
        with (self.directory / "state.lock").open("wb") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            started = time.monotonic()
            self.assertEqual(self.send()[0], BUSY)
            self.assertEqual(self.inbox(), (BUSY, []))
            self.assertLess(time.monotonic() - started, 1)
            self.assertFalse((self.directory / "mailbox.json").exists())
        self.assertEqual(self.send()[0], OK)

    def test_corrupt_records_and_symlinks_fail_closed(self):
        self.assertEqual(self.send()[0], OK)
        path = self.directory / "mailbox.json"
        original = path.read_bytes()
        for change in (
            "duplicate",
            "sequence",
            "state",
            "embedded-nul",
            "run",
            "truncated",
        ):
            record = json.loads(original)
            if change == "duplicate":
                record["messages"].append(dict(record["messages"][0], sequence=2))
            elif change == "run":
                record["run"] = "f" * 32
            elif change == "truncated":
                path.write_text("{")
            elif change == "embedded-nul":
                record["messages"][0]["payload"] = "a\0b"
            else:
                record["messages"][0][change] = 99
            if change != "truncated":
                path.write_text(json.dumps(record))
            before = path.read_bytes()
            self.assertEqual(self.send(b"new")[0], CORRUPT, change)
            self.assertEqual(path.read_bytes(), before)
        path.unlink()
        target = self.directory / "outside"
        target.write_bytes(original)
        path.symlink_to(target)
        self.assertEqual(self.send(b"new")[0], IO)
        self.assertEqual(target.read_bytes(), original)
        path.unlink()
        path.mkdir()
        self.assertEqual(self.send(b"new")[0], IO)

    def test_unsupported_before_side_effects_and_missing_authorizer(self):
        self.service.native_local = False
        self.assertEqual(self.send()[0], UNSUPPORTED)
        self.assertEqual(self.inbox(), (UNSUPPORTED, []))
        self.assertEqual(self.access("mark_delivered")[0], UNSUPPORTED)
        self.assertEqual(sorted(p.name for p in self.directory.iterdir()), ["job.json"])
        self.service.native_local = True
        self.service.authorize = Authorize()
        self.assertEqual(self.send()[0], DENIED)
        self.assertEqual(sorted(p.name for p in self.directory.iterdir()), ["job.json"])

    def fork_and_kill_after(self, action):
        read_fd, write_fd = os.pipe()
        pid = os.fork()
        if pid == 0:
            os.close(read_fd)
            rc = action()
            os.write(write_fd, bytes([rc]))
            signal.pause()
            os._exit(2)
        os.close(write_fd)
        try:
            # Bounded observation, never PID-only polling.
            import select

            ready, _, _ = select.select([read_fd], [], [], 10)
            self.assertTrue(ready, "child failed to reach persistence barrier")
            self.assertEqual(os.read(read_fd, 1), bytes([OK]))
        finally:
            os.kill(pid, signal.SIGKILL)
            _, status = os.waitpid(pid, 0)
            os.close(read_fd)
        self.assertTrue(os.WIFSIGNALED(status))

    def test_killed_sender_lost_receipt_and_recipient_replay(self):
        self.fork_and_kill_after(lambda: self.send()[0])
        self.assertEqual(self.send()[1].sequence, 1)
        self.assertEqual(len(self.record()["messages"]), 1)
        # A read before transcript persistence changes nothing; recovery replays.
        self.fork_and_kill_after(lambda: self.inbox()[0])
        self.assertEqual(self.inbox()[1][0].state, QUEUED)
        transcript = self.directory / "fixture-transcript.json"
        transcript.write_text(json.dumps([{"run": RUN.decode(), "id": "m1"}]))
        self.fork_and_kill_after(lambda: self.access("mark_delivered")[0])
        replay = self.inbox()[1]
        self.assertEqual(replay[0].state, DELIVERED)
        persisted_ids = {
            (v["run"], v["id"]) for v in json.loads(transcript.read_text())
        }
        injected = [
            m
            for m in replay
            if (m.sender.run.decode(), m.id.decode()) not in persisted_ids
        ]
        self.assertEqual(injected, [])  # consumer algorithm, not native-loop proof
        self.fork_and_kill_after(lambda: self.access("ack")[0])
        self.assertEqual(self.inbox(), (OK, []))
        self.assertEqual(self.access("ack")[0], OK)

    def test_death_before_send_and_corrupt_authority_refusal(self):
        # Killed before entering send: no acceptance, no empty mailbox file.
        self.fork_and_kill_after(lambda: OK)
        self.assertFalse((self.directory / "mailbox.json").exists())
        self.assertEqual(self.send()[0], OK)
        before = (self.directory / "mailbox.json").read_bytes()
        # A rejected/corrupt record must never be replaced with a fresh inbox.
        self.job["items"][2]["cancel_requested"] = "false"
        self.write_job()
        self.assertEqual(self.send(b"new")[0], CORRUPT)
        self.assertEqual((self.directory / "mailbox.json").read_bytes(), before)

    def test_maximum_escaped_payload_and_byte_pagination(self):
        for i in range(5):
            self.assertEqual(self.send(f"big{i}".encode(), b"\x01" * 16384)[0], OK)
        rc, batch = self.inbox()
        self.assertEqual(rc, OK)
        self.assertEqual(len(batch), 4)
        self.assertTrue(all(m.payload_len == 16384 for m in batch))
        self.assertEqual(self.inbox(after=batch[-1].sequence)[1][0].id, b"big4")
        self.assertEqual(len(self.record()["messages"][0]["payload"]), 16384)

    def test_authority_revocation_blocks_all_access(self):
        self.assertEqual(self.send()[0], OK)
        self.job["fixture_authority"].pop("0")
        self.write_job()
        self.assertEqual(self.inbox(), (DENIED, []))
        for operation in ("read", "mark_delivered", "ack"):
            self.assertEqual(self.access(operation)[0], DENIED)
        self.assertEqual(self.record()["messages"][0]["state"], QUEUED)
        self.job["fixture_authority"].pop("-1")
        self.write_job()
        self.assertEqual(self.send()[0], DENIED)

    def test_concurrent_duplicate_send_single_durable_sequence(self):
        children = []
        for _ in range(4):
            pid = os.fork()
            if pid == 0:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    rc, msg = self.send()
                    if rc != BUSY:
                        os._exit(0 if rc == OK and msg.sequence == 1 else 1)
                    time.sleep(0.01)
                os._exit(2)
            children.append(pid)
        statuses = []
        deadline = time.monotonic() + 10
        try:
            while children and time.monotonic() < deadline:
                for pid in children[:]:
                    done, status = os.waitpid(pid, os.WNOHANG)
                    if done:
                        children.remove(pid)
                        statuses.append(os.waitstatus_to_exitcode(status))
                if children:
                    time.sleep(0.01)
            self.assertFalse(children, "concurrent send exceeded deadline")
        finally:
            for pid in children:
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
        self.assertEqual(statuses, [0] * 4)
        self.assertEqual(len(self.record()["messages"]), 1)


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary(), verbosity=2)
