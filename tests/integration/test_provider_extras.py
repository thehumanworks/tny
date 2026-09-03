#!/usr/bin/env python3
"""Per-provider request add-ons (docs/adr/0067) end to end.

OpenCode Go asks every client for `x-opencode-session: <one stable id per
conversation>`. tny sends its session id whenever the provider resolves to
an opencode.ai host or an `opencode*` profile name, keeps it stable across
`--resume`, sends nothing on other providers, and honors the
TNY_PROVIDER_EXTRAS=0 kill switch. Fixture-only: the mock logs the header.
"""

import json
import os
import socket
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MOCK = os.path.join(HERE, "mock_openai.py")
TNY = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TNY", "build/tny")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def ask(env, ws, provider, *ask_args):
    r = subprocess.run(
        [TNY, "--cwd", ws, "--provider", provider, "ask", "--json", *ask_args, "hello"],
        env=env,
        capture_output=True,
        timeout=30,
    )
    assert r.returncode == 0, f"exit {r.returncode}: {r.stderr.decode()}"
    return json.loads(r.stdout)


def with_mock(fn):
    port = free_port()
    with tempfile.TemporaryDirectory() as home:
        log = os.path.join(home, "hdr.log")
        mock = subprocess.Popen(
            [sys.executable, MOCK, str(port)],
            env=dict(
                os.environ, MOCK_LOG_HEADERS="x-opencode-session", MOCK_HEADER_LOG=log
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        try:
            assert "ready" in mock.stdout.readline().decode()
            ws = os.path.join(home, "ws")
            os.makedirs(ws)
            fn(port, home, ws, log)
        finally:
            mock.kill()
            mock.wait()


def logged(path):
    if not os.path.exists(path):
        return []
    return [line.split("=", 1) for line in open(path).read().splitlines()]


def check_opencodego_env_profile(port, home, ws, log):
    """OPENCODEGO_BASE_URL/OPENCODEGO_API_KEY (an env-only profile whose
    host is the mock, not opencode.ai): matched by name."""
    env = dict(
        os.environ,
        HOME=home,
        OPENCODEGO_BASE_URL=f"http://127.0.0.1:{port}/v1",
        OPENCODEGO_API_KEY="test-key-not-real",
    )
    out = ask(env, ws, "opencodego")
    sid = out["session_id"]
    assert sid
    out2 = ask(env, ws, "opencodego", "--resume", sid)
    assert out2["session_id"] == sid
    seen = logged(log)
    assert len(seen) >= 2, seen
    assert all(n == "x-opencode-session" and v == sid for n, v in seen), (seen, sid)


def check_opencode_settings_profile(port, home, ws, log):
    """A settings.json profile under a non-opencode name against a
    non-opencode host: no add-on, and the header is absent on the wire."""
    os.makedirs(os.path.join(home, ".tny"))
    with open(os.path.join(home, ".tny", "settings.json"), "w") as f:
        json.dump(
            {
                "work": {
                    "base_url": f"http://127.0.0.1:{port}/v1",
                    "api_key": "test-key-not-real",
                }
            },
            f,
        )
    env = dict(os.environ, HOME=home)
    ask(env, ws, "work")
    seen = logged(log)
    assert seen and all(v == "" for _, v in seen), seen  # not opencode: nothing


def check_kill_switch(port, home, ws, log):
    env = dict(
        os.environ,
        HOME=home,
        TNY_PROVIDER_EXTRAS="0",
        OPENCODEGO_BASE_URL=f"http://127.0.0.1:{port}/v1",
        OPENCODEGO_API_KEY="test-key-not-real",
    )
    ask(env, ws, "opencodego")
    seen = logged(log)
    assert seen and all(v == "" for _, v in seen), seen


def main():
    for fn in (
        check_opencodego_env_profile,
        check_opencode_settings_profile,
        check_kill_switch,
    ):
        with_mock(fn)
    print("test_provider_extras: ok")


if __name__ == "__main__":
    main()
