#!/usr/bin/env python3
"""End-to-end remote MCP over JSON-only Streamable HTTP; SSE fails cleanly."""

import json
import os
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TNY = os.environ.get("TNY", os.path.join(ROOT, "build", "tny"))
MCP_MOCK = os.path.join(ROOT, "tests", "integration", "mock_mcp_http.py")
OPENAI_MOCK = os.path.join(ROOT, "tests", "integration", "mock_openai.py")
SECRET = "fixture-secret-issue-87"


def start(script, env):
    proc = subprocess.Popen(
        [sys.executable, script, "0"],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    line = proc.stdout.readline().decode()
    assert line.startswith("ready on "), (script, line, proc.stderr.read().decode())
    return proc, int(line.split()[-1])


def stop(proc):
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)


def run_case(mcp_port, endpoint, expected_output):
    arguments = json.dumps(
        {"server": "remote", "tool": "echo", "arguments": {"text": "hello"}},
        separators=(",", ":"),
    )
    provider_env = dict(
        os.environ,
        MOCK_EXPECT_WIRE="responses",
        MOCK_CUSTOM_TOOL="mcp_select_tool",
        MOCK_CUSTOM_ARGUMENTS=arguments,
        MOCK_EXPECT_TOOL_OUTPUT=expected_output,
    )
    provider, provider_port = start(OPENAI_MOCK, provider_env)
    try:
        with tempfile.TemporaryDirectory() as home:
            ws = os.path.join(home, "ws")
            tny_dir = os.path.join(home, ".tny")
            os.makedirs(ws)
            os.makedirs(tny_dir)
            profile = {
                "servers": {
                    "remote": {
                        "type": "http",
                        "url": f"http://127.0.0.1:{mcp_port}/{endpoint}",
                        "headers": {"X-Tenant": "fixture"},
                        "bearer_token_env": "MCP_TEST_TOKEN",
                    }
                }
            }
            with open(os.path.join(tny_dir, "mcp.json"), "w", encoding="utf-8") as f:
                json.dump(profile, f)
            env = dict(
                os.environ,
                HOME=home,
                MCP_TEST_TOKEN=SECRET,
                OPENAI_BASE_URL=f"http://127.0.0.1:{provider_port}/v1",
                OPENAI_API_KEY="synthetic-openai-key",
            )
            result = subprocess.run(
                [TNY, "--cwd", ws, "--ephemeral", "ask", "--json", "call remote echo"],
                env=env,
                capture_output=True,
                timeout=30,
            )
            combined = result.stdout + result.stderr
            assert SECRET.encode() not in combined, (
                "MCP auth secret leaked in tny output"
            )
            assert result.returncode == 0, (
                endpoint,
                result.returncode,
                result.stdout.decode(errors="replace"),
                result.stderr.decode(errors="replace"),
            )
            parsed = json.loads(result.stdout)
            assert parsed["exit_code"] == 0, parsed
            assert parsed["tool_calls"][0]["name"] == "mcp_select_tool", parsed
    finally:
        stop(provider)


def main():
    with tempfile.TemporaryDirectory() as temp:
        state_path = os.path.join(temp, "state.json")
        fixture_env = dict(
            os.environ,
            MCP_MOCK_STATE=state_path,
            MCP_MOCK_TOKEN=SECRET,
        )
        fixture, mcp_port = start(MCP_MOCK, fixture_env)
        try:
            run_case(mcp_port, "modern", "modern called ok\n")
            run_case(mcp_port, "legacy", "legacy called ok\n")
            run_case(
                mcp_port,
                "sse",
                "error: MCP call to remote/echo failed: unsupported SSE response transport; "
                "configure the server to return application/json over Streamable HTTP POST",
            )
            run_case(
                mcp_port,
                "legacy-sse",
                "error: could not connect MCP server remote: unsupported legacy HTTP+SSE "
                "transport; configure the Streamable HTTP POST endpoint or use a local stdio "
                "proxy",
            )
            time.sleep(0.1)
            with open(state_path, encoding="utf-8") as f:
                state = json.load(f)
            assert state["errors"] == [], state
            assert state["get"] == 0, state
            assert state["modern"].get("server/discover", 0) >= 1, state
            assert state["modern"].get("initialize", 0) == 0, state
            assert state["modern"].get("notifications/initialized", 0) == 0, state
            # wasm has no warm-up threads, so the catalog prefetch (tools/list)
            # is lazy and may not run before the calls above.
            if not os.environ.get("TNY_TEST_EXPECT_WASM"):
                assert state["modern"].get("tools/list", 0) >= 1, state
            assert state["modern"].get("tools/call", 0) == 1, state
            assert state["sse"].get("tools/call", 0) == 1, state
            assert state["legacy"].get("server/discover", 0) >= 1, state
            assert state["legacy"].get("initialize", 0) >= 1, state
            assert state["legacy"].get("notifications/initialized", 0) >= 1, state
            if not os.environ.get("TNY_TEST_EXPECT_WASM"):
                assert state["legacy"].get("tools/list", 0) >= 1, state
            assert state["legacy"].get("tools/call", 0) == 1, state
            assert state["auth_ok"] > 0, state
        finally:
            stop(fixture)

    print("mcp http integration: ok")


if __name__ == "__main__":
    main()
