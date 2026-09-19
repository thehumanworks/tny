"""Public CLI/service team controls against the deterministic jobs provider.

Until dispatch is integrated, build a temporary driver with the real main/jobs
supervisor and the new cmd_team entry. No production dispatch/build files change.
TNY_TEAM_DRIVER can select an already-built equivalent driver. No live providers.
"""

import base64
import json
import os
import shlex
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

import test_jobs as jobs

ROOT = jobs.ROOT

# Private test adapter: environment identities are deliberately confined to this
# temporary binary. Production cmd_team never consumes them. The service sees
# an explicit caller struct exactly as a trusted tool adapter would supply it.
DRIVER = r"""
#include "cli/cli.h"
#include "core/team_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int cmd_team(tny_ctx *, const cli_globals *, int, char **);
int tny_original_main(int, char **);
#define main tny_original_main
#include "MAIN_SOURCE"
#undef main
static bool interrupt_wait(void *p) { (void)p; return true; }
int main(int argc, char **argv) {
    int ci = cli_command_index(argc, argv);
    if (ci < 0 || ci >= argc || strcmp(argv[ci], "team") != 0)
        return tny_original_main(argc, argv);
    cli_globals g = {0};
    if (cli_parse_globals(argc, argv, &g) < 0) return 1;
    tny_ctx *ctx = cli_make_ctx(&g);
    if (!ctx) return 1;
    const char *mode = getenv("TNY_TEAM_TEST_MODE");
    int rc;
    if (!mode) rc = cmd_team(ctx, &g, argc-ci-1, argv+ci+1);
    else {
        const char *session = getenv("TNY_TEAM_TEST_SESSION");
        const char *run = getenv("TNY_TEAM_TEST_RUN");
        const char *task = getenv("TNY_TEAM_TEST_TASK");
        const char *attempt = getenv("TNY_TEAM_TEST_ATTEMPT");
        tny_team_caller caller = {.local_operator = !session, .session_id = session,
            .run_id = run, .task_index = task ? atoi(task) : -1,
            .attempt = attempt ? atoi(attempt) : 0};
        if (strcmp(mode, "embedded") == 0) ctx->library_mode = true;
        if (strcmp(mode, "ephemeral") == 0) ctx->no_save = true;
        if (strcmp(mode, "ssh") == 0) ctx->ssh_host = xstrdup("fixture-remote");
        buf_t input = {0}, out = {0};
        char bytes[4096]; size_t count;
        while ((count = fread(bytes, 1, sizeof bytes, stdin)) != 0) {
            if (input.len + count > TNY_JOBS_REQUEST_MAX) return 1;
            buf_append(&input, bytes, count);
        }
        char *request = NULL; const char *error = NULL;
        tny_team_op op = tny_team_parse_argv(argc-ci-1, argv+ci+1, input.data, input.len,
                                            &request, &error);
        yyjson_doc *doc = request ? jparse(request, strlen(request)) : NULL;
        char err[320] = "";
        rc = 1;
        if (doc && strcmp(mode, "detail") == 0) {
            char *reason = NULL;
            char *detail = tny_team_detail(ctx, &caller, op, yyjson_doc_get_root(doc), &reason);
            if (detail) { puts(detail); rc = 0; }
            else if (reason) fprintf(stderr, "%s\n", reason);
            free(detail); free(reason);
        } else if (doc) {
            rc = tny_team_run(ctx, &caller, op, yyjson_doc_get_root(doc), &out, err, sizeof err,
                strcmp(mode, "interrupt") == 0 ? interrupt_wait : NULL, NULL);
            if (out.len) fwrite(out.data, 1, out.len, stdout);
            if (err[0]) fprintf(stderr, "%s\n", err);
        }
        yyjson_doc_free(doc); free(request); buf_free(&input); buf_free(&out);
    }
    free(g.add_dirs); free(g.agent_argv); tny_ctx_free(ctx);
    return rc;
}
"""


