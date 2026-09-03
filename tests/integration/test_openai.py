#!/usr/bin/env python3
"""End-to-end: tny ask --json against the mock OpenAI provider.

Covers the full native loop on the default Responses API wire (the mock
400s anything that hits /chat/completions): typed SSE streaming split at
arbitrary byte boundaries, fragmented function_call assembly, tool
execution (list_files), second POST with the echoed function_call +
function_call_output items, session persistence + resume. A second block
re-runs the loop on the legacy chat wire via OPENAI_WIRE_API=chat,
--wire-api chat, and the settings wire_api key (docs/adr/0016), and a
third exercises the response.failed error path.
"""

import glob
import json
import os
import shlex
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TNY = os.environ.get("TNY", os.path.join(ROOT, "build", "tny"))
MOCK = os.path.join(ROOT, "tests", "integration", "mock_openai.py")
IS_WASM = "/wasm/" in TNY.replace("\\", "/")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def check_tool_profile_wire(base_env, ws, wire):
    """Pin schema, enforcement, and prompt behavior on one OpenAI wire."""
    for profile in ("terminal", "terminal+edit", "all"):
        port = free_port()
        mock_env = dict(os.environ, MOCK_EXPECT_WIRE=wire)
        if profile in ("terminal", "terminal+edit") and not IS_WASM:
            names = "terminal,read_image"
            instructions = "Shell tool profile\ntny edit FILE"
            if profile == "terminal+edit":
                names += ",edit_file"
                instructions = "Shell tool profile\nedit_file"
            mock_env.update(
                MOCK_EXPECT_TOOL_NAMES=names,
                MOCK_EXPECT_INSTRUCTIONS=instructions,
                MOCK_CUSTOM_TOOL="read_file",
                MOCK_CUSTOM_ARGUMENTS='{"path":"a.txt"}',
                MOCK_EXPECT_TOOL_OUTPUT="error: unknown tool read_file",
            )
        else:
            mock_env["MOCK_REJECT_INSTRUCTIONS"] = "Shell tool profile"
        mock = subprocess.Popen(
            [sys.executable, MOCK, str(port)],
            env=mock_env,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        try:
            line = mock.stdout.readline().decode()
            assert "ready" in line, (
                f"{wire} {profile} profile mock did not start: {line!r}"
            )
            run_env = dict(
                base_env,
                TNY_TOOLS=profile,
                OPENAI_BASE_URL=f"http://127.0.0.1:{port}/v1",
            )
            if wire == "chat":
                run_env["OPENAI_WIRE_API"] = "chat"
            result = subprocess.run(
                [
                    TNY,
                    "--cwd",
                    ws,
                    "ask",
                    "--json",
                    "--no-save",
                    "inspect the workspace",
                ],
                env=run_env,
                capture_output=True,
                timeout=30,
            )
            assert result.returncode == 0, (
                f"{wire} {profile} profile exit {result.returncode}: {result.stderr.decode()}"
            )
            assert "MOCK-OK" in json.loads(result.stdout)["output"], result.stdout
        finally:
            mock.terminate()
            mock.wait(timeout=5)


def check_shell_profile_result_file(base_env, ws):
    if IS_WASM:
        return
    port = free_port()
    command = "i=0; while [ $i -lt 9000 ]; do printf x; i=$((i+1)); done; exit 7"
    mock = subprocess.Popen(
        [sys.executable, MOCK, str(port)],
        env=dict(
            os.environ,
            MOCK_EXPECT_WIRE="responses",
            MOCK_EXPECT_TOOL_NAMES="terminal,read_image",
            MOCK_EXPECT_INSTRUCTIONS="Shell tool profile",
            MOCK_CUSTOM_TOOL="terminal",
            MOCK_CUSTOM_ARGUMENTS=json.dumps({"command": command}),
            MOCK_EXPECT_SHELL_RESULT="1",
        ),
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    try:
        line = mock.stdout.readline().decode()
        assert "ready" in line, f"shell-result mock did not start: {line!r}"
        run_env = dict(
            base_env,
            TNY_TOOLS="terminal",
            OPENAI_BASE_URL=f"http://127.0.0.1:{port}/v1",
        )
        result = subprocess.run(
            [TNY, "--cwd", ws, "ask", "--json", "produce a large result"],
            env=run_env,
            capture_output=True,
            timeout=30,
        )
        assert result.returncode == 0, result.stderr.decode()
        assert "MOCK-OK" in json.loads(result.stdout)["output"], result.stdout
    finally:
        mock.terminate()
        mock.wait(timeout=5)


def main():
    port = free_port()
    # the default wire must be the Responses API: any request that falls
    # back to /chat/completions makes this mock 400 and the run fail
    mock = subprocess.Popen(
        [sys.executable, MOCK, str(port)],
        env=dict(os.environ, MOCK_EXPECT_WIRE="responses"),
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    try:
        line = mock.stdout.readline().decode()
        assert "ready" in line, f"mock did not start: {line!r}"

        with tempfile.TemporaryDirectory() as home:
            ws = os.path.join(home, "ws")
            os.makedirs(ws)
            for name in ("a.txt", "b.txt", "c.txt"):
                open(os.path.join(ws, name), "w").write("x\n")

            env = dict(
                os.environ,
                HOME=home,
                OPENAI_BASE_URL=f"http://127.0.0.1:{port}/v1",
                OPENAI_API_KEY="test-key-not-real",
            )

            check_tool_profile_wire(env, ws, "responses")
            check_tool_profile_wire(env, ws, "chat")
            check_shell_profile_result_file(env, ws)

            task_dir = os.path.join(ws, ".tny", "tasks")
            os.makedirs(task_dir)
            open(os.path.join(task_dir, "alpha.md"), "w").write(
                "---\nname: alpha\ndescription: Alpha project task\n---\n\nAlpha body.\n"
            )
            listed = subprocess.run(
                [TNY, "--cwd", ws, "tasks", "--json"],
                env=env,
                capture_output=True,
                timeout=10,
            )
            assert listed.returncode == 0, listed.stderr.decode()
            task_items = json.loads(listed.stdout)["tasks"]
            assert [item["name"] for item in task_items] == sorted(
                item["name"] for item in task_items
            )
            alpha = next(item for item in task_items if item["name"] == "alpha")
            assert alpha == {
                "name": "alpha",
                "source": "project",
                "description": "Alpha project task",
                "valid": True,
            }, alpha
            shown = subprocess.run(
                [TNY, "--cwd", ws, "task", "show", "alpha", "--json"],
                env=env,
                capture_output=True,
                timeout=10,
            )
            assert shown.returncode == 0, shown.stderr.decode()
            shown_task = json.loads(shown.stdout)
            assert shown_task["kind"] == "task", shown_task
            assert shown_task["name"] == "alpha", shown_task
            assert shown_task["source"] == "project", shown_task
            assert shown_task["description"] == "Alpha project task", shown_task
            assert shown_task["instructions"] == "Alpha body.\n", shown_task
            assert len(shown_task["digest"]) == 40, shown_task
            unknown = subprocess.run(
                [TNY, "--cwd", ws, "task", "show", "not-present"],
                env=env,
                capture_output=True,
                timeout=10,
            )
            assert unknown.returncode == 1, unknown
            assert b"unknown task 'not-present'" in unknown.stderr, unknown.stderr
            extra = subprocess.run(
                [TNY, "--cwd", ws, "tasks", "unexpected"],
                env=env,
                capture_output=True,
                timeout=10,
            )
            assert extra.returncode == 1, extra
            assert b"Usage: tny [--json] tasks" in extra.stderr, extra.stderr
            os.unlink(os.path.join(task_dir, "alpha.md"))
            os.rmdir(task_dir)
            os.rmdir(os.path.join(ws, ".tny"))

            # single tool call: one call streamed fragmented, one tool
            # message back, one follow-up POST (the mock 400s any request
            # whose assistant tool_calls are not fully paired)
            r = subprocess.run(
                [TNY, "--cwd", ws, "ask", "--json", "list files in ."],
                env=env,
                capture_output=True,
                timeout=30,
            )
            assert r.returncode == 0, f"exit {r.returncode}: {r.stderr.decode()}"
            out = json.loads(r.stdout)
            assert "MOCK-OK" in out["output"], out
            assert "tier=unset" in out["output"], out  # no --fast: omit the field
            assert out["exit_code"] == 0
            assert out["steps"] == 2, out
            # the mock streams TWO parallel function calls; both must be
            # assembled (fragmented arguments, late call_id/name on the
            # second) and both must succeed
            assert len(out["tool_calls"]) == 2, out
            assert out["tool_calls"][0]["name"] == "list_files", out
            assert out["tool_calls"][1]["name"] == "glob_files", out
            assert all(t["status"] == "success" for t in out["tool_calls"]), out
            sid = out["session_id"]
            assert sid, out

            # resume must find the saved session and run another turn
            r2 = subprocess.run(
                [TNY, "--cwd", ws, "ask", "--json", "--resume", sid, "again"],
                env=env,
                capture_output=True,
                timeout=30,
            )
            assert r2.returncode == 0, f"exit {r2.returncode}: {r2.stderr.decode()}"
            out2 = json.loads(r2.stdout)
            assert out2["session_id"] == sid, out2
            assert "MOCK-OK" in out2["output"], out2

            # Task selection is a persisted session snapshot. Resume without
            # a selector restores it; an explicit selector must match both
            # name and digest, and an old taskless session rejects grafting.
            task_run = subprocess.run(
                [TNY, "--cwd", ws, "ask", "--json", "--task", "review", "task turn"],
                env=env,
                capture_output=True,
                timeout=30,
            )
            assert task_run.returncode == 0, task_run.stderr.decode()
            task_out = json.loads(task_run.stdout)
            task_sid = task_out["session_id"]
            assert task_out["task"]["name"] == "review", task_out
            assert task_out["task"]["source"] == "builtin", task_out
            session_dirs = glob.glob(
                os.path.join(home, ".tny", "sessions", "*", task_sid)
            )
            assert len(session_dirs) == 1, session_dirs
            stored = json.load(open(os.path.join(session_dirs[0], "session.json")))
            assert set(stored["task"]) == {"name", "source", "digest"}, stored["task"]
            assert "rigorous code reviewer" not in json.dumps(stored), stored
            assert (
                "rigorous code reviewer"
                in open(os.path.join(session_dirs[0], "task.md")).read()
            )
            listed_sessions = subprocess.run(
                [TNY, "--cwd", ws, "sessions", "--json"],
                env=env,
                capture_output=True,
                timeout=10,
            )
            assert listed_sessions.returncode == 0, listed_sessions.stderr.decode()
            listed_task = next(
                item["task"]
                for item in json.loads(listed_sessions.stdout)["sessions"]
                if item["id"] == task_sid
            )
            assert listed_task == task_out["task"], listed_task

            restored = subprocess.run(
                [
                    TNY,
                    "--cwd",
                    ws,
                    "ask",
                    "--json",
                    "--resume",
                    task_sid,
                    "resume task",
                ],
                env=env,
                capture_output=True,
                timeout=30,
            )
            assert restored.returncode == 0, restored.stderr.decode()
            assert json.loads(restored.stdout)["task"] == task_out["task"]

            matching = subprocess.run(
                [
                    TNY,
                    "--cwd",
                    ws,
                    "ask",
                    "--json",
                    "--resume",
                    task_sid,
                    "--task",
                    "review",
                    "matching task",
                ],
                env=env,
                capture_output=True,
                timeout=30,
            )
            assert matching.returncode == 0, matching.stderr.decode()

            mismatch = subprocess.run(
                [
                    TNY,
                    "--cwd",
                    ws,
                    "ask",
                    "--resume",
                    task_sid,
                    "--task",
                    "optimizer",
                    "must reject",
                ],
                env=env,
                capture_output=True,
                timeout=10,
            )
            assert mismatch.returncode == 1, mismatch
            assert b"name and digest must match" in mismatch.stderr, mismatch.stderr

            graft = subprocess.run(
                [
                    TNY,
                    "--cwd",
                    ws,
                    "--task",
                    "review",
                    "ask",
                    "--resume",
                    sid,
                    "must reject",
                ],
                env=env,
                capture_output=True,
                timeout=10,
            )
            assert graft.returncode == 1, graft
            assert b"cannot be added after turns exist" in graft.stderr, graft.stderr

            # sessions list must show it
            r3 = subprocess.run(
                [TNY, "--cwd", ws, "sessions", "--json"],
                env=env,
                capture_output=True,
                timeout=10,
            )
            assert r3.returncode == 0, r3.stderr.decode()
            sessions = json.loads(r3.stdout)
            ids = [
                s["id"]
                for s in (
                    sessions
                    if isinstance(sessions, list)
                    else sessions.get("sessions", [])
                )
            ]
            assert sid in ids, (sid, r3.stdout.decode())

            # the API key must never leak into output
            for blob in (r.stdout, r.stderr, r2.stdout, r2.stderr):
                assert b"test-key-not-real" not in blob, "api key leaked"

            # wasm has no forked runner and no terminal child (ADR 0017), so the
            # socket-bound attach cannot happen there; the profile tests above
            # already cover the wasm clean-error path.
            if not IS_WASM:
                # A terminal child receives the resolved runner socket and can
                # queue an image while the terminal tool is still blocking. The
                # mock rejects the next POST unless it carries an input_image.
                image_path = os.path.join(ws, "runner-image.png")
                open(image_path, "wb").write(b"\x89PNG\r\n\x1a\n\x00\x00\x00\x00")
                iport = free_port()
                imock = subprocess.Popen(
                    [sys.executable, MOCK, str(iport)],
                    env=dict(
                        os.environ,
                        MOCK_EXPECT_WIRE="responses",
                        MOCK_CUSTOM_TOOL="terminal",
                        MOCK_CUSTOM_ARGUMENTS=json.dumps(
                            {
                                "command": f"{shlex.quote(os.path.abspath(TNY))} image attach "
                                "runner-image.png"
                            }
                        ),
                        MOCK_EXPECT_ATTACHED_IMAGE="1",
                    ),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                )
                try:
                    line = imock.stdout.readline().decode()
                    assert "ready" in line, f"image mock did not start: {line!r}"
                    ienv = dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{iport}/v1")
                    attached = subprocess.run(
                        [TNY, "--cwd", ws, "ask", "--json", "attach the image"],
                        env=ienv,
                        capture_output=True,
                        timeout=30,
                    )
                    assert attached.returncode == 0, attached.stderr.decode()
                    attached_out = json.loads(attached.stdout)
                    assert attached_out["tool_calls"][0]["name"] == "terminal", (
                        attached_out
                    )
                    assert "MOCK-OK" in attached_out["output"], attached_out
                finally:
                    imock.terminate()
                    imock.wait(timeout=5)
                os.unlink(image_path)

            # Native permissions can now park the OpenAI loop and resume from
            # respond_permission, like host backends. Ask mode denies the
            # sensitive write without hanging; yolo permits the same call.
            pport = free_port()
            pmock = subprocess.Popen(
                [sys.executable, MOCK, str(pport)],
                env=dict(os.environ, MOCK_SENSITIVE="1", MOCK_EXPECT_WIRE="responses"),
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            try:
                line = pmock.stdout.readline().decode()
                assert "ready" in line, f"permission mock did not start: {line!r}"
                penv = dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{pport}/v1")
                target = os.path.join(ws, "permission.txt")
                denied = subprocess.run(
                    [
                        TNY,
                        "--permission-mode",
                        "ask",
                        "--cwd",
                        ws,
                        "ask",
                        "--json",
                        "--no-save",
                        "write the marker",
                    ],
                    env=penv,
                    capture_output=True,
                    timeout=30,
                )
                assert denied.returncode == 2, denied.stderr.decode()
                denied_out = json.loads(denied.stdout)
                assert denied_out["exit_code"] == 2, denied_out
                assert denied_out["tool_calls"] == [
                    {"name": "write_file", "status": "error"}
                ], denied_out
                assert not os.path.exists(target), "ask mode executed a denied write"

                allowed = subprocess.run(
                    [
                        TNY,
                        "--yolo",
                        "--cwd",
                        ws,
                        "ask",
                        "--json",
                        "--no-save",
                        "write the marker",
                    ],
                    env=penv,
                    capture_output=True,
                    timeout=30,
                )
                assert allowed.returncode == 0, allowed.stderr.decode()
                allowed_out = json.loads(allowed.stdout)
                assert allowed_out["tool_calls"] == [
                    {"name": "write_file", "status": "success"}
                ], allowed_out
                assert open(target).read() == "allowed"
                os.unlink(target)
            finally:
                pmock.terminate()
                pmock.wait(timeout=5)

            # --system-prompt maps onto the provider's native instructions
            # field and leads the operational preamble (docs/adr/0045)
            sport = free_port()
            smock = subprocess.Popen(
                [sys.executable, MOCK, str(sport)],
                env=dict(
                    os.environ,
                    MOCK_EXPECT_INSTRUCTIONS="Answer like a pirate named SYSMARK.",
                ),
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            try:
                line = smock.stdout.readline().decode()
                assert "ready" in line, f"system-prompt mock did not start: {line!r}"
                senv = dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{sport}/v1")
                rs = subprocess.run(
                    [
                        TNY,
                        "--system-prompt",
                        "Answer like a pirate named SYSMARK.",
                        "--cwd",
                        ws,
                        "ask",
                        "--json",
                        "--no-save",
                        "list files in .",
                    ],
                    env=senv,
                    capture_output=True,
                    timeout=30,
                )
                assert rs.returncode == 0, rs.stderr.decode()
                assert "MOCK-OK" in json.loads(rs.stdout)["output"], rs.stdout
            finally:
                smock.terminate()
                smock.wait(timeout=5)

            # --task rides the provider's native instructions field too: the
            # full preset body must reach the model in the system prompt
            # (docs/adr/0048) alongside any --system-prompt addition — the
            # agent never has to discover or read the preset file itself
            tport = free_port()
            tmock = subprocess.Popen(
                [sys.executable, MOCK, str(tport)],
                env=dict(
                    os.environ,
                    MOCK_EXPECT_INSTRUCTIONS="# Task preset: review\n"
                    "rigorous code reviewer\n"
                    "SYSMARK-TASK-COMBO",
                ),
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            try:
                line = tmock.stdout.readline().decode()
                assert "ready" in line, f"task mock did not start: {line!r}"
                tenv = dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{tport}/v1")
                rt = subprocess.run(
                    [
                        TNY,
                        "--task",
                        "review",
                        "--system-prompt",
                        "SYSMARK-TASK-COMBO",
                        "--cwd",
                        ws,
                        "ask",
                        "--json",
                        "--no-save",
                        "list files in .",
                    ],
                    env=tenv,
                    capture_output=True,
                    timeout=30,
                )
                assert rt.returncode == 0, rt.stderr.decode()
                assert "MOCK-OK" in json.loads(rt.stdout)["output"], rt.stdout
            finally:
                tmock.terminate()
                tmock.wait(timeout=5)

            # A model that opens its answer with blank lines: plain `tny ask`
            # starts printing at the first visible byte, on both the isolated
            # runner path and the in-process one; --json keeps the raw text.
            wport = free_port()
            wmock = subprocess.Popen(
                [sys.executable, MOCK, str(wport)],
                env=dict(os.environ, MOCK_LEADING_WS="1", MOCK_EXPECT_WIRE="responses"),
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            try:
                line = wmock.stdout.readline().decode()
                assert "ready" in line, f"whitespace mock did not start: {line!r}"
                wenv = dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{wport}/v1")
                for isolate in ("1", "0"):
                    plain = subprocess.run(
                        [TNY, "--cwd", ws, "ask", "--no-save", "list files in ."],
                        env=dict(wenv, TNY_ISOLATE=isolate),
                        capture_output=True,
                        timeout=30,
                    )
                    assert plain.returncode == 0, plain.stderr.decode()
                    assert plain.stdout.startswith(b"The workspace"), (
                        isolate,
                        plain.stdout,
                    )
                    assert plain.stdout.endswith(b"\n"), plain.stdout
                raw = subprocess.run(
                    [TNY, "--cwd", ws, "ask", "--json", "--no-save", "list files in ."],
                    env=wenv,
                    capture_output=True,
                    timeout=30,
                )
                assert raw.returncode == 0, raw.stderr.decode()
                raw_out = json.loads(raw.stdout)["output"]
                assert raw_out.startswith("\n" * 9 + "The workspace"), raw_out
            finally:
                wmock.terminate()
                wmock.wait(timeout=5)

            # piped stdin: the connect/stdin overlap path must still run a
            # full turn end-to-end
            r4 = subprocess.run(
                [TNY, "--cwd", ws, "ask", "--json"],
                input=b"list files in .",
                env=env,
                capture_output=True,
                timeout=30,
            )
            assert r4.returncode == 0, f"exit {r4.returncode}: {r4.stderr.decode()}"
            out4 = json.loads(r4.stdout)
            assert "MOCK-OK" in out4["output"], out4
            assert out4["steps"] == 2, out4

            # --fast (TNY_CAP_FAST) must ride the request as the paid tier
            # ("priority", the pre-rename spelling every provider accepts)
            r6 = subprocess.run(
                [
                    TNY,
                    "--cwd",
                    ws,
                    "--fast",
                    "ask",
                    "--json",
                    "--no-save",
                    "list files in .",
                ],
                env=env,
                capture_output=True,
                timeout=30,
            )
            assert r6.returncode == 0, f"exit {r6.returncode}: {r6.stderr.decode()}"
            out6 = json.loads(r6.stdout)
            assert "tier=priority" in out6["output"], out6

            # --fast on a provider without the capability is a startup error
            r7 = subprocess.run(
                [TNY, "--cwd", ws, "--provider", "acp", "--fast", "ask", "hi"],
                env=env,
                capture_output=True,
                timeout=15,
            )
            assert r7.returncode == 1, f"exit {r7.returncode}: {r7.stderr.decode()}"
            assert b"--fast is not supported" in r7.stderr, r7.stderr

            # step limit (docs/adr/0024): unlimited by default (the 2-step
            # runs above pass with no cap configured); --max-steps 1 stops
            # the turn after the first model call
            r_cap = subprocess.run(
                [
                    TNY,
                    "--cwd",
                    ws,
                    "--max-steps",
                    "1",
                    "ask",
                    "--json",
                    "--no-save",
                    "list files in .",
                ],
                env=env,
                capture_output=True,
                timeout=30,
            )
            assert r_cap.returncode == 2, (
                f"exit {r_cap.returncode}: {r_cap.stderr.decode()}"
            )
            assert b"step limit reached" in r_cap.stderr, r_cap.stderr
            cap_out = json.loads(r_cap.stdout)
            assert cap_out["steps"] == 1, cap_out  # one model call was made

            # a .tny.json "steps" repo limit still caps the loop, and the
            # explicit flag beats it (--max-steps unlimited clears the cap)
            repo_cfg = os.path.join(ws, ".tny.json")
            open(repo_cfg, "w").write('{"steps": 1}')
            try:
                r_repo = subprocess.run(
                    [TNY, "--cwd", ws, "ask", "--json", "--no-save", "list files in ."],
                    env=env,
                    capture_output=True,
                    timeout=30,
                )
                assert r_repo.returncode == 2, r_repo.stderr.decode()
                assert b"step limit reached" in r_repo.stderr, r_repo.stderr
                r_uncap = subprocess.run(
                    [
                        TNY,
                        "--cwd",
                        ws,
                        "--max-steps",
                        "unlimited",
                        "ask",
                        "--json",
                        "--no-save",
                        "list files in .",
                    ],
                    env=env,
                    capture_output=True,
                    timeout=30,
                )
                assert r_uncap.returncode == 0, r_uncap.stderr.decode()
                assert json.loads(r_uncap.stdout)["steps"] == 2, r_uncap.stdout
            finally:
                os.unlink(repo_cfg)

            # a bad value is a startup error with a usable example
            r_bad = subprocess.run(
                [TNY, "--cwd", ws, "--max-steps", "lots", "ask", "hi"],
                env=env,
                capture_output=True,
                timeout=15,
            )
            assert r_bad.returncode == 1, r_bad.stderr.decode()
            assert b"--max-steps takes a positive integer" in r_bad.stderr, r_bad.stderr

            # empty stdin: exit 1 with the usage error, no hang, no half-open
            # backend left behind
            r5 = subprocess.run(
                [TNY, "--cwd", ws, "ask"],
                input=b"",
                env=env,
                capture_output=True,
                timeout=15,
            )
            assert r5.returncode == 1, f"exit {r5.returncode}: {r5.stderr.decode()}"
            assert b"ask needs a prompt" in r5.stderr, r5.stderr

            # structured output: --output-schema rides response_format
            # (mock validates the wrapper) and the answer is schema JSON
            schema = (
                '{"type":"object","properties":{"count":'
                '{"type":"integer"}},"required":["count"],'
                '"additionalProperties":false}'
            )
            r6 = subprocess.run(
                [
                    TNY,
                    "--cwd",
                    ws,
                    "ask",
                    "--json",
                    "--no-save",
                    "--output-schema",
                    schema,
                    "how many files?",
                ],
                env=env,
                capture_output=True,
                timeout=30,
            )
            assert r6.returncode == 0, f"exit {r6.returncode}: {r6.stderr.decode()}"
            out6 = json.loads(r6.stdout)
            answer = json.loads(out6["output"])
            assert answer["count"] == 3, out6

            # schema from a file path works too
            schema_path = os.path.join(home, "schema.json")
            open(schema_path, "w").write(schema)
            r7 = subprocess.run(
                [
                    TNY,
                    "--cwd",
                    ws,
                    "ask",
                    "--no-save",
                    "--output-schema",
                    schema_path,
                    "how many files?",
                ],
                env=env,
                capture_output=True,
                timeout=30,
            )
            assert r7.returncode == 0, f"exit {r7.returncode}: {r7.stderr.decode()}"
            assert json.loads(r7.stdout)["count"] == 3, r7.stdout

            # startup errors: bad schema and non-openai provider exit 1
            r8 = subprocess.run(
                [TNY, "--cwd", ws, "ask", "--output-schema", "{not json", "hi"],
                env=env,
                capture_output=True,
                timeout=15,
            )
            assert r8.returncode == 1, r8.stderr.decode()
            assert b"not a JSON object" in r8.stderr, r8.stderr
            r9 = subprocess.run(
                [
                    TNY,
                    "--provider",
                    "codex",
                    "--cwd",
                    ws,
                    "ask",
                    "--output-schema",
                    schema,
                    "hi",
                ],
                env=env,
                capture_output=True,
                timeout=15,
            )
            assert r9.returncode == 1, r9.stderr.decode()
            assert b"openai-compatible provider" in r9.stderr, r9.stderr

            # --effort: a second mock demands effort=xhigh; the canonical
            # "max" must clamp to xhigh and ride reasoning.effort on the
            # responses wire (the mock 400s a chat-style reasoning_effort
            # member, and the earlier runs already proved the field is
            # absent without the flag).
            eport = free_port()
            emock = subprocess.Popen(
                [sys.executable, MOCK, str(eport)],
                env=dict(
                    os.environ, MOCK_EXPECT_EFFORT="xhigh", MOCK_EXPECT_WIRE="responses"
                ),
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            try:
                line = emock.stdout.readline().decode()
                assert "ready" in line, f"effort mock did not start: {line!r}"
                eenv = dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{eport}/v1")
                r10 = subprocess.run(
                    [
                        TNY,
                        "--cwd",
                        ws,
                        "--effort",
                        "max",
                        "ask",
                        "--json",
                        "--no-save",
                        "list files in .",
                    ],
                    env=eenv,
                    capture_output=True,
                    timeout=30,
                )
                assert r10.returncode == 0, (
                    f"exit {r10.returncode}: {r10.stderr.decode()}"
                )
                assert b"MOCK-OK" in r10.stdout, r10.stdout
            finally:
                emock.terminate()
                emock.wait(timeout=5)

            # a terminal response.failed event is a run error (exit 2)
            # with a stable redacted diagnostic, never a silent empty answer
            fport = free_port()
            fmock = subprocess.Popen(
                [sys.executable, MOCK, str(fport)],
                env=dict(os.environ, MOCK_FAIL_RESPONSE="mock exploded"),
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            try:
                line = fmock.stdout.readline().decode()
                assert "ready" in line, f"fail mock did not start: {line!r}"
                fenv = dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{fport}/v1")
                r11 = subprocess.run(
                    [TNY, "--cwd", ws, "ask", "--no-save", "boom please"],
                    env=fenv,
                    capture_output=True,
                    timeout=30,
                )
                assert r11.returncode == 2, (
                    f"exit {r11.returncode}: {r11.stderr.decode()}"
                )
                assert b"provider stream reported an error" in r11.stderr, r11.stderr
                assert b"mock exploded" not in r11.stderr, r11.stderr
                # response.failed already classified the end of stream: the
                # abrupt close after it must not double-report a transport
                # error
                assert b"stream aborted" not in r11.stderr, r11.stderr
            finally:
                fmock.terminate()
                fmock.wait(timeout=5)

            # response.incomplete (token cutoff): partial text is preserved,
            # but the provider limit remains a non-success terminal reason
            iport = free_port()
            imock = subprocess.Popen(
                [sys.executable, MOCK, str(iport)],
                env=dict(os.environ, MOCK_INCOMPLETE="1"),
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            try:
                line = imock.stdout.readline().decode()
                assert "ready" in line, f"incomplete mock did not start: {line!r}"
                ienv = dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{iport}/v1")
                r11b = subprocess.run(
                    [TNY, "--cwd", ws, "ask", "--json", "--no-save", "list files in ."],
                    env=ienv,
                    capture_output=True,
                    timeout=30,
                )
                assert r11b.returncode == 2, (
                    f"exit {r11b.returncode}: {r11b.stderr.decode()}"
                )
                out11b = json.loads(r11b.stdout)
                assert "MOCK-OK" in out11b["output"], out11b
                assert b"stream aborted" not in r11b.stderr, r11b.stderr
            finally:
                imock.terminate()
                imock.wait(timeout=5)

            # ---- legacy chat wire (wire_api "chat", docs/adr/0016) ----
            # a chat-only mock: any request to /responses 400s, so these
            # runs prove each opt-in spelling really switches the wire
            cport = free_port()
            cmock = subprocess.Popen(
                [sys.executable, MOCK, str(cport)],
                env=dict(os.environ, MOCK_EXPECT_WIRE="chat"),
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            try:
                line = cmock.stdout.readline().decode()
                assert "ready" in line, f"chat mock did not start: {line!r}"
                cbase = dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{cport}/v1")

                # 1. OPENAI_WIRE_API=chat
                cenv = dict(cbase, OPENAI_WIRE_API="chat")
                r12 = subprocess.run(
                    [TNY, "--cwd", ws, "ask", "--json", "--no-save", "list files in ."],
                    env=cenv,
                    capture_output=True,
                    timeout=30,
                )
                assert r12.returncode == 0, (
                    f"exit {r12.returncode}: {r12.stderr.decode()}"
                )
                out12 = json.loads(r12.stdout)
                assert "MOCK-OK" in out12["output"], out12
                assert out12["steps"] == 2, out12
                assert out12["tool_calls"][0]["name"] == "list_files", out12

                # 2. --wire-api chat (flag beats the responses default)
                r13 = subprocess.run(
                    [
                        TNY,
                        "--cwd",
                        ws,
                        "--wire-api",
                        "chat",
                        "ask",
                        "--json",
                        "--no-save",
                        "list files in .",
                    ],
                    env=cbase,
                    capture_output=True,
                    timeout=30,
                )
                assert r13.returncode == 0, (
                    f"exit {r13.returncode}: {r13.stderr.decode()}"
                )
                assert b"MOCK-OK" in r13.stdout, r13.stdout

                # 3. settings.json {"openai":{"wire_api":"chat"}}
                tnydir = os.path.join(home, ".tny")
                os.makedirs(tnydir, exist_ok=True)
                settings = os.path.join(tnydir, "settings.json")
                open(settings, "w").write('{"openai":{"wire_api":"chat"}}')
                try:
                    r14 = subprocess.run(
                        [
                            TNY,
                            "--cwd",
                            ws,
                            "ask",
                            "--json",
                            "--no-save",
                            "list files in .",
                        ],
                        env=cbase,
                        capture_output=True,
                        timeout=30,
                    )
                    assert r14.returncode == 0, (
                        f"exit {r14.returncode}: {r14.stderr.decode()}"
                    )
                    assert b"MOCK-OK" in r14.stdout, r14.stdout
                finally:
                    os.remove(settings)

                # 4. structured outputs still ride response_format on chat
                schema = (
                    '{"type":"object","properties":{"count":'
                    '{"type":"integer"}},"required":["count"],'
                    '"additionalProperties":false}'
                )
                r15 = subprocess.run(
                    [
                        TNY,
                        "--cwd",
                        ws,
                        "--wire-api",
                        "chat",
                        "ask",
                        "--json",
                        "--no-save",
                        "--output-schema",
                        schema,
                        "how many?",
                    ],
                    env=cbase,
                    capture_output=True,
                    timeout=30,
                )
                assert r15.returncode == 0, (
                    f"exit {r15.returncode}: {r15.stderr.decode()}"
                )
                assert json.loads(json.loads(r15.stdout)["output"])["count"] == 3

                # 5. --wire-api rejects unknown values at startup
                r16 = subprocess.run(
                    [TNY, "--cwd", ws, "--wire-api", "grpc", "ask", "hi"],
                    env=cbase,
                    capture_output=True,
                    timeout=15,
                )
                assert r16.returncode == 1, r16.stderr.decode()
                assert b"--wire-api must be responses|chat" in r16.stderr, r16.stderr
            finally:
                cmock.terminate()
                cmock.wait(timeout=5)

            # OpenRouter-compatible mid-stream errors keep HTTP 200 and place
            # a top-level error object inside SSE. They must fail the agent,
            # not turn into a successful empty answer at [DONE].
            ceport = free_port()
            cemock = subprocess.Popen(
                [sys.executable, MOCK, str(ceport)],
                env=dict(
                    os.environ,
                    MOCK_CHAT_ERROR="routed provider failed",
                    MOCK_EXPECT_WIRE="chat",
                ),
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            try:
                line = cemock.stdout.readline().decode()
                assert "ready" in line, f"chat error mock did not start: {line!r}"
                ceenv = dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{ceport}/v1")
                cerr = subprocess.run(
                    [
                        TNY,
                        "--cwd",
                        ws,
                        "--wire-api",
                        "chat",
                        "ask",
                        "--no-save",
                        "fail in stream",
                    ],
                    env=ceenv,
                    capture_output=True,
                    timeout=30,
                )
                assert cerr.returncode == 2, cerr.stderr.decode()
                assert b"provider stream reported an error" in cerr.stderr, cerr.stderr
                assert b"routed provider failed" not in cerr.stderr, cerr.stderr
            finally:
                cemock.terminate()
                cemock.wait(timeout=5)

            # settings.json default effort (docs/adr/0015): with no flag and
            # no env, `"effort"` in settings must ride the request — mapped
            # to the openai wire vocabulary (canonical "light" -> "low").
            # A fresh HOME keeps earlier runs' saved settings out of it.
            shome = os.path.join(home, "settings-effort-home")
            os.makedirs(shome)
            os.makedirs(os.path.join(shome, ".tny"))
            open(os.path.join(shome, ".tny", "settings.json"), "w").write(
                '{"effort":{"openai":"light"}}'
            )
            sport = free_port()
            smock = subprocess.Popen(
                [sys.executable, MOCK, str(sport)],
                env=dict(os.environ, MOCK_EXPECT_EFFORT="low"),
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            try:
                line = smock.stdout.readline().decode()
                assert "ready" in line, f"settings mock did not start: {line!r}"
                senv = dict(
                    env, HOME=shome, OPENAI_BASE_URL=f"http://127.0.0.1:{sport}/v1"
                )
                senv.pop("TNY_REASONING_EFFORT", None)
                r12 = subprocess.run(
                    [TNY, "--cwd", ws, "ask", "--json", "--no-save", "list files in ."],
                    env=senv,
                    capture_output=True,
                    timeout=30,
                )
                assert r12.returncode == 0, (
                    f"exit {r12.returncode}: {r12.stderr.decode()}"
                )
                assert b"MOCK-OK" in r12.stdout, r12.stdout
                # and an explicit --effort default beats the settings value:
                # the same mock 400s any request carrying an effort field
                emock2_env = dict(os.environ)  # EXPECT unset = field absent
                eport2 = free_port()
                emock2 = subprocess.Popen(
                    [sys.executable, MOCK, str(eport2)],
                    env=emock2_env,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                )
                try:
                    line = emock2.stdout.readline().decode()
                    assert "ready" in line, f"default mock did not start: {line!r}"
                    senv2 = dict(senv, OPENAI_BASE_URL=f"http://127.0.0.1:{eport2}/v1")
                    r13 = subprocess.run(
                        [
                            TNY,
                            "--cwd",
                            ws,
                            "--effort",
                            "default",
                            "ask",
                            "--json",
                            "--no-save",
                            "list files in .",
                        ],
                        env=senv2,
                        capture_output=True,
                        timeout=30,
                    )
                    assert r13.returncode == 0, (
                        f"exit {r13.returncode}: {r13.stderr.decode()}"
                    )
                    assert b"MOCK-OK" in r13.stdout, r13.stdout
                finally:
                    emock2.terminate()
                    emock2.wait(timeout=5)
            finally:
                smock.terminate()
                smock.wait(timeout=5)

            # parallel tool calls on the chat wire (the gateway shape from
            # the field is a Chat Completions stream): three calls in one
            # step, including one that reuses an "index" with a fresh "id". Every
            # call must execute with its own arguments and every id must get
            # a tool message — the mock rejects the follow-up request with
            # 400 "no tool output found for function call" otherwise, which
            # is exactly how the field failure looked.
            open(os.path.join(ws, "a.txt"), "w").write("aaa\n")
            open(os.path.join(ws, "b.txt"), "w").write("bbb\n")
            pport = free_port()
            pmock = subprocess.Popen(
                [sys.executable, MOCK, str(pport)],
                env=dict(os.environ, MOCK_PARALLEL="1", MOCK_EXPECT_WIRE="chat"),
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            try:
                line = pmock.stdout.readline().decode()
                assert "ready" in line, f"parallel mock did not start: {line!r}"
                penv = dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{pport}/v1")
                r11 = subprocess.run(
                    [
                        TNY,
                        "--cwd",
                        ws,
                        "--wire-api",
                        "chat",
                        "ask",
                        "--json",
                        "--no-save",
                        "read both files and list the dir",
                    ],
                    env=penv,
                    capture_output=True,
                    timeout=30,
                )
                assert r11.returncode == 0, (
                    f"exit {r11.returncode}: {r11.stderr.decode()}"
                )
                out11 = json.loads(r11.stdout)
                assert "PARALLEL-OK" in out11["output"], out11
                assert out11["steps"] == 2, out11
                names = [t["name"] for t in out11["tool_calls"]]
                assert names == ["read_file", "read_file", "list_files"], out11
                assert all(t["status"] == "success" for t in out11["tool_calls"]), out11
            finally:
                pmock.terminate()
                pmock.wait(timeout=5)
        print("test_openai: all assertions passed")
    finally:
        mock.terminate()
        mock.wait(timeout=5)


if __name__ == "__main__":
    start = time.time()
    main()
    print(f"test_openai: done in {time.time() - start:.1f}s")
