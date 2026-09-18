#!/usr/bin/env python3
"""Real CLI startup faults. Run after make release; no live credentials needed."""

import fcntl
import hashlib
import json
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from test_jobs import JobsFixture, argv_without_runner_binary

ROOT = Path(__file__).resolve().parents[2]
RUN = "0123456789abcdef0123456789abcdef"
TOKEN = "a" * 64

# Only syscall errors are injected. No turn, provider response or success is mocked.
FAULT = r"""
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
#include <stdio.h>
static int seen;
static int fault_flock(int fd, int op) {
#ifdef __APPLE__
    int (*real_fn)(int, int) = flock;
#else
    int (*real_fn)(int, int) = dlsym(RTLD_NEXT, "flock");
#endif
    char path[4096] = {0};
#ifdef __APPLE__
    (void)fcntl(fd, F_GETPATH, path);
#else
    char proc[80];
    snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
    (void)readlink(proc, path, sizeof path - 1);
#endif
    const char *mode = getenv("STARTUP_FAULT");
    if (mode && strstr(path, "/state.lock") && (op & LOCK_EX)) {
        seen++;
        if (!strcmp(mode, "busy") || (!strcmp(mode, "release") && seen <= 3)) {
            errno = EWOULDBLOCK;
            return -1;
        }
        if (!strcmp(mode, "io")) { errno = EIO; return -1; }
    }
    return real_fn(fd, op);
}
static int fault_rename(const char *old, const char *dest) {
#ifdef __APPLE__
    int (*real_fn)(const char *, const char *) = rename;
#else
    int (*real_fn)(const char *, const char *) = dlsym(RTLD_NEXT, "rename");
#endif
    const char *mode = getenv("STARTUP_FAULT");
    if (mode && !strcmp(mode, "persist") && seen && strstr(dest, "/session.json")) {
        errno = EIO;
        return -1;
    }
    return real_fn(old, dest);
}
#ifdef __APPLE__
__attribute__((used, section("__DATA,__interpose")))
static struct { const void *replacement, *original; } hooks[] = {
    {(const void *)fault_flock, (const void *)flock},
    {(const void *)fault_rename, (const void *)rename}
};
#else
int flock(int fd, int op) { return fault_flock(fd, op); }
int rename(const char *a, const char *b) { return fault_rename(a, b); }
#endif
"""