def build_driver(directory):
    subprocess.run(["make", "release"], cwd=ROOT, check=True, capture_output=True)
    plan = subprocess.run(
        ["make", "-n", "-B", "release"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.splitlines()
    compile_cmd = shlex.split(
        next(
            line
            for line in plan
            if " -o build/rel/src/main.o " in line and " -c " in line
        )
    )
    link_cmd = shlex.split(next(line for line in plan if " -o build/tny " in line))
    source = directory / "driver.c"
    source.write_text(DRIVER.replace("MAIN_SOURCE", str(ROOT / "src/main.c")))
    obj = directory / "driver.o"
    binary = directory / "tny"
    compile_cmd[compile_cmd.index("-o") + 1] = str(obj)
    compile_cmd[compile_cmd.index("src/main.c")] = str(source)
    subprocess.run(compile_cmd, cwd=ROOT, check=True, capture_output=True)
    link_cmd[link_cmd.index("-o") + 1] = str(binary)
    link_cmd[link_cmd.index("build/rel/src/main.o")] = str(obj)
    subprocess.run(link_cmd, cwd=ROOT, check=True, capture_output=True)
    return str(binary)


class TeamControlTests(jobs.JobsFixture):
    hold = 2.0

    @classmethod
    def setUpClass(cls):
        if jobs.WASM:
            raise unittest.SkipTest(
                "team provider fixture requires native process ownership"
            )
        cls.driver_tmp = tempfile.TemporaryDirectory(prefix="tny-team-driver-")
        cls.original_tny = jobs.TNY
        jobs.TNY = os.environ.get("TNY_TEAM_DRIVER") or build_driver(
            Path(cls.driver_tmp.name)
        )

    @classmethod
    def tearDownClass(cls):
        jobs.TNY = cls.original_tny
        cls.driver_tmp.cleanup()

    def setUp(self):
        super().setUp()
        self.env.pop("TNY_NESTED", None)  # explicit same-user operator fixture
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

    def specification(self, *, barrier=False, fail=False):
        return {
            "kind": "ask",
            "dag": True,
            "concurrency": 3,
            "items": [
                {"role": "lead", "label": "lead", "prompt": "read only: LEAD"},
                {
                    "role": "worker",
                    "label": "review",
                    "prompt": "read only: "
                    + ("DAG_BARRIER" if barrier else "HOLD worker-one"),
                },
                {
                    "role": "worker",
                    "label": "tests",
                    "prompt": "read only: "
                    + ("FAIL worker-two" if fail else "HOLD worker-two"),
                },
            ],
        }

    def test_async_start_overlap_cursor_and_bounded_collection(self):
        started = time.monotonic()
        launch, handle = self.team("start", self.specification(barrier=True))
        self.assertEqual(launch.returncode, 0)
        self.assertLess(time.monotonic() - started, 5)
        run = handle["run_id"]
        self.assertEqual(run, handle["job"]["id"])
        self.assertEqual(handle["verification"], "unverified")
        self.assertTrue(self.state["dag_entered"].wait(20))
        deadline = time.monotonic() + 20
        while self.state["peak"] < 2 and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertGreaterEqual(self.state["peak"], 2)
        _, status = self.team("status", {"id": run})
        self.assertNotIn(status["job"]["state"], jobs.TERMINAL)
        # Submission process is gone, but the barrier worker still lives.
        seen = []
        for _ in range(2):
            rc, event = self.team(
                "wait-any", {"id": run, "seen": seen, "timeout_ms": 10000}
            )
            self.assertEqual(rc.returncode, 0)
            self.assertNotIn(event["cursor"], seen)
            self.assertNotEqual(event["cursor"]["item"], 1)
            seen.append(event["cursor"])
        before = time.monotonic()
        timeout, event = self.team(
            "wait-any", {"id": run, "seen": seen, "timeout_ms": 120}, check=False
        )
        self.assertEqual(timeout.returncode, 124)
        self.assertLess(time.monotonic() - before, 2)
        self.assertFalse(event["cancelled"])
        self.assertFalse(self.status(run)["cancel_requested"])
        self.state["dag_release"].set()
        final = self.await_terminal(run)
        self.assertEqual(final["state"], "succeeded")
        _, event = self.team("wait-any", {"id": run, "seen": seen, "timeout_ms": 0})
        self.assertEqual(event["cursor"], {"item": 1, "attempt": 1})
        _, collected = self.team("collect", {"id": run, "item": 1, "max_bytes": 12})
        self.assertEqual(collected["result_integrity"], "matched")
        self.assertEqual(collected["result_sha256"], final["items"][1]["result_sha256"])
        self.assertEqual(len(base64.b64decode(collected["result_base64"])), 12)
        self.assertLessEqual(len(base64.b64decode(collected["log_base64"])), 12)
        self.assertEqual(collected["verification"], "unverified")
        self.assertEqual(collected["integration"], "not_recorded")

    def test_cancel_is_fenced_selected_and_unrelated_run_survives(self):
        _, first = self.team("start", self.specification(barrier=True))
        _, other = self.team("start", self.specification())
        run = first["run_id"]
        self.assertTrue(self.state["dag_entered"].wait(20))
        for request in [
            {"id": run, "item": 1},
            {"id": run, "item": 1, "expected_attempt": 2},
        ]:
            rejected, _ = self.team("cancel", request, check=False)
            self.assertEqual(rejected.returncode, 1)
            self.assertFalse(self.status(run)["items"][1]["cancel_requested"])
        self.team("cancel", {"id": run, "item": 1, "expected_attempt": 1})
        final = self.await_terminal(run)
        self.assertEqual(final["items"][1]["state"], "cancelled")
        self.assertEqual(final["items"][0]["state"], "succeeded")
        self.assertEqual(self.await_terminal(other["run_id"])["state"], "succeeded")

    def test_one_worker_failure_is_execution_not_acceptance(self):
        _, handle = self.team("start", self.specification(fail=True))
        run = handle["run_id"]
        final = self.await_terminal(run)
        self.assertEqual(final["items"][2]["state"], "failed")
        self.assertEqual(final["items"][1]["state"], "succeeded")
        rc, event = self.team("wait-any", {"id": run, "item": 2})
        self.assertEqual(rc.returncode, 0)
        self.assertEqual(event["item"]["state"], "failed")
        self.assertEqual(event["verification"], "unverified")
        rc, collected = self.team("collect", {"id": run, "item": 2}, check=False)
        self.assertEqual(rc.returncode, 2)
        self.assertEqual(collected["verification"], "unverified")

    def test_membership_comes_only_from_captured_caller(self):
        parent = "0123456789abcdef"
        env = {
            **self.env,
            "TNY_TEAM_TEST_MODE": "session",
            "TNY_TEAM_TEST_SESSION": parent,
        }
        _, handle = self.team("start", self.specification(), env=env)
        run = handle["run_id"]
        self.assertEqual(handle["job"]["parent_session_id"], parent)
        final = self.await_terminal(run)
        self.team("status", {"id": run}, env=env)
        outsider = {**env, "TNY_TEAM_TEST_SESSION": "fedcba9876543210"}
        for op, extra in [
            ("status", {}),
            ("collect", {"item": 1}),
            ("cancel", {"expected_attempt": 1}),
        ]:
            denied, _ = self.team(op, {"id": run, **extra}, check=False, env=outsider)
            self.assertEqual(denied.returncode, 1)
        worker = {
            **env,
            "TNY_TEAM_TEST_SESSION": final["items"][1]["session_id"],
            "TNY_TEAM_TEST_RUN": run,
            "TNY_TEAM_TEST_TASK": "1",
            "TNY_TEAM_TEST_ATTEMPT": "1",
        }
        self.team("collect", {"id": run, "item": 1}, env=worker)
        denied, _ = self.team(
            "collect", {"id": run, "item": 0}, check=False, env=worker
        )
        self.assertEqual(denied.returncode, 1)
        stale = {**worker, "TNY_TEAM_TEST_ATTEMPT": "2"}
        denied, _ = self.team("collect", {"id": run, "item": 1}, check=False, env=stale)
        self.assertEqual(denied.returncode, 1)
        # Neither a request session id nor a role label creates authority.
        denied, _ = self.team(
            "collect",
            {"id": run, "item": 1, "session_id": final["items"][1]["session_id"]},
            check=False,
        )
        self.assertEqual(denied.returncode, 1)

    def test_verification_refuses_without_fabricated_success_or_side_effects(self):
        _, handle = self.team("start", self.specification())
        run = handle["run_id"]
        self.await_terminal(run)
        marker = self.workspace / "MUST_NOT_RUN"
        request = {
            "id": run,
            "item": 1,
            "expected_attempt": 1,
            "command": f"touch '{marker}'; exit 0",
            "cwd": str(self.workspace),
            "timeout_ms": 1000,
        }
        env = {**self.env, "TNY_TEAM_TEST_MODE": "detail"}
        _, detail = self.team("verify", request, env=env)
        self.assertEqual(detail["operation"], "team_verify")
        self.assertEqual(json.loads(detail["request_detail"]), request)
        _, read_detail = self.team("status", {"id": run}, env=env)
        self.assertEqual(read_detail["operation"], "team_status")
        rc, result = self.team("verify", request, check=False)
        self.assertEqual(rc.returncode, 1)
        self.assertEqual(result["error_code"], "TEAM_VERIFY_UNSUPPORTED")
        self.assertEqual(result["verification"], "unverified")
        self.assertFalse(marker.exists())
        self.assertEqual(self.status(run)["verification"], "unverified")

    def test_validation_and_unsupported_contexts_have_no_launch_side_effect(self):
        spec = self.specification()
        variants = [
            {**spec, "dag": False},
            {**spec, "parent_session_id": "0123456789abcdef"},
            {**spec, "items": spec["items"][:2]},
            {**spec, "items": [dict(item, role="worker") for item in spec["items"]]},
        ]
        for request in variants:
            result, _ = self.team("start", request, check=False)
            self.assertEqual(result.returncode, 1)
        nested, _ = self.team(
            "start", spec, check=False, env={**self.env, "TNY_NESTED": "1"}
        )
        self.assertEqual(nested.returncode, 1)
        self.assertIn(b"trusted team terminal adapter", nested.stderr)
        for mode in ["ssh", "embedded", "ephemeral"]:
            result, _ = self.team(
                "start", spec, check=False, env={**self.env, "TNY_TEAM_TEST_MODE": mode}
            )
            self.assertEqual(result.returncode, 1)
        duplicate = json.dumps(spec).replace('"dag": true', '"dag": false, "dag": true')
        result = self.run_tny(
            "team", "start", "--request", "-", stdin=duplicate.encode(), check=False
        )
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.job_dirs(), [])
        self.assertEqual(self.state["requests"], [])
        # Permission inspection uses the same validator, is secret-safe, and
        # must not create a run even for a valid definition.
        _, detail = self.team(
            "start", spec, env={**self.env, "TNY_TEAM_TEST_MODE": "detail"}
        )
        self.assertEqual(detail["operation"], "team_start")
        self.assertNotIn(spec["items"][0]["prompt"], json.dumps(detail))
        self.assertNotIn(jobs.API_KEY, json.dumps(detail))
        self.assertEqual(self.job_dirs(), [])

    def test_collection_rejects_changed_artifacts_and_wait_interrupt_does_not_cancel(
        self,
    ):
        _, handle = self.team("start", self.specification())
        run = handle["run_id"]
        final = self.await_terminal(run)
        rc, _ = self.team(
            "wait-any",
            {"id": run},
            check=False,
            env={**self.env, "TNY_TEAM_TEST_MODE": "interrupt"},
        )
        self.assertEqual(rc.returncode, 130)
        self.assertFalse(self.status(run)["cancel_requested"])
        for op, extra in [
            ("wait-any", {"timeout_ms": 30001}),
            ("wait-any", {"seen": [{"item": 1, "attempt": 0}]}),
            ("collect", {"item": 1, "max_bytes": 0}),
            ("collect", {"item": 1, "expected_attempt": True}),
            ("collect", {"item": 1, "session_id": "0123456789abcdef"}),
        ]:
            rejected, _ = self.team(op, {"id": run, **extra}, check=False)
            self.assertEqual(rejected.returncode, 1)
        # A changed authoritative answer must not be replaced with plausible log prose.
        sid = final["items"][0]["session_id"]
        session_path = next(
            (self.home / ".tny" / "sessions").glob(f"*/{sid}/session.json")
        )
        session = json.loads(session_path.read_text())
        for message in reversed(session["messages"]):
            if message.get("role") == "assistant":
                message["content"] = "edited after completion"
                break
        session_path.write_text(json.dumps(session))
        changed, data = self.team("collect", {"id": run, "item": 0}, check=False)
        self.assertEqual(changed.returncode, 2)
        self.assertEqual(data["result_integrity"], "unavailable_or_changed")
        self.assertEqual(data["result_base64"], "")
        self.assertEqual(data["verification"], "unverified")
        Path(final["items"][1]["log_path"]).write_text("changed")
        rc, _ = self.team("collect", {"id": run, "item": 1}, check=False)
        self.assertEqual(rc.returncode, 1)
        self.assertIn(b"integrity changed", rc.stderr)


def load_tests(loader, tests, pattern):
    # Reuse fixture methods, not the unrelated jobs suite's inherited test cases.
    return unittest.TestSuite(
        TeamControlTests(name)
        for name in sorted(TeamControlTests.__dict__)
        if name.startswith("test_")
    )


if __name__ == "__main__":
    unittest.main(argv=jobs.argv_without_runner_binary())
