#!/usr/bin/env python3
"""End-to-end Python hooks over the normalized native-provider event stream."""

import json
import os
import socket
import subprocess
import sys
import tempfile


ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TNY = os.environ.get("TNY", os.path.join(ROOT, "build", "tny"))
MOCK = os.path.join(ROOT, "tests", "integration", "mock_openai.py")


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


EXTENSION = r'''
import json
import os
from tny_ext import AgentEndEvent, BeforeAgentStartEvent, context, continue_with

log_path = os.environ["TNY_TEST_EXTENSION_LOG"]

def record(event):
    with open(log_path, "a", encoding="utf-8") as stream:
        stream.write(json.dumps({
            "type": event.type,
            "sequence": event.sequence,
            "provider": event.provider,
            "session_id": event.session_id,
            "turn_id": event.turn_id,
        }, separators=(",", ":")) + "\n")

def setup(api):
    api.on("*", record)

    @api.on(BeforeAgentStartEvent)
    def inject(event):
        return context("visible test context", custom_type="integration.context")

    @api.on(AgentEndEvent)
    def continue_once(event):
        if event.continuation_count == 0:
            return continue_with("extension followup")
'''


def main():
    port = free_port()
    mock = subprocess.Popen(
        [sys.executable, MOCK, str(port)],
        env=dict(os.environ, MOCK_EXPECT_WIRE="responses"),
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    try:
        line = mock.stdout.readline().decode()
        assert "ready" in line, line
        with tempfile.TemporaryDirectory() as home:
            workspace = os.path.join(home, "ws")
            extensions = os.path.join(home, ".tny", "extensions")
            os.makedirs(workspace)
            os.makedirs(extensions)
            open(os.path.join(workspace, "a.txt"), "w").write("x\n")
            open(os.path.join(extensions, "integration.py"), "w").write(EXTENSION)
            event_log = os.path.join(home, "events.jsonl")
            env = dict(
                os.environ,
                HOME=home,
                OPENAI_BASE_URL=f"http://127.0.0.1:{port}/v1",
                OPENAI_API_KEY="test-key-not-real",
                TNY_TEST_EXTENSION_LOG=event_log,
            )
            run = subprocess.run(
                [TNY, "--cwd", workspace, "ask", "--json", "list files"],
                env=env,
                capture_output=True,
                timeout=30,
            )
            assert run.returncode == 0, run.stderr.decode()
            output = json.loads(run.stdout)
            assert "MOCK-OK" in output["output"], output
            assert any(
                message.get("kind") == "custom"
                and message.get("custom_type") == "integration.context"
                for message in output["extension_messages"]
            ), output
            assert any(
                message == {"kind": "user", "content": "extension followup"}
                for message in output["extension_messages"]
            ), output
            stderr = run.stderr.decode()
            assert "visible test context" in stderr, stderr
            assert "extension follow-up: extension followup" in stderr, stderr

            assert os.path.exists(event_log), (stderr, output)
            events = [json.loads(line) for line in open(event_log, encoding="utf-8")]
            kinds = [event["type"] for event in events]
            for required in (
                "session_start",
                "before_agent_start",
                "agent_start",
                "tool_start",
                "tool_end",
                "thinking",
                "text_delta",
                "usage",
                "turn_end",
                "agent_end",
                "agent_settled",
                "session_end",
            ):
                assert required in kinds, (required, kinds)
            assert kinds.count("agent_end") == 2, kinds
            assert kinds.count("agent_settled") == 1, kinds
            sequences = [event["sequence"] for event in events]
            assert sequences == sorted(sequences), sequences
            assert len(sequences) == len(set(sequences)), sequences
            assert all(event["provider"] == "openai" for event in events), events
            assert all(event["session_id"] for event in events), events
        print("test_extensions: all assertions passed")
    finally:
        mock.terminate()
        mock.wait(timeout=5)


if __name__ == "__main__":
    main()
