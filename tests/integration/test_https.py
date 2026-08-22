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
import json
import os
import re
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time

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


def raw_tls_server(cert, key, handler):
    """One-shot TLS server driven by `handler(tls_socket)` on a thread."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=cert, keyfile=key)
    lsock = socket.socket()
    lsock.bind(("127.0.0.1", 0))
    lsock.listen(1)
    port = lsock.getsockname()[1]

    def run():
        conn, _ = lsock.accept()
        try:
            handler(ctx.wrap_socket(conn, server_side=True))
        except OSError:
            pass
        finally:
            conn.close()
            lsock.close()

    t = threading.Thread(target=run, daemon=True)
    t.start()
    return port, t


def recv_headers(tls):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = tls.recv(65536)
        if not chunk:
            break
        data += chunk
    return data


MODELS_RESP = (b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
               b'{"data":[{"id":"mock-model-tls"}]}')


def eof_with_close_notify(tls):
    """Close-delimited body, delayed so the client hits a would-block read
    mid-body, ended by a proper TLS close_notify (SSL_ERROR_ZERO_RETURN)."""
    recv_headers(tls)
    split = MODELS_RESP.index(b"\r\n\r\n") + 14  # headers + a body prefix
    tls.sendall(MODELS_RESP[:split])
    time.sleep(0.2)
    tls.sendall(MODELS_RESP[split:])
    tls.unwrap().close()


def eof_without_close_notify(tls):
    """Same body, but a bare TCP FIN with no close_notify — the buggy-server
    EOF that must read as end-of-body, not an error (SSL_ERROR_SYSCALL, 0)."""
    recv_headers(tls)
    tls.sendall(MODELS_RESP)
    tls.close()


def slow_reading_provider(tls):
    """Sleep before draining the request so a multi-megabyte POST fills both
    socket buffers and the client's SSL_write hits SSL_ERROR_WANT_WRITE."""
    time.sleep(0.8)
    data = recv_headers(tls)
    head, _, rest = data.partition(b"\r\n\r\n")
    clen = int(re.search(rb"content-length:\s*(\d+)", head, re.I).group(1))
    while len(rest) < clen:
        chunk = tls.recv(1 << 20)
        if not chunk:
            break
        rest += chunk
    body = (b'data: {"type":"response.output_text.delta","delta":"BIG-OK"}\n\n'
            b'data: {"type":"response.completed",'
            b'"response":{"status":"completed"}}\n\n')
    tls.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                b"Content-Length: %d\r\n\r\n%s" % (len(body), body))
    tls.unwrap().close()


def run_models(cert, key, handler, env):
    port, t = raw_tls_server(cert, key, handler)
    env = dict(env, OPENAI_BASE_URL=f"https://127.0.0.1:{port}/v1")
    r = subprocess.run([TNY, "models", "--json"], env=env,
                       capture_output=True, timeout=30)
    t.join(timeout=10)
    return r


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

        # close-delimited body ending in a TLS close_notify, with a stall
        # mid-body: exercises would-block reads and the clean-EOF read path
        r = run_models(cert, key, eof_with_close_notify, env)
        assert r.returncode == 0, f"exit {r.returncode}: {r.stderr.decode()}"
        assert b"mock-model-tls" in r.stdout, (r.stdout, r.stderr)

        # bare TCP FIN with no close_notify: must still be end-of-body
        r = run_models(cert, key, eof_without_close_notify, env)
        assert r.returncode == 0, f"exit {r.returncode}: {r.stderr.decode()}"
        assert b"mock-model-tls" in r.stdout, (r.stdout, r.stderr)

        # multi-megabyte prompt against a provider that reads slowly: the
        # request write must survive SSL_ERROR_WANT_WRITE and complete
        port, t = raw_tls_server(cert, key, slow_reading_provider)
        env_big = dict(env, OPENAI_BASE_URL=f"https://127.0.0.1:{port}/v1")
        r = subprocess.run([TNY, "--cwd", ws, "ask", "--json"],
                           input=b"summarize: " + b"x" * (4 << 20),
                           env=env_big, capture_output=True, timeout=30)
        t.join(timeout=10)
        assert r.returncode == 0, f"exit {r.returncode}: {r.stderr.decode()}"
        assert "BIG-OK" in json.loads(r.stdout)["output"], r.stdout
    print("test_https: all assertions passed")


if __name__ == "__main__":
    start = time.time()
    main()
    print(f"test_https: done in {time.time() - start:.1f}s")
