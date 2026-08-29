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

import json
import os
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TNY = os.environ.get("TNY", os.path.join(ROOT, "build", "tny"))
MOCK = os.path.join(ROOT, "tests", "integration", "mock_openai.py")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


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
