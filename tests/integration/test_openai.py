#!/usr/bin/env python3
"""End-to-end: tny ask --json against the mock OpenAI provider.

Covers the full native loop: SSE streaming, fragmented tool_call assembly,
tool execution (list_files), second POST, session persistence + resume.
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
    mock = subprocess.Popen([sys.executable, MOCK, str(port)],
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

            # single tool call: one call streamed fragmented, one tool
            # message back, one follow-up POST (the mock 400s any request
            # whose assistant tool_calls are not fully paired)
            r = subprocess.run(
                [TNY, "--cwd", ws, "ask", "--json", "list files in ."],
                env=env, capture_output=True, timeout=30)
            assert r.returncode == 0, f"exit {r.returncode}: {r.stderr.decode()}"
            out = json.loads(r.stdout)
            assert "MOCK-OK" in out["output"], out
            assert "tier=unset" in out["output"], out  # no --fast: omit the field
            assert out["exit_code"] == 0
            assert out["steps"] == 2, out
            assert len(out["tool_calls"]) == 1, out
            assert out["tool_calls"][0]["name"] == "list_files", out
            assert out["tool_calls"][0]["status"] == "success", out
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

            # --effort: a second mock demands reasoning_effort=xhigh; the
            # canonical "max" must clamp to xhigh on the OpenAI wire, and the
            # mock 400s any request that carries something else (the earlier
            # runs already proved the field is absent without the flag).
            eport = free_port()
            emock = subprocess.Popen(
                [sys.executable, MOCK, str(eport)],
                env=dict(os.environ, MOCK_EXPECT_EFFORT="xhigh"),
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

            # settings.json default effort (docs/adr/0015): with no flag and
            # no env, `"effort"` in settings must ride the request — mapped
            # to the openai wire vocabulary (canonical "light" -> "low").
            # A fresh HOME keeps earlier runs' saved settings out of it.
            shome = os.path.join(home, "settings-effort-home")
            os.makedirs(shome)
            os.makedirs(os.path.join(shome, ".tny"))
            open(os.path.join(shome, ".tny", "settings.json"), "w").write(
                '{"effort":{"openai":"light"}}')
            sport = free_port()
            smock = subprocess.Popen(
                [sys.executable, MOCK, str(sport)],
                env=dict(os.environ, MOCK_EXPECT_EFFORT="low"),
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            try:
                line = smock.stdout.readline().decode()
                assert "ready" in line, f"settings mock did not start: {line!r}"
                senv = dict(env, HOME=shome,
                            OPENAI_BASE_URL=f"http://127.0.0.1:{sport}/v1")
                senv.pop("TNY_REASONING_EFFORT", None)
                r12 = subprocess.run(
                    [TNY, "--cwd", ws, "ask", "--json", "--no-save",
                     "list files in ."],
                    env=senv, capture_output=True, timeout=30)
                assert r12.returncode == 0, \
                    f"exit {r12.returncode}: {r12.stderr.decode()}"
                assert b"MOCK-OK" in r12.stdout, r12.stdout
                # and an explicit --effort default beats the settings value:
                # the same mock 400s any request carrying an effort field
                emock2_env = dict(os.environ)  # EXPECT unset = field absent
                eport2 = free_port()
                emock2 = subprocess.Popen(
                    [sys.executable, MOCK, str(eport2)], env=emock2_env,
                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
                try:
                    line = emock2.stdout.readline().decode()
                    assert "ready" in line, f"default mock did not start: {line!r}"
                    senv2 = dict(senv, OPENAI_BASE_URL=f"http://127.0.0.1:{eport2}/v1")
                    r13 = subprocess.run(
                        [TNY, "--cwd", ws, "--effort", "default", "ask",
                         "--json", "--no-save", "list files in ."],
                        env=senv2, capture_output=True, timeout=30)
                    assert r13.returncode == 0, \
                        f"exit {r13.returncode}: {r13.stderr.decode()}"
                    assert b"MOCK-OK" in r13.stdout, r13.stdout
                finally:
                    emock2.terminate()
                    emock2.wait(timeout=5)
            finally:
                smock.terminate()
                smock.wait(timeout=5)

            # parallel tool calls: three calls in one step, including the
            # gateway shape that reuses an "index" with a fresh "id". Every
            # call must execute with its own arguments and every id must get
            # a tool message — the mock rejects the follow-up request with
            # 400 "no tool output found for function call" otherwise, which
            # is exactly how the field failure looked.
            open(os.path.join(ws, "a.txt"), "w").write("aaa\n")
            open(os.path.join(ws, "b.txt"), "w").write("bbb\n")
            pport = free_port()
            pmock = subprocess.Popen(
                [sys.executable, MOCK, str(pport)],
                env=dict(os.environ, MOCK_PARALLEL="1"),
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            try:
                line = pmock.stdout.readline().decode()
                assert "ready" in line, f"parallel mock did not start: {line!r}"
                penv = dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{pport}/v1")
                r11 = subprocess.run(
                    [TNY, "--cwd", ws, "ask", "--json", "--no-save",
                     "read both files and list the dir"],
                    env=penv, capture_output=True, timeout=30)
                assert r11.returncode == 0, \
                    f"exit {r11.returncode}: {r11.stderr.decode()}"
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
