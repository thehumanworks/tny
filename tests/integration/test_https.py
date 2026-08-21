#!/usr/bin/env python3
"""End-to-end: tny ask --json over HTTPS against the TLS-wrapped mock
OpenAI provider (Linux system-OpenSSL dlopen path, docs/adr/0006).

- Generates a throwaway self-signed cert for 127.0.0.1 at runtime.
- SSL_CERT_FILE points tny's TLS stack at the test cert: the full native
  loop (handshake, chunked SSE, tool turn) must work over https://.
- Without SSL_CERT_FILE the same connection must be *rejected* — proof
  that certificate verification is on by default.

Linux-only: macOS SecureTransport verifies against the keychain, not
SSL_CERT_FILE, and other platforms have no TLS yet.
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import json

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TNY = sys.argv[1] if len(sys.argv) > 1 else \
    os.environ.get("TNY", os.path.join(ROOT, "build", "tny"))
MOCK = os.path.join(ROOT, "tests", "integration", "mock_openai.py")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def make_cert(tmp):
    cert = os.path.join(tmp, "cert.pem")
    key = os.path.join(tmp, "key.pem")
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
         "ec_paramgen_curve:prime256v1", "-keyout", key, "-out", cert,
         "-days", "2", "-nodes", "-subj", "/CN=127.0.0.1",
         "-addext", "subjectAltName=IP:127.0.0.1"],
        check=True, capture_output=True)
    return cert, key


def main():
    if sys.platform != "linux":
        print("test_https: skip (linux-only system-libssl path)")
        return
    if not shutil.which("openssl"):
        print("test_https: skip (no openssl CLI to mint a test cert)")
        return

    with tempfile.TemporaryDirectory() as tmp:
        cert, key = make_cert(tmp)
        port = free_port()
        mock = subprocess.Popen(
            [sys.executable, MOCK, str(port), cert, key],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        try:
            line = mock.stdout.readline().decode()
            assert "ready" in line, f"mock did not start: {line!r}"

            ws = os.path.join(tmp, "ws")
            os.makedirs(ws)
            for name in ("a.txt", "b.txt"):
                open(os.path.join(ws, name), "w").write("x\n")

            env = dict(os.environ,
                       HOME=os.path.join(tmp, "home"),
                       OPENAI_BASE_URL=f"https://127.0.0.1:{port}/v1",
                       OPENAI_API_KEY="test-key-not-real",
                       SSL_CERT_FILE=cert)
            env.pop("SSL_CERT_DIR", None)

            # trusted CA: the full tool turn must stream over https
            r = subprocess.run(
                [TNY, "--cwd", ws, "ask", "--json", "list files in ."],
                env=env, capture_output=True, timeout=30)
            assert r.returncode == 0, f"exit {r.returncode}: {r.stderr.decode()}"
            out = json.loads(r.stdout)
            assert "MOCK-OK" in out["output"], out
            assert out["steps"] == 2, out
            assert out["tool_calls"][0]["name"] == "list_files", out

            # untrusted CA: verification must reject the self-signed cert
            env_noca = dict(env)
            del env_noca["SSL_CERT_FILE"]
            r2 = subprocess.run(
                [TNY, "--cwd", ws, "ask", "--json", "hi"],
                env=env_noca, capture_output=True, timeout=30)
            assert r2.returncode != 0, \
                f"self-signed cert was accepted: {r2.stdout.decode()}"
            assert b"TLS" in r2.stderr, r2.stderr
        finally:
            mock.terminate()
            mock.wait(timeout=5)
    print("test_https: all assertions passed")


if __name__ == "__main__":
    start = time.time()
    main()
    print(f"test_https: done in {time.time() - start:.1f}s")