class Startup(JobsFixture):
    @classmethod
    def setUpClass(cls):
        cls.compiled = tempfile.TemporaryDirectory(prefix="tny-startup-fixture-")
        source = Path(cls.compiled.name) / "fault.c"
        source.write_text(FAULT)
        cls.library = source.with_suffix(".so")
        subprocess.run(
            [
                "cc",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-shared",
                "-fPIC",
                str(source),
                "-o",
                str(cls.library),
            ]
            + ([] if sys.platform == "darwin" else ["-ldl"]),
            check=True,
        )

        helper_source = Path(cls.compiled.name) / "reader.c"
        helper_source.write_text(r"""
#include "core/team_runtime.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc != 5) return 3;
    tny_ctx ctx = {0};
    ctx.tny_dir = argv[1];
    const char *category = tny_team_startup_diagnostic_read(&ctx, argv[2], atoi(argv[3]), atoi(argv[4]));
    if (!category) return 2;
    puts(category);
    return 0;
}
""")
        helper_object = helper_source.with_suffix(".o")
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-Isrc",
                "-Iinclude",
                "-Ithird_party/yyjson",
                "-c",
                str(helper_source),
                "-o",
                str(helper_object),
            ],
            cwd=ROOT,
            check=True,
        )
        # Reuse the checkout's exact platform/toolchain link contract, never a
        # lead binary or source copy. The public CLI still drives every fault.
        commands = subprocess.check_output(
            ["make", "-Bn", "release"], cwd=ROOT, text=True
        )
        command = next(
            shlex.split(line)
            for line in commands.splitlines()
            if " -o build/tny " in line
        )
        cls.reader = helper_source.with_suffix("")
        command[command.index("-o") + 1] = str(cls.reader)
        command[command.index("build/rel/src/main.o")] = str(helper_object)
        subprocess.run(command, cwd=ROOT, check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.compiled.cleanup()

    def setUp(self):
        super().setUp()
        for key in list(self.env):
            if key.startswith("TNY_TEAM_") or key in (
                "TNY_NESTED",
                "TNY_SESSION_ID",
                "TNY_SESSION_SOCK",
            ):
                self.env.pop(key)
        _, seed = self.submit(
            "ask",
            "--request",
            "-",
            stdin=json.dumps(
                {
                    "kind": "ask",
                    "dag": True,
                    "items": [{"prompt": "seed", "role": "lead"}, {"prompt": "seed"}],
                }
            ).encode(),
        )
        self.await_terminal(seed["id"])
        seeded = self.record_at(self.jobs_root() / seed["id"])
        self.state["requests"].clear()
        self.state["bodies"].clear()
        self.env.update(
            TNY_ISOLATE="0",
            TNY_TEAM_RUN=RUN,
            TNY_TEAM_TASK="0",
            TNY_TEAM_ATTEMPT="1",
            TNY_TEAM_CAPABILITY=TOKEN,
        )
        self.directory = self.jobs_root() / RUN
        self.directory.mkdir(parents=True)
        self.record = {
            "version": 1,
            "kind": "job",
            "id": RUN,
            "dag": True,
            "state": "failed",
            "attempt": 1,
            "cancel_requested": False,
            "items": [
                dict(
                    index=i,
                    attempt=1,
                    state="failed",
                    cancel_requested=False,
                    role="lead" if i == 0 else "worker",
                    mailbox_capability_sha256=hashlib.sha256(
                        TOKEN.encode()
                    ).hexdigest(),
                )
                for i in range(2)
            ],
        }
        for item, original in zip(self.record["items"], seeded["items"]):
            original.update(item)
        seeded.update({k: v for k, v in self.record.items() if k != "items"})
        self.record = seeded
        self.save_record()
        self.sidecar = self.directory / "startup-0-1.json"

    def save_record(self):
        (self.directory / "job.json").write_text(json.dumps(self.record))

    def ask(self, fault=None, **overrides):
        env = dict(self.env, **overrides)
        if fault:
            env["STARTUP_FAULT"] = fault
            env[
                "DYLD_INSERT_LIBRARIES" if sys.platform == "darwin" else "LD_PRELOAD"
            ] = str(self.library)
        return self.run_tny(
            "ask", "--events=jsonl", "startup probe", env=env, check=False, timeout=15
        )

    def category(self, expected):
        value = json.loads(self.sidecar.read_text())
        self.assertEqual(value, dict(run=RUN, task=0, attempt=1, category=expected))
        self.assertLessEqual(self.sidecar.stat().st_size, 256)
        self.assertEqual(self.sidecar.stat().st_mode & 0o777, 0o600)
        self.assertNotIn(TOKEN, self.sidecar.read_text())

    def read_category(self, task=0, attempt=1):
        result = subprocess.run(
            [str(self.reader), str(self.home / ".tny"), RUN, str(task), str(attempt)],
            capture_output=True,
            timeout=5,
        )
        return result.returncode, result.stdout.decode().strip()

    def test_reader_rejects_stale_malformed_and_symlink(self):
        self.assertEqual(self.ask("busy").returncode, 2)
        self.assertEqual(self.read_category(), (0, "MAILBOX_BUSY"))
        self.assertEqual(self.read_category(attempt=2), (2, ""))
        self.assertEqual(self.read_category(task=1), (2, ""))
        good = self.sidecar.read_text()
        for changes in (
            dict(task=1),
            dict(attempt=2),
            dict(run="f" * 32),
            dict(category="raw secret"),
            dict(category="MAILBOX_BUSY\x00secret"),
            dict(extra="secret"),
        ):
            value = json.loads(good)
            value.update(changes)
            self.sidecar.write_text(json.dumps(value))
            self.assertEqual(self.read_category(), (2, ""), changes)
        self.sidecar.unlink()
        target = self.home / "outside.json"
        target.write_text(good)
        self.sidecar.symlink_to(target)
        self.assertEqual(self.read_category(), (2, ""))
        self.assertEqual(self.ask("busy").returncode, 2)
        self.assertEqual(target.read_text(), good)

    def test_real_state_lock_does_not_block_diagnostic(self):
        with (self.directory / "state.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.ask()
            self.assertEqual(result.returncode, 2, result.stderr)
            self.category("MAILBOX_BUSY")
            self.assertEqual(self.read_category(), (0, "MAILBOX_BUSY"))
            self.assertEqual(self.ask_requests(), [])
        # First category wins, even if a later generic startup error occurs.
        self.ask(OPENAI_BASE_URL="https://provider.invalid/v1", OPENAI_API_KEY="")
        self.category("MAILBOX_BUSY")
        self.record["attempt"] = 2
        self.save_record()
        self.assertEqual(self.read_category(), (2, ""))

    def test_busy_release_exactly_one_post(self):
        result = self.ask("release")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.ask_requests()), 1)
        self.assertFalse(self.sidecar.exists())

    def test_busy_bound_no_post(self):
        result = self.ask("busy")
        self.assertEqual(result.returncode, 2, result.stderr)

        self.assertEqual(self.ask_requests(), [])
        self.category("MAILBOX_BUSY")

    def test_mailbox_io_no_post(self):
        self.assertEqual(self.ask("io").returncode, 2)
        self.assertEqual(self.ask_requests(), [])
        self.category("MAILBOX_IO")

    def test_context_persistence_no_post(self):
        result = self.ask("persist")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(self.ask_requests(), [])
        self.category("CONTEXT_PERSISTENCE")

    def test_provider_start_no_post(self):
        result = self.ask(
            OPENAI_BASE_URL="https://provider.invalid/v1", OPENAI_API_KEY=""
        )
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(self.ask_requests(), [])
        self.category("PROVIDER_START")

    def test_wrong_capability_attempt_and_identity_refuse_write(self):
        for overrides in (
            dict(TNY_TEAM_CAPABILITY="b" * 64),
            dict(TNY_TEAM_ATTEMPT="2"),
            dict(TNY_TEAM_TASK="63"),
            dict(TNY_TEAM_RUN="f" * 32),
        ):
            with self.subTest(overrides=overrides):
                self.assertNotEqual(self.ask("busy", **overrides).returncode, 0)
                self.assertEqual(list(self.directory.glob("startup-*.json")), [])
        self.assertEqual(self.ask_requests(), [])

    def test_refused_write_preserves_failure(self):
        self.sidecar.mkdir()
        result = self.ask("busy")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertTrue(self.sidecar.is_dir())
        self.assertEqual(self.ask_requests(), [])

    def test_failed_status_and_notification_once(self):
        status = self.run_tny("jobs", "status", RUN, "--json", check=False)
        self.assertEqual(status.returncode, 2)
        self.assertEqual(json.loads(status.stdout)["state"], "failed")
        result = self.ask()
        self.assertEqual(result.returncode, 0, result.stderr)
        bodies = json.dumps(self.state["bodies"])
        self.assertEqual(bodies.count("Team execution notification:"), 1)
        self.assertIn("state failed", bodies)
        session = json.loads(result.stdout.splitlines()[-1])["session_id"]
        self.state["bodies"].clear()
        resumed = self.run_tny(
            "ask", "--resume", session, "--events=jsonl", "again", check=False
        )
        self.assertEqual(resumed.returncode, 0, resumed.stderr)
        # Existing context is retained, but a second notification is not added.
        self.assertEqual(
            json.dumps(self.state["bodies"]).count("Team execution notification:"), 1
        )

    def test_unreadable_member_fails_closed(self):
        records = ["not json"]
        for fields in (
            dict(kind="other"),
            dict(id="f" * 32),
            dict(dag=False),
            dict(items=[]),
        ):
            records.append(json.dumps(dict(self.record, **fields)))
        for record in records:
            with self.subTest(record=record[:80]):
                (self.directory / "job.json").write_text(record)
                self.assertEqual(self.ask().returncode, 2)
                self.assertEqual(self.ask_requests(), [])
                self.assertFalse(self.sidecar.exists())


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary())
