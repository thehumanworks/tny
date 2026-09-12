#!/usr/bin/env python3
"""Native MSYS2 job scopes: real CLI flows and compiled admission barriers."""

import contextlib
import json
import os
import shlex
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

import test_jobs as jobs

MSYS = os.name == "posix" and os.uname().sysname.startswith(("MSYS", "CYGWIN"))


def proc_matches(*needles):
    matches = []
    for directory in Path("/proc").iterdir():
        if not directory.name.isdigit():
            continue
        try:
            args = (
                (directory / "cmdline")
                .read_bytes()
                .replace(b"\0", b" ")
                .decode(errors="replace")
            )
        except OSError:
            continue
        if all(needle in args for needle in needles):
            matches.append((int(directory.name), args))
    return matches


@unittest.skipUnless(MSYS, "requires the native MSYS2 process runtime")
class NativeMsysJobs(jobs.JobsFixture):
    hold = 6.0

    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="tny-msys-scopes-")
        cls.addClassCleanup(cls.build.cleanup)
        directory = Path(cls.build.name)
        cls.tree = directory / "tree.exe"
        cls.scope = directory / "scope.exe"
        compiler = [
            "gcc",
            "-std=c11",
            "-D_DEFAULT_SOURCE",
            "-D_DARWIN_C_SOURCE",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(jobs.ROOT / "src"),
        ]
        subprocess.run(
            [
                *compiler,
                str(jobs.ROOT / "tests/fixtures/jobs_msys_tree.c"),
                "-o",
                str(cls.tree),
            ],
            check=True,
            capture_output=True,
        )
        source = (jobs.ROOT / "src/util/process_scope.c").read_text()
        paused = "extern void tny_scope_test_pause(int);\n" + source
        replacements = [
            (
                "    HANDLE job = OpenJobObjectA(JOB_OBJECT_ASSIGN_PROCESS, FALSE, name);",
                "    tny_scope_test_pause(0);\n    HANDLE job = OpenJobObjectA(JOB_OBJECT_ASSIGN_PROCESS, FALSE, name);",
            ),
            (
                "    bool admitted = AssignProcessToJobObject(job, GetCurrentProcess()) != 0;",
                "    tny_scope_test_pause(1);\n    bool admitted = AssignProcessToJobObject(job, GetCurrentProcess()) != 0;\n    tny_scope_test_pause(2);\n    tny_scope_test_pause(3);",
            ),
        ]
        for before, after in replacements:
            if paused.count(before) != 1:
                raise AssertionError(
                    "the admission barrier no longer matches its source boundary"
                )
            paused = paused.replace(before, after)
        scope_source = directory / "process_scope.pauses.c"
        scope_source.write_text(paused)
        subprocess.run(
            [
                *compiler,
                str(jobs.ROOT / "tests/fixtures/jobs_msys_scope.c"),
                str(scope_source),
                *(
                    str(jobs.ROOT / "src/util" / f)
                    for f in ["process.c", "util.c", "tny_poll.c"]
                ),
                "-o",
                str(cls.scope),
            ],
            check=True,
            capture_output=True,
        )
        # Compile a source-bound root-wait fault against the actual native
        # object inventory. This is a separate temporary executable, never shipped.
        variables = subprocess.check_output(
            ["make", "-s", "-f", "Makefile", "-f", "-", "msys-fault-flags"],
            cwd=jobs.ROOT,
            text=True,
            input=".PHONY: msys-fault-flags\nmsys-fault-flags:\n"
            "\t@printf '%s\\n' '$(CC)' '$(REL_CFLAGS) $(REL_INLINE) $(REL_SIZE_OPT)' '$(REL_LTO)' '$(REL_LDFLAGS)' '$(REL_OBJS)' '$(OBJ_REL)/src/util/process_scope.o'\n",
        ).splitlines()
        cc, flags, lto, linker, objects, scope_objects = map(shlex.split, variables)
        wait = "pid_t got = waitpid(scope->pid, &scope->status, WNOHANG);"
        if source.count(wait) != 1:
            raise AssertionError(
                "the wait-loss fault no longer matches its source boundary"
            )
        fault_source = directory / "process_scope.wait-loss.c"
        fault_source.write_text(source.replace(wait, "pid_t got = -1; errno = ECHILD;"))
        fault_object = directory / "wait-loss.o"
        cls.wait_loss = directory / "tny-wait-loss.exe"
        subprocess.run(
            [*cc, *flags, *lto, "-c", str(fault_source), "-o", str(fault_object)],
            cwd=jobs.ROOT,
            check=True,
            capture_output=True,
        )
        if len(scope_objects) != 1 or scope_objects[0] not in objects:
            raise AssertionError(
                "the native scope object is absent from the Makefile inventory"
            )
        original_scope = jobs.ROOT / scope_objects[0]
        retained = [
            str(jobs.ROOT / name)
            for name in objects
            if (jobs.ROOT / name).resolve() != original_scope.resolve()
        ]
        subprocess.run(
            [
                *cc,
                *flags,
                *lto,
                "-o",
                str(cls.wait_loss),
                *retained,
                str(fault_object),
                *linker,
            ],
            cwd=jobs.ROOT,
            check=True,
            capture_output=True,
        )
        cls.previous_matches = jobs.pids_matching
        cls.previous_argv = jobs.pids_argv
        jobs.pids_matching = lambda *needles: [
            pid for pid, _args in proc_matches(*needles)
        ]
        jobs.pids_argv = lambda *needles: [
            args for _pid, args in proc_matches(*needles)
        ]
        cls.addClassCleanup(setattr, jobs, "pids_matching", cls.previous_matches)
        cls.addClassCleanup(setattr, jobs, "pids_argv", cls.previous_argv)

    def start_sentinel(self):
        # A cooperative file lifetime avoids signalling a recycled sentinel PID.
        gate = self.workspace / "sentinel.gate"
        gate.touch()
        process = subprocess.Popen(
            [str(self.tree), "sentinel", str(gate)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )

        def finish():
            gate.unlink(missing_ok=True)
            process.wait(timeout=10)

        self.addCleanup(finish)
        return process

    test_durable_submitter_exit = jobs.JobsSubmitAndStatus.test_submit_returns_a_durable_id_and_paths_before_the_item_finishes
    test_queued_cancel_never_spends = jobs.JobsCancellation.test_cancelling_a_queued_item_prevents_any_provider_request
    test_running_cancel_preserves_sentinel = jobs.JobsCancellation.test_cancelling_a_running_item_stops_its_tree_and_spares_a_sentinel
    test_supervisor_loss_is_interrupted = jobs.JobsCancellation.test_a_killed_supervisor_reads_as_interrupted_with_unknown_cleanup
    test_concurrency = jobs.JobsConcurrency.test_the_concurrency_bound_holds_against_an_independent_counter
    test_forged_metadata_pid_is_not_authority = jobs.JobsCancellation.test_a_forged_pid_in_the_record_never_signals_an_unrelated_process

    def test_actual_supervisor_wait_loss_retains_claim_after_exit(self):
        output = self.workspace / "held.png"
        ordinary = jobs.TNY
        try:
            jobs.TNY = str(self.wait_loss)
            _, accepted = self.submit(
                "image",
                "--output-file",
                str(output),
                "--prompt",
                "held after wait loss",
            )
        finally:
            jobs.TNY = ordinary
        job = accepted["id"]
        final = self.await_terminal(job)
        self.assertEqual(
            (final["state"], final["cleanup"]), ("interrupted", "unknown"), final
        )
        self.assertTrue(final["cleanup_hold"], final)
        directory = Path(final["metadata_path"]).parent
        jobs.JobsReviewedRaces.wait_owner(self, directory, held=False)
        record = directory / "job.json"
        claim = next((self.jobs_root() / "reservations").glob("*.lock"))
        before = record.read_bytes(), claim.read_bytes()
        request_count = len(self.image_requests())
        contender, _ = self.submit(
            "image",
            "--output-file",
            str(output),
            "--prompt",
            "must not spend",
            check=False,
        )
        self.assertNotEqual(contender.returncode, 0)
        for args in [
            ("jobs", "retry", job, "--failed", "--json"),
            ("jobs", "rm", job, "--json"),
        ]:
            result = self.run_tny(*args, check=False)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertEqual((record.read_bytes(), claim.read_bytes()), before)
        self.assertEqual(len(self.image_requests()), request_count)

    def test_every_stopped_admission_boundary(self):
        for mode in ["individual", "supervisor-loss"]:
            for boundary in range(4):
                with self.subTest(mode=mode, boundary=boundary):
                    result = subprocess.run(
                        [str(self.scope), mode, str(boundary)],
                        capture_output=True,
                        timeout=40,
                    )
                    self.assertEqual(
                        result.returncode, 0, result.stdout + result.stderr
                    )
                    self.assertIn(b"RESULT PASS", result.stdout)
                    self.assertIn(b"external_sentinel_live=1", result.stdout)
                    self.assertNotIn(b"WORK_FORBIDDEN", result.stdout)

    def test_real_tool_tree_with_separate_session_and_auto_reaping_parent(self):
        for auto in [False, True]:
            with self.subTest(auto=auto):
                sentinel = self.start_sentinel()
                marker = self.workspace / f"tree-{auto}.pids"
                self.state["envdump"] = shlex.join(
                    [str(self.tree), "tree", str(marker)] + (["auto"] if auto else [])
                )
                result = self.run_tny(
                    "--permission-mode",
                    "yolo",
                    "jobs",
                    "submit",
                    "ask",
                    "--json",
                    stdin=f"ENVDUMP tree {auto}".encode(),
                )
                job = json.loads(result.stdout)["id"]
                deadline = time.monotonic() + 15
                pids = []
                while time.monotonic() < deadline:
                    with contextlib.suppress(OSError, ValueError):
                        pids = [int(value) for value in marker.read_text().split()]
                    if len(pids) == 3:
                        break
                    time.sleep(0.05)
                self.assertEqual(len(pids), 3, self.status(job))
                self.run_tny("jobs", "cancel", job, "--json")
                final = self.await_terminal(job)
                self.assertEqual(
                    (final["state"], final["cleanup"]), ("cancelled", "complete"), final
                )
                for pid in pids:
                    self.assertFalse(
                        jobs.running(pid), f"captured descendant {pid} still exists"
                    )
                self.assertIsNone(sentinel.poll())
                (self.workspace / "sentinel.gate").unlink(missing_ok=True)
                sentinel.wait(timeout=10)


if __name__ == "__main__":
    unittest.main(argv=jobs.argv_without_runner_binary())
