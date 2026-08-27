#!/usr/bin/env python3
"""--ssh: tny stays local, workspace tools run on the remote host (ADR 0022).

A fake `ssh` on PATH logs its argv and executes the remote command with the
local sh inside a sandbox "remote" dir. The mock OpenAI server drives the
native loop's list_files/glob_files scenario, so the tool calls the model
makes must land in the sandbox — not in --cwd — and the session stays local.
"""

import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
TNY = os.environ.get("TNY", str(ROOT / "build" / "tny"))
MOCK = str(ROOT / "tests" / "integration" / "mock_openai.py")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def main():
    port = free_port()
    td_obj = tempfile.TemporaryDirectory()
    d = pathlib.Path(td_obj.name)
    home, ws, remote, binp = d / "home", d / "ws", d / "remote", d / "bin"
    # the system prompt must announce the remote host + cwd and must not
    # advertise the local --cwd as the workspace (the model "corrects" pwd
    # against it otherwise)
    mock = subprocess.Popen(
        [sys.executable, MOCK, str(port)],
        env=dict(
            os.environ,
            MOCK_EXPECT_INSTRUCTIONS=f"REMOTE environment: every workspace tool "
            f"(files, grep, terminal) executes over SSH on "
            f"alice@example.test. The local machine running "
            f"tny is not your workspace.\n"
            f"Current working directory (remote): {remote}\n",
            MOCK_REJECT_INSTRUCTIONS=f"Primary workspace: {ws}",
        ),
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    try:
        assert "ready" in mock.stdout.readline().decode()
        with td_obj:
            for p in (home, ws, remote, binp):
                p.mkdir()
            (ws / "local-only.txt").write_text("x\n")
            (remote / "remote-only.txt").write_text("y\n")
            log = d / "ssh-argv.json"
            fake = binp / "ssh"
            # resolve the real interpreter: a mise/pyenv shim breaks under the test HOME
            py = os.path.realpath(sys.executable)
            fake.write_text(f"""#!{py}
import json, os, subprocess, sys
av = sys.argv[1:]
with open({str(log)!r}, "a") as f: f.write(json.dumps(av) + "\\n")
if av[-1] == "true" or "exit" in av: sys.exit(0)
os.environ["HOME"] = {str(remote)!r}
sys.exit(subprocess.call(["sh", "-c", av[-1]]))
""")
            fake.chmod(0o755)
            env = dict(
                os.environ,
                HOME=str(home),
                PATH=f"{binp}{os.pathsep}{os.environ.get('PATH', '')}",
                OPENAI_BASE_URL=f"http://127.0.0.1:{port}/v1",
                OPENAI_API_KEY="test-key-not-real",
            )

            r = subprocess.run(
                [
                    TNY,
                    "--cwd",
                    str(ws),
                    "--ssh",
                    "alice@example.test:2222",
                    "--ssh-cwd",
                    str(remote),
                    "ask",
                    "--json",
                    "list files in .",
                ],
                env=env,
                capture_output=True,
                timeout=60,
            )
            assert r.returncode == 0, f"exit {r.returncode}: {r.stderr.decode()}"
            out = json.loads(r.stdout)
            assert "MOCK-OK" in out["output"], out
            assert [t["name"] for t in out["tool_calls"]] == [
                "list_files",
                "glob_files",
            ], out
            assert all(t["status"] == "success" for t in out["tool_calls"]), out

            calls = [json.loads(line) for line in log.read_text().splitlines()]
            # master first (interactive, no BatchMode), then batch calls
            assert calls[0][-1] == "true" and "BatchMode=yes" not in calls[0], calls[0]
            # calls[1] resolves the remote cwd (`cd DIR && pwd`); tools follow
            assert "&& pwd" in calls[1][-1], calls[1]
            for av in calls[1:]:
                assert "BatchMode=yes" in av and "ControlMaster=auto" in av, av
                assert av[av.index("-p") + 1] == "2222", av
                assert av[av.index("--") + 1] == "alice@example.test", av
            for av in calls[2:]:
                assert f"cd '{remote}' && exec sh -c " in av[-1], av
            remote_cmds = [av[-1] for av in calls]
            assert any("ls -1Ap" in c for c in remote_cmds), remote_cmds
            assert any("find ." in c for c in remote_cmds), remote_cmds
            # the session is local (under HOME), nothing was written remotely
            assert (home / ".tny").exists()
            assert sorted(p.name for p in remote.iterdir()) == ["remote-only.txt"]
            assert (home / ".tny" / "ssh").stat().st_mode & 0o777 == 0o700

            # ---- error paths, no provider needed ----
            r = subprocess.run(
                [TNY, "--ssh", "host:0", "ask", "x"],
                env=env,
                text=True,
                capture_output=True,
            )
            assert r.returncode == 1 and "invalid SSH port" in r.stderr, r.stderr
            r = subprocess.run([TNY, "--ssh"], env=env, text=True, capture_output=True)
            assert r.returncode == 1 and "--ssh requires a value" in r.stderr, r.stderr
            # host backends own their tool loop: refused, not silently local
            r = subprocess.run(
                [TNY, "--provider", "codex", "--ssh", "box", "ask", "x"],
                env=env,
                text=True,
                capture_output=True,
            )
            assert r.returncode == 1 and "native loop" in r.stderr, r.stderr
            # a failing master connection is reported, not ignored
            (binp / "ssh").write_text("#!/bin/sh\nexit 255\n")
            r = subprocess.run(
                [TNY, "--ssh", "box", "ask", "x"],
                env=env,
                text=True,
                capture_output=True,
            )
            assert r.returncode == 1 and "ssh box failed" in r.stderr, r.stderr
    finally:
        mock.kill()
    print("ssh integration: ok")


if __name__ == "__main__":
    main()
