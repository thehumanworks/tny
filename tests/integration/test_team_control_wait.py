#!/usr/bin/env python3
"""Regression coverage for notification-driven team wait-any.

The real team/jobs driver is relinked with wrappers around only the wait's
status and jobs_host watch calls. The trace lives outside the watched run
directory, so measuring snapshots cannot wake the operation being measured.
No provider account or live inference is used.
"""

from __future__ import annotations

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

import test_jobs as jobs
import test_team_control as team_control

ROOT = jobs.ROOT

WRAPPERS = r"""
#include "util/jobs_host.h"
#include <fcntl.h>
#include <unistd.h>

static int wait_test_status_calls;
static int wait_test_drains;

static void wait_test_touch(const char *path) {
    if (!path || !*path) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        if (write(fd, "1", 1) != 1) abort();
        close(fd);
    }
}
static void wait_test_trace(char event) {
    const char *path = getenv("TNY_WAIT_TEST_TRACE");
    if (!path) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) {
        if (write(fd, &event, 1) != 1) abort();
        close(fd);
    }
}
static void wait_test_barrier(const char *marker_name, const char *ack_name) {
    const char *marker = getenv(marker_name), *ack = getenv(ack_name);
    if (!marker || !ack) return;
    wait_test_touch(marker);
    int64_t deadline = monotonic_ms() + 5000;
    while (access(ack, F_OK) != 0 && monotonic_ms() < deadline) usleep(1000);
}
static bool wait_test_cancelled(void *unused) {
    (void)unused;
    const char *path = getenv("TNY_WAIT_TEST_CANCEL");
    return path && access(path, F_OK) == 0;
}

int tny_wait_test_jobs_run_cancel(tny_ctx *ctx, tny_jobs_op op, yyjson_val *args, buf_t *out,
                                  char *err, size_t n, bool (*cancelled)(void *), void *ud) {
    if (op == TNY_JOBS_OP_STATUS) {
        wait_test_status_calls++;
        wait_test_trace('S');
    }
    int rc = tny_jobs_run_cancel(ctx, op, args, out, err, n, cancelled, ud);
    if (op == TNY_JOBS_OP_STATUS && wait_test_status_calls > 1 && out && out->data &&
        getenv("TNY_WAIT_TEST_FAKE_ATTEMPT")) {
        static const char field[] = "\"attempt\":";
        for (size_t i = 0; i + sizeof field < out->len; i++) {
            if (!memcmp(out->data + i, field, sizeof field - 1) &&
                out->data[i + sizeof field - 1] == '1') {
                out->data[i + sizeof field - 1] = '2';
                break;
            }
        }
    }
    if (op == TNY_JOBS_OP_STATUS && getenv("TNY_WAIT_TEST_INCOHERENT") && out->data) {
        char *state = strstr(out->data, "\"state\":\"running\"");
        if (state) memcpy(state + strlen("\"state\":\""), "pending", 7);
    }
    if (op == TNY_JOBS_OP_STATUS && wait_test_status_calls == 1 &&
        getenv("TNY_WAIT_TEST_SELF_EVENT")) {
        char path[4096];
        snprintf(path, sizeof path, "%s/jobs/%s/.wait-self-event", ctx->tny_dir,
                 jget_str(args, "id"));
        wait_test_touch(path);
    }
    return rc;
}
bool tny_wait_test_watch_supported(void) {
    return getenv("TNY_WAIT_TEST_NO_WATCH") ? false : tny_jobs_host_watch_supported();
}
int tny_wait_test_watch_open(const char *directory, tny_jobs_watch *watch) {
    wait_test_trace('O');
    return tny_jobs_host_watch_open(directory, watch);
}
int tny_wait_test_watch_drain(tny_jobs_watch *watch) {
    int rc = tny_jobs_host_watch_drain(watch);
    wait_test_drains++;
    wait_test_trace('D');
    const char *at = getenv("TNY_WAIT_TEST_BARRIER_DRAIN");
    if (!rc && at && wait_test_drains == atoi(at))
        wait_test_barrier("TNY_WAIT_TEST_MARKER", "TNY_WAIT_TEST_ACK");
    return rc;
}
int tny_wait_test_watch_next(tny_jobs_watch *watch, int timeout_ms,
                             bool (*cancelled)(void *), void *ud) {
    wait_test_trace('N');
    wait_test_touch(getenv("TNY_WAIT_TEST_NEXT_MARKER"));
    const char *ack = getenv("TNY_WAIT_TEST_NEXT_ACK");
    if (ack) {
        int64_t deadline = monotonic_ms() + 5000;
        while (access(ack, F_OK) != 0 && monotonic_ms() < deadline) usleep(1000);
    }
    return tny_jobs_host_watch_next(watch, timeout_ms, cancelled, ud);
}
"""


