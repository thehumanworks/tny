#!/usr/bin/env python3
"""End-to-end: tny ask --json against the mock OpenAI provider.

Covers the full native loop on the default Responses API wire (the mock
400s anything that hits /chat/completions): typed SSE streaming split at
arbitrary byte boundaries, fragmented function_call assembly, tool
execution (list_files), second POST with the echoed function_call +
function_call_output items, session persistence + resume. A second block
re-runs the loop on the legacy chat wire via OPENAI_WIRE_API=chat,
--wire-api chat, and the settings wire_api key (docs/adr/0014), and a
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
    mock = subprocess.Popen([sys.executable, MOCK, str(port)],
                            env=dict(os.environ, MOCK_EXPECT_WIRE="responses"),
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        line = mock.stdout.readline().decode()
        assert "ready" in line, f"mock did not start: {line!r}"

        with tempfile.TemporaryDirectory() as home:
            ws = os.path.join(home, "ws")
            os.makedirs(ws)
            for name in ("a.txt", "b.txt", "c.txt"):
                open(os.path.join(ws, name), "w").write("x\n")

            env = dict(os.environ,
                       HOME=home,
                       OPENAI_BASE_URL=f"http://127.0.0.1:{port}/v1",
                       OPENAI_API_KEY="test-key-not-real")

            r = subprocess.run(
                [TNY, "--cwd", ws, "ask", "--json", "list files in ."],
                env=env, capture_output=True, timeout=30)
            assert r.returncode == 0, f"exit {r.returncode}: {r.stderr.decode()}"
            out = json.loads(r.stdout)
            assert "MOCK-OK" in out["output"], out
            assert "tier=unset" in out["output"], out  # no --fast: omit the field
            assert out["exit_code"] == 0
            assert out["steps"] == 2, out
            assert out["tool_calls"][0]["name"] == "list_files", out
            sid = out["session_id"]
            assert sid, out

            # resume must find the saved session and run another turn
            r2 = subprocess.run(
                [TNY, "--cwd", ws, "ask", "--json", "--resume", sid, "again"],
                env=env, capture_output=True, timeout=30)
            assert r2.returncode == 0, f"exit {r2.returncode}: {r2.stderr.decode()}"
            out2 = json.loads(r2.stdout)
            assert out2["session_id"] == sid, out2
            assert "MOCK-OK" in out2["output"], out2

            # sessions list must show it
            r3 = subprocess.run([TNY, "--cwd", ws, "sessions", "--json"],
                                env=env, capture_output=True, timeout=10)
            assert r3.returncode == 0, r3.stderr.decode()
            sessions = json.loads(r3.stdout)
            ids = [s["id"] for s in (sessions if isinstance(sessions, list)
                                     else sessions.get("sessions", []))]
            assert sid in ids, (sid, r3.stdout.decode())

            # the API key must never leak into output
            for blob in (r.stdout, r.stderr, r2.stdout, r2.stderr):
                assert b"test-key-not-real" not in blob, "api key leaked"

            # piped stdin: the connect/stdin overlap path must still run a
            # full turn end-to-end
            r4 = subprocess.run(
                [TNY, "--cwd", ws, "ask", "--json"],
                input=b"list files in .", env=env,
                capture_output=True, timeout=30)
            assert r4.returncode == 0, f"exit {r4.returncode}: {r4.stderr.decode()}"
            out4 = json.loads(r4.stdout)
            assert "MOCK-OK" in out4["output"], out4
            assert out4["steps"] == 2, out4

            # --fast (TNY_CAP_FAST) must ride the request as the paid tier
            # ("priority", the pre-rename spelling every provider accepts)
            r6 = subprocess.run(
                [TNY, "--cwd", ws, "--fast", "ask", "--json", "--no-save",
                 "list files in ."],
                env=env, capture_output=True, timeout=30)
            assert r6.returncode == 0, f"exit {r6.returncode}: {r6.stderr.decode()}"
            out6 = json.loads(r6.stdout)
            assert "tier=priority" in out6["output"], out6

            # --fast on a provider without the capability is a startup error
            r7 = subprocess.run(
                [TNY, "--cwd", ws, "--provider", "acp", "--fast", "ask", "hi"],
                env=env, capture_output=True, timeout=15)
            assert r7.returncode == 1, f"exit {r7.returncode}: {r7.stderr.decode()}"
            assert b"--fast is not supported" in r7.stderr, r7.stderr

            # empty stdin: exit 1 with the usage error, no hang, no half-open
            # backend left behind
            r5 = subprocess.run(
                [TNY, "--cwd", ws, "ask"],
                input=b"", env=env, capture_output=True, timeout=15)
            assert r5.returncode == 1, f"exit {r5.returncode}: {r5.stderr.decode()}"
            assert b"ask needs a prompt" in r5.stderr, r5.stderr

            # structured output: --output-schema rides response_format
            # (mock validates the wrapper) and the answer is schema JSON
            schema = ('{"type":"object","properties":{"count":'
                      '{"type":"integer"}},"required":["count"],'
                      '"additionalProperties":false}')
            r6 = subprocess.run(
                [TNY, "--cwd", ws, "ask", "--json", "--no-save",
                 "--output-schema", schema, "how many files?"],
                env=env, capture_output=True, timeout=30)
            assert r6.returncode == 0, f"exit {r6.returncode}: {r6.stderr.decode()}"
            out6 = json.loads(r6.stdout)
            answer = json.loads(out6["output"])
            assert answer["count"] == 3, out6

            # schema from a file path works too
            schema_path = os.path.join(home, "schema.json")
            open(schema_path, "w").write(schema)
            r7 = subprocess.run(
                [TNY, "--cwd", ws, "ask", "--no-save",
                 "--output-schema", schema_path, "how many files?"],
                env=env, capture_output=True, timeout=30)
            assert r7.returncode == 0, f"exit {r7.returncode}: {r7.stderr.decode()}"
            assert json.loads(r7.stdout)["count"] == 3, r7.stdout

            # startup errors: bad schema and non-openai provider exit 1
            r8 = subprocess.run(
                [TNY, "--cwd", ws, "ask", "--output-schema", "{not json", "hi"],
                env=env, capture_output=True, timeout=15)
            assert r8.returncode == 1, r8.stderr.decode()
            assert b"not a JSON object" in r8.stderr, r8.stderr
            r9 = subprocess.run(
                [TNY, "--provider", "codex", "--cwd", ws, "ask",
                 "--output-schema", schema, "hi"],
                env=env, capture_output=True, timeout=15)
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
                env=dict(os.environ, MOCK_EXPECT_EFFORT="xhigh",
                         MOCK_EXPECT_WIRE="responses"),
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            try:
                line = emock.stdout.readline().decode()
                assert "ready" in line, f"effort mock did not start: {line!r}"
                eenv = dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{eport}/v1")
                r10 = subprocess.run(
                    [TNY, "--cwd", ws, "--effort", "max", "ask", "--json",
                     "--no-save", "list files in ."],
                    env=eenv, capture_output=True, timeout=30)
                assert r10.returncode == 0, \
                    f"exit {r10.returncode}: {r10.stderr.decode()}"
                assert b"MOCK-OK" in r10.stdout, r10.stdout
            finally:
                emock.terminate()
                emock.wait(timeout=5)

            # a terminal response.failed event is a run error (exit 2)
            # whose message reaches stderr, never a silent empty answer
            fport = free_port()
            fmock = subprocess.Popen(
                [sys.executable, MOCK, str(fport)],
                env=dict(os.environ, MOCK_FAIL_RESPONSE="mock exploded"),
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            try:
                line = fmock.stdout.readline().decode()
                assert "ready" in line, f"fail mock did not start: {line!r}"
                fenv = dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{fport}/v1")
                r11 = subprocess.run(
                    [TNY, "--cwd", ws, "ask", "--no-save", "boom please"],
                    env=fenv, capture_output=True, timeout=30)
                assert r11.returncode == 2, \
                    f"exit {r11.returncode}: {r11.stderr.decode()}"
                assert b"mock exploded" in r11.stderr, r11.stderr
            finally:
                fmock.terminate()
                fmock.wait(timeout=5)

            # ---- legacy chat wire (wire_api "chat", docs/adr/0014) ----
            # a chat-only mock: any request to /responses 400s, so these
            # runs prove each opt-in spelling really switches the wire
            cport = free_port()
            cmock = subprocess.Popen(
                [sys.executable, MOCK, str(cport)],
                env=dict(os.environ, MOCK_EXPECT_WIRE="chat"),
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            try:
                line = cmock.stdout.readline().decode()
                assert "ready" in line, f"chat mock did not start: {line!r}"
                cbase = dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{cport}/v1")

                # 1. OPENAI_WIRE_API=chat
                cenv = dict(cbase, OPENAI_WIRE_API="chat")
                r12 = subprocess.run(
                    [TNY, "--cwd", ws, "ask", "--json", "--no-save",
                     "list files in ."],
                    env=cenv, capture_output=True, timeout=30)
                assert r12.returncode == 0, \
                    f"exit {r12.returncode}: {r12.stderr.decode()}"
                out12 = json.loads(r12.stdout)
                assert "MOCK-OK" in out12["output"], out12
                assert out12["steps"] == 2, out12
                assert out12["tool_calls"][0]["name"] == "list_files", out12

                # 2. --wire-api chat (flag beats the responses default)
                r13 = subprocess.run(
                    [TNY, "--cwd", ws, "--wire-api", "chat", "ask", "--json",
                     "--no-save", "list files in ."],
                    env=cbase, capture_output=True, timeout=30)
                assert r13.returncode == 0, \
                    f"exit {r13.returncode}: {r13.stderr.decode()}"
                assert b"MOCK-OK" in r13.stdout, r13.stdout

                # 3. settings.json {"openai":{"wire_api":"chat"}}
                tnydir = os.path.join(home, ".tny")
                os.makedirs(tnydir, exist_ok=True)
                settings = os.path.join(tnydir, "settings.json")
                open(settings, "w").write('{"openai":{"wire_api":"chat"}}')
                try:
                    r14 = subprocess.run(
                        [TNY, "--cwd", ws, "ask", "--json", "--no-save",
                         "list files in ."],
                        env=cbase, capture_output=True, timeout=30)
                    assert r14.returncode == 0, \
                        f"exit {r14.returncode}: {r14.stderr.decode()}"
                    assert b"MOCK-OK" in r14.stdout, r14.stdout
                finally:
                    os.remove(settings)

                # 4. structured outputs still ride response_format on chat
                schema = ('{"type":"object","properties":{"count":'
                          '{"type":"integer"}},"required":["count"],'
                          '"additionalProperties":false}')
                r15 = subprocess.run(
                    [TNY, "--cwd", ws, "--wire-api", "chat", "ask", "--json",
                     "--no-save", "--output-schema", schema, "how many?"],
                    env=cbase, capture_output=True, timeout=30)
                assert r15.returncode == 0, \
                    f"exit {r15.returncode}: {r15.stderr.decode()}"
                assert json.loads(json.loads(r15.stdout)["output"])["count"] == 3

                # 5. --wire-api rejects unknown values at startup
                r16 = subprocess.run(
                    [TNY, "--cwd", ws, "--wire-api", "grpc", "ask", "hi"],
                    env=cbase, capture_output=True, timeout=15)
                assert r16.returncode == 1, r16.stderr.decode()
                assert b"--wire-api must be responses|chat" in r16.stderr, r16.stderr
            finally:
                cmock.terminate()
                cmock.wait(timeout=5)
        print("test_openai: all assertions passed")
    finally:
        mock.terminate()
        mock.wait(timeout=5)


if __name__ == "__main__":
    start = time.time()
    main()
    print(f"test_openai: done in {time.time() - start:.1f}s")
