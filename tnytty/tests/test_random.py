#!/usr/bin/env python3
"""Random-source failures must not publish fallback tokens or launch sessions."""

from __future__ import annotations

import argparse
import contextlib
import http.client
import json
import os
import re
import socket
import subprocess
import tempfile
import time
from pathlib import Path


def request(port, token, method, path, body=None):
    connection = http.client.HTTPConnection("127.0.0.2", port, timeout=3)
    try:
        connection.request(
            method,
            path,
            body=json.dumps(body) if body is not None else None,
            headers={"Authorization": f"Bearer {token}"},
        )
        response = connection.getresponse()
        return response.status, json.loads(response.read())
    finally:
        connection.close()


@contextlib.contextmanager
def server(binary, env, token=None):
    # A local 127/8 address outside the server's explicit anonymous allowlist
    # exercises its generated-token path without exposing a public listener.
    with socket.socket() as probe:
        probe.bind(("127.0.0.2", 0))
        port = probe.getsockname()[1]
    command = [str(binary), "serve", "--listen", f"127.0.0.2:{port}"]
    if token:
        command += ["--token", token]
    with tempfile.TemporaryFile() as output:
        proc = subprocess.Popen(
            command, env=env, stdin=subprocess.DEVNULL, stdout=output, stderr=output
        )
        try:
            deadline = time.monotonic() + 5
            while True:
                log = os.pread(output.fileno(), 65536, 0).decode()
                if "HTTP API on" in log:
                    break
                assert proc.poll() is None, log
                assert time.monotonic() < deadline, log
                time.sleep(0.01)
            yield port, log
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    binary = parser.parse_args().binary.resolve()
    with tempfile.TemporaryDirectory(prefix="tnytty-random-") as root:
        env = dict(os.environ, HOME=root, XDG_CONFIG_HOME=root, XDG_RUNTIME_DIR=root)
        env.pop("TNYTTY_TOKEN", None)
        marker = Path(root) / "spawned"
        for mode in ("open", "read", "eof", "partial"):
            env["TNYTTY_FIXTURE_RANDOM"] = mode
            result = subprocess.run(
                [str(binary), "serve", "--listen", "127.0.0.2:1"],
                env=env,
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=True,
                timeout=5,
            )
            assert result.returncode == 2, result
            assert "cannot generate API token" in result.stderr, result
            assert "supply --token" in result.stderr, result
            assert "generated API token" not in result.stderr, result
            assert "HTTP API on" not in result.stderr, result

            # An explicit token permits serving; random ID failure must still
            # reject the session before creating its PTY or touching its file.
            with server(binary, env, "fixture-token") as (port, _):
                status, _ = request(
                    port,
                    "fixture-token",
                    "POST",
                    "/v1/sessions",
                    {
                        "cmd": [
                            "/bin/sh",
                            "-c",
                            'printf spawned > "$1"',
                            "fixture",
                            str(marker),
                        ]
                    },
                )
                assert status == 500, status
                status, health = request(port, "fixture-token", "GET", "/v1/health")
                assert status == 200 and health["sessions"] == 0, health
                assert not marker.exists(), "random failure launched a child"
            print(f"ok  {mode}: no generated token, session, or child")

        env["TNYTTY_FIXTURE_RANDOM"] = "interrupted"
        with server(binary, env) as (port, log):
            token = re.search(r"generated API token .*: ([0-9a-f]{32})\n", log)
            assert token, log
            status, health = request(port, token.group(1), "GET", "/v1/health")
            assert status == 200 and health["sessions"] == 0, health
            status, _ = request(port, "wrong-token", "GET", "/v1/health")
            assert status == 401, status
        print(
            "ok  interrupted open and fragmented reads yield a working authenticated listener"
        )


if __name__ == "__main__":
    main()