def build_wait_driver(directory: Path) -> str:
    def run_build(command):
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(
                f"{shlex.join(command)} failed ({result.returncode}):\n"
                f"{result.stdout}{result.stderr}"
            )
        return result

    run_build(["make", "release"])
    plan = subprocess.run(
        ["make", "-n", "-B", "release"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.splitlines()
    main_compile = shlex.split(
        next(
            line
            for line in plan
            if " -o build/rel/src/main.o " in line and " -c " in line
        )
    )
    wait_compile = shlex.split(
        next(
            line
            for line in plan
            if " -o build/rel/src/core/team_control.o " in line and " -c " in line
        )
    )
    link = shlex.split(next(line for line in plan if " -o build/tny " in line))

    driver_source = directory / "driver.c"
    driver_source.write_text(
        team_control.DRIVER.replace(
            "static bool interrupt_wait(void *p) { (void)p; return true; }",
            "static bool interrupt_wait(void *p) { (void)p; return true; }\n"
            + WRAPPERS,
        )
        .replace(
            'strcmp(mode, "interrupt") == 0 ? interrupt_wait : NULL, NULL);',
            'strcmp(mode, "interrupt") == 0 ? interrupt_wait :\n'
            '                strcmp(mode, "wait-cancel") == 0 ? wait_test_cancelled : NULL, NULL);',
        )
        .replace("MAIN_SOURCE", str(ROOT / "src/main.c"))
    )
    driver_object = directory / "driver.o"
    main_compile[main_compile.index("-o") + 1] = str(driver_object)
    main_compile[main_compile.index("src/main.c")] = str(driver_source)
    run_build(main_compile)

    wait_object = directory / "team_control.o"
    wait_compile[wait_compile.index("-o") + 1] = str(wait_object)
    wait_compile.extend(
        [
            "-Dtny_jobs_run_cancel=tny_wait_test_jobs_run_cancel",
            "-Dtny_jobs_host_watch_supported=tny_wait_test_watch_supported",
            "-Dtny_jobs_host_watch_open=tny_wait_test_watch_open",
            "-Dtny_jobs_host_watch_drain=tny_wait_test_watch_drain",
            "-Dtny_jobs_host_watch_next=tny_wait_test_watch_next",
        ]
    )
    run_build(wait_compile)

    binary = directory / "tny"
    link[link.index("-o") + 1] = str(binary)
    link[link.index("build/rel/src/main.o")] = str(driver_object)
    link[link.index("build/rel/src/core/team_control.o")] = str(wait_object)
    run_build(link)
    return str(binary)


class TeamControlWaitTests(jobs.JobsFixture):
    hold = 4.0

    @classmethod
    def setUpClass(cls):
        if jobs.WASM:
            raise unittest.SkipTest("team completion watches require a native host")
        cls.driver_tmp = tempfile.TemporaryDirectory(prefix="tny-team-wait-driver-")
        cls.original_tny = jobs.TNY
        jobs.TNY = os.environ.get("TNY_TEAM_WAIT_DRIVER") or build_wait_driver(
            Path(cls.driver_tmp.name)
        )

    @classmethod
    def tearDownClass(cls):
        jobs.TNY = cls.original_tny
        cls.driver_tmp.cleanup()

    def setUp(self):
        super().setUp()
        self.env.pop("TNY_NESTED", None)
        self.state["dag_entered"] = threading.Event()
        self.state["dag_release"] = threading.Event()

    def tearDown(self):
        self.state["dag_release"].set()
        super().tearDown()

    def team(self, op, request, *, check=True, env=None):
        result = self.run_tny(
            "team",
            op,
            "--request",
            "-",
            "--json",
            stdin=json.dumps(request).encode(),
            check=check,
            env=env,
        )
        return result, json.loads(result.stdout) if result.stdout.strip() else None

    def start_held(self):
        specification = {
            "kind": "ask",
            "dag": True,
            "concurrency": 3,
            "items": [
                {"role": "lead", "label": "lead", "prompt": "read only: LEAD"},
                {
                    "role": "worker",
                    "label": "held",
                    "prompt": "read only: DAG_BARRIER",
                },
                {"role": "worker", "label": "peer", "prompt": "read only: PEER"},
            ],
        }
        _, handle = self.team("start", specification)
        self.assertTrue(self.state["dag_entered"].wait(20))
        run = handle["run_id"]
        deadline = time.monotonic() + 10
        while True:
            items = self.status(run)["items"]
            if all(items[index]["state"] in jobs.TERMINAL for index in (0, 2)):
                return run
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.01)

    def wait_async(self, run, env, timeout_ms=3000):
        result = {}

        def invoke():
            result["value"] = self.team(
                "wait-any",
                {"id": run, "item": 1, "timeout_ms": timeout_ms},
                check=False,
                env=env,
            )

        thread = threading.Thread(target=invoke, daemon=True)
        thread.start()
        return thread, result

    def wait_for_path(self, path: Path):
        deadline = time.monotonic() + 10
        while not path.exists() and time.monotonic() < deadline:
            time.sleep(0.005)
        self.assertTrue(path.exists(), f"timed out waiting for {path}")

    def join_wait(self, thread, result):
        thread.join(10)
        self.assertFalse(thread.is_alive(), "wait-any did not terminate")
        return result["value"]

    def test_quiet_deadline_has_only_initial_and_final_snapshots(self):
        run = self.start_held()
        trace = self.workspace / "wait.trace"
        env = {
            **self.env,
            "TNY_TEAM_TEST_MODE": "session",
            "TNY_WAIT_TEST_TRACE": str(trace),
            "TNY_WAIT_TEST_SELF_EVENT": "1",
        }
        started = time.monotonic()
        result, event = self.team(
            "wait-any",
            {"id": run, "item": 1, "timeout_ms": 260},
            check=False,
            env=env,
        )
        self.assertEqual(result.returncode, 124)
        self.assertEqual(event["kind"], "team_wait_timeout")
        self.assertGreaterEqual(time.monotonic() - started, 0.20)
        events = trace.read_text()
        self.assertEqual(events.count("S"), 2, events)
        self.assertEqual(events.count("N"), 1, events)

    def test_continuous_snapshot_mismatch_still_obeys_deadline(self):
        run = self.start_held()
        trace = self.workspace / "wait.trace"
        env = {
            **self.env,
            "TNY_TEAM_TEST_MODE": "session",
            "TNY_WAIT_TEST_TRACE": str(trace),
            "TNY_WAIT_TEST_INCOHERENT": "1",
        }
        started = time.monotonic()
        process, event = self.team(
            "wait-any",
            {"id": run, "item": 1, "timeout_ms": 80},
            check=False,
            env=env,
        )
        self.assertEqual(process.returncode, 124)
        self.assertEqual(event["kind"], "team_wait_timeout")
        self.assertLess(time.monotonic() - started, 1.0)
        self.assertGreaterEqual(trace.read_text().count("S"), 2)

    def test_notification_between_subscribe_and_snapshot_is_observed(self):
        run = self.start_held()
        marker = self.workspace / "drained"
        ack = self.workspace / "continue"
        trace = self.workspace / "wait.trace"
        env = {
            **self.env,
            "TNY_TEAM_TEST_MODE": "session",
            "TNY_WAIT_TEST_TRACE": str(trace),
            "TNY_WAIT_TEST_BARRIER_DRAIN": "1",
            "TNY_WAIT_TEST_MARKER": str(marker),
            "TNY_WAIT_TEST_ACK": str(ack),
        }
        thread, result = self.wait_async(run, env)
        self.wait_for_path(marker)
        self.state["dag_release"].set()
        deadline = time.monotonic() + 10
        while self.status(run)["items"][1]["state"] not in jobs.TERMINAL:
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.01)
        ack.touch()
        process, event = self.join_wait(thread, result)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(event["kind"], "team_completion")
        self.assertEqual(event["cursor"], {"item": 1, "attempt": 1})
        self.assertLess(trace.read_text().index("D"), trace.read_text().index("S"))

    def test_change_after_status_drain_forces_coherent_resnapshot(self):
        run = self.start_held()
        marker = self.workspace / "drained-after-status"
        ack = self.workspace / "continue"
        trace = self.workspace / "wait.trace"
        env = {
            **self.env,
            "TNY_TEAM_TEST_MODE": "session",
            "TNY_WAIT_TEST_TRACE": str(trace),
            "TNY_WAIT_TEST_BARRIER_DRAIN": "2",
            "TNY_WAIT_TEST_MARKER": str(marker),
            "TNY_WAIT_TEST_ACK": str(ack),
        }
        thread, result = self.wait_async(run, env)
        self.wait_for_path(marker)
        self.state["dag_release"].set()
        deadline = time.monotonic() + 10
        while self.status(run)["items"][1]["state"] not in jobs.TERMINAL:
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.01)
        ack.touch()
        process, event = self.join_wait(thread, result)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(event["kind"], "team_completion")
        self.assertEqual(event["cursor"], {"item": 1, "attempt": 1})
        self.assertGreaterEqual(trace.read_text().count("S"), 2)

    def test_completion_notification_wakes_wait(self):
        run = self.start_held()
        marker = self.workspace / "waiting"
        trace = self.workspace / "wait.trace"
        env = {
            **self.env,
            "TNY_TEAM_TEST_MODE": "session",
            "TNY_WAIT_TEST_TRACE": str(trace),
            "TNY_WAIT_TEST_NEXT_MARKER": str(marker),
        }
        thread, result = self.wait_async(run, env)
        self.wait_for_path(marker)
        self.state["dag_release"].set()
        process, event = self.join_wait(thread, result)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(event["kind"], "team_completion")
        # If replacement overlaps the public snapshot, the private confirmation
        # deliberately forces one immediate coherent resnapshot.
        self.assertGreaterEqual(trace.read_text().count("S"), 2)

    def test_cancellation_stops_watch_without_cancelling_job(self):
        run = self.start_held()
        marker = self.workspace / "waiting"
        cancel = self.workspace / "cancel"
        trace = self.workspace / "wait.trace"
        env = {
            **self.env,
            "TNY_TEAM_TEST_MODE": "wait-cancel",
            "TNY_WAIT_TEST_TRACE": str(trace),
            "TNY_WAIT_TEST_NEXT_MARKER": str(marker),
            "TNY_WAIT_TEST_CANCEL": str(cancel),
        }
        thread, result = self.wait_async(run, env)
        self.wait_for_path(marker)
        cancel.touch()
        process, event = self.join_wait(thread, result)
        self.assertEqual(process.returncode, 130)
        self.assertIsNone(event)
        self.assertFalse(self.status(run)["cancel_requested"])
        self.assertEqual(trace.read_text().count("S"), 1)

    def test_attempt_change_after_notification_fails_closed(self):
        run = self.start_held()
        marker = self.workspace / "waiting"
        ack = self.workspace / "continue"
        trace = self.workspace / "wait.trace"
        env = {
            **self.env,
            "TNY_TEAM_TEST_MODE": "session",
            "TNY_WAIT_TEST_TRACE": str(trace),
            "TNY_WAIT_TEST_NEXT_MARKER": str(marker),
            "TNY_WAIT_TEST_NEXT_ACK": str(ack),
            "TNY_WAIT_TEST_FAKE_ATTEMPT": "1",
        }
        thread, result = self.wait_async(run, env)
        self.wait_for_path(marker)
        # A harmless directory notification drives a new canonical snapshot;
        # the test wrapper changes only that public projection's attempt.
        event_path = self.home / ".tny" / "jobs" / run / ".attempt-event"
        event_path.write_text("1")
        ack.touch()
        process, event = self.join_wait(thread, result)
        self.assertEqual(process.returncode, 1)
        self.assertIsNone(event)
        self.assertIn(b"attempt or membership changed", process.stderr)
        self.assertEqual(trace.read_text().count("S"), 2)

    def test_watch_loss_is_reported_instead_of_timing_out(self):
        run = self.start_held()
        marker = self.workspace / "waiting"
        env = {
            **self.env,
            "TNY_TEAM_TEST_MODE": "session",
            "TNY_WAIT_TEST_NEXT_MARKER": str(marker),
        }
        thread, result = self.wait_async(run, env)
        self.wait_for_path(marker)
        run_dir = self.home / ".tny" / "jobs" / run
        moved = run_dir.with_name(run + ".moved")
        os.replace(run_dir, moved)
        try:
            process, event = self.join_wait(thread, result)
        finally:
            os.replace(moved, run_dir)
        self.assertEqual(process.returncode, 1)
        self.assertIsNone(event)
        self.assertIn(b"notification watch was lost", process.stderr)

    def test_final_snapshot_projects_abandoned_owner(self):
        run = self.start_held()
        marker = self.workspace / "waiting"
        env = {
            **self.env,
            "TNY_TEAM_TEST_MODE": "session",
            "TNY_WAIT_TEST_NEXT_MARKER": str(marker),
        }
        thread, result = self.wait_async(run, env, timeout_ms=300)
        self.wait_for_path(marker)
        os.kill(self.worker_pid(run), signal.SIGKILL)
        process, event = self.join_wait(thread, result)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(event["kind"], "team_completion")
        self.assertEqual(event["item"]["state"], "interrupted")

    def test_positive_wait_requires_notifications_but_status_does_not(self):
        run = self.start_held()
        env = {
            **self.env,
            "TNY_TEAM_TEST_MODE": "session",
            "TNY_WAIT_TEST_NO_WATCH": "1",
        }
        refused, _ = self.team(
            "wait-any",
            {"id": run, "item": 1, "timeout_ms": 1},
            check=False,
            env=env,
        )
        self.assertEqual(refused.returncode, 1)
        self.assertIn(b"require native directory notifications", refused.stderr)
        nonblocking, event = self.team(
            "wait-any",
            {"id": run, "item": 1, "timeout_ms": 0},
            check=False,
            env=env,
        )
        self.assertEqual(nonblocking.returncode, 124)
        self.assertEqual(event["kind"], "team_wait_timeout")
        status, event = self.team("status", {"id": run}, env=env)
        self.assertEqual(status.returncode, 0)
        self.assertEqual(event["kind"], "team")


def load_tests(loader, tests, pattern):
    return unittest.TestSuite(
        TeamControlWaitTests(name)
        for name in sorted(TeamControlWaitTests.__dict__)
        if name.startswith("test_")
    )


if __name__ == "__main__":
    unittest.main(argv=jobs.argv_without_runner_binary())
