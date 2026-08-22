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

            r = subprocess.run(
                [TNY, "--cwd", ws, "ask", "--json", "list files in ."],
                env=env, capture_output=True, timeout=30)
            assert r.returncode == 0, f"exit {r.returncode}: {r.stderr.decode()}"
            out = json.loads(r.stdout)
            assert "MOCK-OK" in out["output"], out
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

            # empty stdin: exit 1 with the usage error, no hang, no half-open
            # backend left behind
            r5 = subprocess.run(
                [TNY, "--cwd", ws, "ask"],
                input=b"", env=env, capture_output=True, timeout=15)
            assert r5.returncode == 1, f"exit {r5.returncode}: {r5.stderr.decode()}"
            assert b"ask needs a prompt" in r5.stderr, r5.stderr

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
                r6 = subprocess.run(
                    [TNY, "--cwd", ws, "--effort", "max", "ask", "--json",
                     "--no-save", "list files in ."],
                    env=eenv, capture_output=True, timeout=30)
                assert r6.returncode == 0, \
                    f"exit {r6.returncode}: {r6.stderr.decode()}"
                assert b"MOCK-OK" in r6.stdout, r6.stdout
            finally:
                emock.terminate()
                emock.wait(timeout=5)
        print("test_openai: all assertions passed")
    finally:
        mock.terminate()
        mock.wait(timeout=5)


if __name__ == "__main__":
    start = time.time()
    main()
    print(f"test_openai: done in {time.time() - start:.1f}s")
