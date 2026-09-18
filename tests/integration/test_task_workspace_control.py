#!/usr/bin/env python3
"""Real workspace control C service/CLI adapter; registry/dispatch not mocked.

Run `make release` first. A temporary test-only driver links the actual release
objects, invokes cmd_task_workspace or the typed adapter, and uses the helper
only to supply the scheduler's not-yet-wired preparation. Authoritative job.json
fixtures use the DAG schema from e351f40 plus request.workspace.policy. There is
no provider call, fabricated service result, or public prepare operation here.
"""

from __future__ import annotations

import fcntl
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from test_jobs import argv_without_runner_binary

ROOT = Path(__file__).resolve().parents[2]
RUN = "0123456789abcdef0123456789abcdef"
DRIVER = r"""
#include "cli/cli.h"
#include "core/tools_workspace.h"
#include "core/jobs.h"
#include "util/task_workspace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int cmd_task_workspace(tny_ctx *, const cli_globals *, int, char **);
int main(int argc, char **argv) {
    if (argc < 4) return 99;
    tny_ctx ctx = {0};
    ctx.tny_dir = argv[1]; ctx.cwd = argv[2]; ctx.perm_mode = TNY_MODE_YOLO;
    ctx.tool_profile = TNY_TOOLS_ALL;
    ctx.library_mode = getenv("WS_TEST_EMBEDDED") != NULL;
    ctx.ssh_host = getenv("WS_TEST_SSH") ? (char *)"fixture.invalid" : NULL;
    if (getenv("WS_TEST_ASK")) ctx.perm_mode = TNY_MODE_ASK;
    char err[512] = "";
    if (strcmp(argv[3], "_fixture_prepare") == 0 && argc == 7) {
        task_workspace *w = NULL;
        task_workspace_id id = {argv[4], atoi(argv[5]), atoi(argv[6])};
        int rc = task_workspace_prepare(ctx.cwd, id, NULL, &w, err, sizeof err);
        if (!rc) puts(task_workspace_path(w));
        else fprintf(stderr, "%s\n", err);
        task_workspace_close(w);
        return rc ? 1 : 0;
    }
    if (strcmp(argv[3], "_fixture_retry") == 0 && argc == 5) {
        yyjson_doc *d = jparse(argv[4], strlen(argv[4]));
        buf_t out = {0};
        int rc = tny_jobs_run(&ctx, TNY_JOBS_OP_RETRY, yyjson_doc_get_root(d), &out, err, sizeof err);
        if (out.len) fwrite(out.data, 1, out.len, stdout);
        fprintf(stderr, "%s\n", err);
        yyjson_doc_free(d); buf_free(&out); return rc;
    }
    if (strcmp(argv[3], "api") == 0 && argc == 6) {
        yyjson_doc *d = jparse(argv[5], strlen(argv[5]));
        yyjson_val *args = d ? yyjson_doc_get_root(d) : NULL;
        tny_workspace_op op = tool_workspace_op(argv[4]);
        const char *why = NULL;
        char *detail = tny_workspace_detail(&ctx, op, args, &why);
        perm_engine *perm = perm_new(&ctx);
        if (getenv("WS_TEST_READ_GRANT")) {
            ctx.perm_mode = TNY_MODE_ASK;
            char *read_detail = tny_workspace_detail(&ctx, TNY_WORKSPACE_INSPECT, args, NULL);
            perm_grant(perm, "job_workspace_inspect", read_detail);
            free(read_detail);
        }
        int rc = 2;
        buf_t out = {0};
        if (detail && perm && perm_check(perm, tny_workspace_permission_tool(op), detail) == PERM_ALLOW) {
            tools_env env = {0}; env.ctx = &ctx;
            env.session_id = "0123456789abcdef"; /* captured runtime identity in this test driver */
            rc = tool_workspace_run(&env, op, args, &out, err, sizeof err);
        }
        if (out.len) fwrite(out.data, 1, out.len, stdout);
        if (why || err[0]) fprintf(stderr, "%s\n", why ? why : err);
        free(detail); perm_free(perm); buf_free(&out);
        if (d) yyjson_doc_free(d);
        return rc;
    }
    cli_globals g = {0};
    return cmd_task_workspace(&ctx, &g, argc - 3, argv + 3);
}
"""


