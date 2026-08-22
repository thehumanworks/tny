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
        print("test_openai: all assertions passed")
    finally:
        mock.terminate()
        mock.wait(timeout=5)


if __name__ == "__main__":
    start = time.time()
    main()
    print(f"test_openai: done in {time.time() - start:.1f}s")
