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
import fcntl
import hashlib
import json
import os
import shlex
import signal
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUN = b"0123456789abcdef0123456789abcdef"
OK, INVALID, UNSUPPORTED, DENIED, STALE, TERMINAL, BUSY = range(7)
FULL, HISTORY_FULL, CONFLICT, NOT_FOUND, BAD_STATE, CORRUPT, IO = range(7, 14)
QUEUED, DELIVERED, ACKED = range(3)
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

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
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

    def write_job(self):
        (self.directory / "job.json").write_text(json.dumps(self.job))

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
    unittest.main(verbosity=2)