class WorkspaceControl(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="tny-workspace-control-driver-")
        cls.addClassCleanup(cls.build.cleanup)
        work = Path(cls.build.name)
        source = work / "driver.c"
        source.write_text(DRIVER)
        obj = work / "driver.o"
        cls.driver = work / "driver"
        object_root = ROOT / os.environ.get("TNY_WORKSPACE_TEST_OBJECTS", "build/rel")
        objects = sorted((object_root / "src").rglob("*.o"))
        objects += sorted((object_root / "third_party").rglob("*.o"))
        objects = [p for p in objects if p != object_root / "src/main.o"]
        if not (object_root / "src/core/tools_workspace.o").exists():
            raise RuntimeError(
                "build the selected release/debug objects before this fixture"
            )
        subprocess.run(
            shlex.split(os.environ.get("CC", "cc"))
            + [
                "-std=c11",
                "-D_DEFAULT_SOURCE",
                "-D_DARWIN_C_SOURCE",
                "-Iinclude",
                "-Isrc",
                "-Ithird_party/yyjson",
                "-c",
                str(source),
                "-o",
                str(obj),
            ],
            cwd=ROOT,
            check=True,
            capture_output=True,
        )
        flags = (
            ["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-pthread", "-ldl"]
        )
        if os.environ.get("TNY_WORKSPACE_TEST_SANITIZE"):
            flags.append("-fsanitize=address,undefined")
        subprocess.run(
            shlex.split(os.environ.get("CXX", "c++"))
            + [str(obj), *map(str, objects), *flags, "-o", str(cls.driver)],
            cwd=ROOT,
            check=True,
            capture_output=True,
        )

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="tny-workspace-control-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.state = self.root / "state"
        self.job = self.state / "jobs" / RUN
        self.job.mkdir(parents=True, mode=0o700)
        self.state.chmod(0o700)
        self.job.parent.chmod(0o700)
        (self.job / "owner.lock").touch(mode=0o600)
        self.env = {
            k: v
            for k, v in os.environ.items()
            if not k.startswith(("GIT_", "WS_TEST_", "TNY_TEAM_", "TNY_ADMISSION_"))
            and k
            not in (
                "TNY_NESTED",
                "TNY_NESTED_MODE",
                "TNY_SESSION_ID",
                "TNY_SESSION_SOCK",
            )
        }
        self.env["GIT_CONFIG_NOSYSTEM"] = "1"
        self.env["HOME"] = str(self.root)
        self.git(self.repo, "init", "-b", "main")
        self.git(self.repo, "config", "user.name", "Fixture")
        self.git(self.repo, "config", "user.email", "fixture@example.invalid")
        (self.repo / "same.txt").write_text("base\n")
        self.git(self.repo, "add", "same.txt")
        self.git(self.repo, "commit", "-m", "base")
        self.record = {
            "version": 1,
            "kind": "job",
            "id": RUN,
            "run_id": RUN,
            "job_kind": "ask",
            "dag": True,
            "state": "succeeded",
            "attempt": 1,
            "revision": 3,
            "workspace": str(self.repo),
            "cleanup": "complete",
            "cleanup_hold": False,
            "verification": "unverified",
            "parent_session_id": "0123456789abcdef",
            "items": [self.item(0), self.item(1)],
            "secret_fixture": "RECORD_SECRET_MUST_NOT_LEAK",
        }
        for index, item in enumerate(self.record["items"]):
            item["log_path"] = str(self.job / f"attempt-1-item-{index}.log")
        self.save()
        self.paths = []
        for index in range(2):
            result = self.call("_fixture_prepare", RUN, str(index), "1")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.paths.append(Path(result.stdout.strip()))

    @staticmethod
    def item(index):
        return {
            "index": index,
            "task_id": index,
            "attempt": 1,
            "state": "succeeded",
            "verification": "unverified",
            "request": {
                "prompt": "PROMPT_SECRET_MUST_NOT_LEAK",
                "workspace": {"policy": "isolated"},
            },
        }

    def save(self):
        path = self.job / "job.json"
        path.write_text(json.dumps(self.record))
        path.chmod(0o600)

    def git(self, path, *args):
        return subprocess.run(
            ["git", "-C", str(path), *args],
            env=self.env,
            text=True,
            capture_output=True,
            check=True,
            timeout=20,
        ).stdout.strip()

    def call(self, *args, cwd=None, extra_env=None):
        result = subprocess.run(
            [str(self.driver), str(self.state), str(cwd or self.repo), *args],
            env={**self.env, **(extra_env or {})},
            text=True,
            capture_output=True,
            timeout=30,
        )
        self.assertNotIn("SECRET_MUST_NOT_LEAK", result.stdout + result.stderr)
        return result

    def control(self, action, task=0, attempt=1, **kwargs):
        return self.call(
            action,
            "--run",
            RUN,
            "--task",
            str(task),
            "--attempt",
            str(attempt),
            "--json",
            **kwargs,
        )

    def test_nested_caller_cannot_claim_operator_authority(self):
        before = (self.paths[0] / "same.txt").read_bytes()
        denied = self.control("inspect", extra_env={"TNY_NESTED": "1"})
        self.assertNotEqual(denied.returncode, 0)
        self.assertIn("caller", denied.stderr)
        self.assertEqual((self.paths[0] / "same.txt").read_bytes(), before)
        self.record["parent_session_id"] = "fedcba9876543210"
        self.save()
        denied = self.call(
            "api",
            "job_workspace_inspect",
            json.dumps({"run": RUN, "task": 0, "attempt": 1}),
        )
        self.assertNotEqual(
            denied.returncode, 0, "an unrelated captured session became the parent"
        )

    def test_inspect_artifact_and_cli_api_parity(self):
        (self.paths[0] / "same.txt").write_text("worker\n")
        cli = self.control("inspect")
        self.assertEqual(cli.returncode, 0, cli.stderr)
        result = json.loads(cli.stdout)
        self.assertEqual(result["verification"], "unverified")
        self.assertFalse(result["accepted"])
        self.assertTrue(result["dirty"])
        self.assertIn("+worker", result["patch"])
        self.assertEqual(result["path"], str(self.paths[0]))
        api = self.call(
            "api",
            "job_workspace_inspect",
            json.dumps({"run": RUN, "task": 0, "attempt": 1}),
        )
        self.assertEqual(api.returncode, 0, api.stderr)
        self.assertEqual(json.loads(api.stdout), result)
        self.assertEqual(self.git(self.repo, "status", "--porcelain"), "")

    def test_explicit_integration_conflict_and_cleanup(self):
        for index, path in enumerate(self.paths):
            (path / "same.txt").write_text(f"worker {index}\n")
            self.git(path, "commit", "-am", f"worker {index}")
        first = self.control("integrate")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(json.loads(first.stdout)["status"], "integrated")
        self.git(self.repo, "commit", "-m", "accept first after explicit review")
        second = self.control("integrate", task=1)
        self.assertEqual(second.returncode, 2, second.stderr)
        result = json.loads(second.stdout)
        self.assertEqual(result["status"], "conflict")
        self.assertTrue(result["conflict"])
        self.assertFalse(result["accepted"])
        self.assertEqual(result["verification"], "unverified")
        self.assertTrue(self.git(self.repo, "ls-files", "--unmerged"))
        for index in range(2):
            inspected = self.control("inspect", task=index)
            self.assertEqual(inspected.returncode, 0, inspected.stderr)
            self.assertIn(f"+worker {index}", json.loads(inspected.stdout)["patch"])
        for _ in range(2):
            removed = self.control("cleanup")
            self.assertEqual(removed.returncode, 0, removed.stderr)
            self.assertEqual(json.loads(removed.stdout)["status"], "removed")
        self.assertFalse(self.paths[0].exists())

    def test_refuse_active_unknown_cleanup_and_stale_attempt_before_git(self):
        cases = [
            ("state", "running"),
            ("cleanup", "unknown"),
            ("cleanup", "pending"),
            ("cleanup_hold", True),
            ("cleanup_hold", "false"),
            ("attempt", 2),
            ("attempt", 1.5),
            ("dag", False),
            ("run_id", "a" * 32),
        ]
        for key, value in cases:
            with self.subTest(key=key, value=value):
                old = self.record[key]
                self.record[key] = value
                self.save()
                self.assertNotEqual(self.control("cleanup").returncode, 0)
                self.assertTrue(self.paths[0].is_dir())
                self.record[key] = old
        self.record["items"][1]["state"] = "running"
        self.save()
        self.assertNotEqual(self.control("inspect").returncode, 0)
        self.record["items"][1]["state"] = "succeeded"
        self.record["items"][0]["attempt"] = 2
        self.save()
        self.assertNotEqual(self.control("inspect").returncode, 0)
        self.assertFalse((self.paths[0].parent / "result.json").exists())

    def test_policy_and_launch_cwd_binding(self):
        other = self.root / "other"
        other.mkdir()
        self.assertEqual(self.control("inspect", cwd=other).returncode, 0)
        for op in ("integrate", "cleanup"):
            self.assertNotEqual(self.control(op, cwd=other).returncode, 0)
        self.record["items"][0]["request"]["workspace"]["policy"] = "shared"
        self.save()
        self.assertNotEqual(self.control("inspect").returncode, 0)
        del self.record["items"][0]["request"]["workspace"]
        self.save()
        self.assertNotEqual(self.control("inspect").returncode, 0)
        self.record["items"][0]["workspace"] = {"policy": "isolated"}
        self.save()
        self.assertEqual(self.control("inspect").returncode, 0)
        self.record["items"][0]["workspace"] = {"policy": "shared"}
        self.save()
        self.assertNotEqual(self.control("inspect").returncode, 0)

    def test_job_association_cannot_redirect_to_another_linked_checkout(self):
        self.record["workspace"] = str(self.paths[1])
        self.save()
        for op in ("inspect", "integrate", "cleanup"):
            result = self.control(op, cwd=self.paths[1])
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("provenance", result.stderr)
        self.assertTrue(self.paths[0].is_dir())

    def test_refusals_do_not_invoke_git(self):
        marker = self.root / "git-called"
        bindir = self.root / "deny-bin"
        bindir.mkdir()
        script = bindir / "git"
        script.write_text(f"#!/bin/sh\n: > {shlex.quote(str(marker))}\nexit 97\n")
        script.chmod(0o700)
        env = {"PATH": f"{bindir}:{self.env['PATH']}"}
        for state, cleanup in (("running", "complete"), ("succeeded", "unknown")):
            self.record["state"] = state
            self.record["cleanup"] = cleanup
            self.save()
            self.assertNotEqual(self.control("inspect", extra_env=env).returncode, 0)
        self.record["state"] = "succeeded"
        self.record["cleanup"] = "complete"
        self.save()
        self.assertNotEqual(
            self.control("inspect", attempt=2, extra_env=env).returncode, 0
        )
        self.assertNotEqual(
            self.control("cleanup", cwd=self.paths[1], extra_env=env).returncode, 0
        )
        for flag in ("WS_TEST_SSH", "WS_TEST_EMBEDDED"):
            self.assertNotEqual(
                self.control("cleanup", extra_env={**env, flag: "1"}).returncode, 0
            )
        self.record["items"][0]["request"]["workspace"]["policy"] = "shared"
        self.save()
        self.assertNotEqual(self.control("cleanup", extra_env=env).returncode, 0)
        self.assertFalse(marker.exists())

    def test_permissions_and_unsupported_contexts(self):
        args = json.dumps({"run": RUN, "task": 0, "attempt": 1})
        for op in ("inspect", "integrate", "cleanup"):
            result = self.call(
                "api",
                f"job_workspace_{op}",
                args,
                extra_env={"WS_TEST_READ_GRANT": "1"},
            )
            self.assertEqual(
                result.returncode, 0 if op == "inspect" else 2, result.stderr
            )
        for flag in ("WS_TEST_SSH", "WS_TEST_EMBEDDED", "WS_TEST_ASK"):
            for op in ("inspect", "integrate", "cleanup"):
                self.assertNotEqual(
                    self.control(op, extra_env={flag: "1"}).returncode, 0
                )
        self.assertTrue(self.paths[0].is_dir())

    def test_existing_owner_lock_fences_operations(self):
        with (self.job / "owner.lock").open("r+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            for op in ("inspect", "integrate", "cleanup"):
                self.assertNotEqual(self.control(op).returncode, 0)
        self.assertFalse((self.paths[0].parent / "result.json").exists())
        self.assertEqual(self.control("inspect").returncode, 0)

    def test_owner_held_across_git_without_state_lock(self):
        gate = self.root / "gate"
        release = self.root / "release"
        bindir = self.root / "bin"
        bindir.mkdir()
        git = shutil.which("git", path=self.env["PATH"])
        self.assertIsNotNone(git)
        wrapper = bindir / "git"
        wrapper.write_text(
            "#!/bin/sh\n"
            f": > {shlex.quote(str(gate))}\n"
            f"while [ ! -f {shlex.quote(str(release))} ]; do sleep 0.02; done\n"
            f'exec {shlex.quote(git)} "$@"\n'
        )
        wrapper.chmod(0o700)
        child = subprocess.Popen(
            [
                str(self.driver),
                str(self.state),
                str(self.repo),
                "inspect",
                "--run",
                RUN,
                "--task",
                "0",
                "--attempt",
                "1",
                "--json",
            ],
            env={**self.env, "PATH": f"{bindir}:{self.env['PATH']}"},
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            deadline = time.monotonic() + 10
            while (
                not gate.exists()
                and child.poll() is None
                and time.monotonic() < deadline
            ):
                time.sleep(0.01)
            self.assertTrue(gate.exists(), "service did not reach Git")
            with (self.job / "owner.lock").open("r+") as lock:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            retried = self.call("_fixture_retry", json.dumps({"id": RUN}))
            self.assertNotEqual(retried.returncode, 0)
            self.assertIn("another owner holds this job", retried.stderr)
            self.assertEqual(
                json.loads((self.job / "job.json").read_text())["attempt"], 1
            )
            state_lock = self.job / "state.lock"
            state_lock.touch(mode=0o600)
            with state_lock.open("r+") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                release.touch()
                stdout, stderr = child.communicate(timeout=20)
                self.assertEqual(child.returncode, 0, stderr)
                self.assertEqual(json.loads(stdout)["status"], "inspected")
        finally:
            release.touch()
            if child.poll() is None:
                child.terminate()
            child.communicate(timeout=10)

    def test_terminal_failed_results_and_dirty_cleanup(self):
        self.record["state"] = "failed"
        self.record["items"][0]["state"] = "failed"
        self.save()
        self.assertEqual(self.control("inspect").returncode, 0)
        (self.paths[0] / "untracked").write_text("preserve cancelled work")
        for op in ("cleanup", "integrate"):
            result = self.control(op)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertFalse(json.loads(result.stdout)["accepted"])
        self.assertEqual(
            (self.paths[0] / "untracked").read_text(), "preserve cancelled work"
        )
        self.record["state"] = "cancelled"
        self.record["items"][0]["state"] = "cancelled"
        self.save()
        self.assertEqual(self.control("inspect").returncode, 0)

    def test_private_record_confinement_and_no_adoption(self):
        original = self.job / "job.json"
        foreign = self.root / "foreign.json"
        original.rename(foreign)
        original.symlink_to(foreign)
        self.assertNotEqual(self.control("cleanup").returncode, 0)
        original.unlink()
        foreign.rename(original)
        original.chmod(0o644)
        self.assertNotEqual(self.control("cleanup").returncode, 0)
        original.chmod(0o600)
        owner = self.job / "owner.lock"
        owner.unlink()
        self.assertNotEqual(self.control("cleanup").returncode, 0)
        self.assertFalse(owner.exists())
        owner.touch(mode=0o600)
        self.git(self.repo, "worktree", "remove", str(self.paths[0]))
        self.git(self.repo, "worktree", "add", "-b", "foreign", str(self.paths[0]))
        self.assertNotEqual(self.control("cleanup").returncode, 0)
        self.assertEqual(self.git(self.paths[0], "branch", "--show-current"), "foreign")

    def test_strict_public_grammar_and_no_prepare(self):
        for action in ("prepare", "merge", "remove"):
            self.assertNotEqual(self.control(action).returncode, 0)
        for tail in (
            ("--task", "-1"),
            ("--task", "1.0"),
            ("--attempt", "0"),
            ("--cwd", str(self.paths[1])),
            ("--session", RUN),
        ):
            result = self.call(
                "inspect", "--run", RUN, "--task", "0", "--attempt", "1", *tail
            )
            self.assertNotEqual(result.returncode, 0)
        for bad in (
            {"run": RUN, "task": 0.5, "attempt": 1},
            {"run": RUN, "task": 0, "attempt": 1, "cwd": str(self.paths[1])},
        ):
            self.assertNotEqual(
                self.call("api", "job_workspace_cleanup", json.dumps(bad)).returncode, 0
            )


if __name__ == "__main__":
    unittest.main(argv=argv_without_runner_binary())
