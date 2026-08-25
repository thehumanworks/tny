#!/usr/bin/env python3
"""End-to-end Python hooks over the normalized native-provider event stream."""

import json
import glob
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
from tny_ext import (
    AgentEndEvent,
    BeforeAgentStartEvent,
    UserPromptSubmitEvent,
    block_prompt,
    context,
    continue_with,
    transform_prompt,
)

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
    selected = api.capabilities.selected
    assert api.capabilities.schema_version == 1
    assert api.capabilities.selected_provider == "openai"
    assert selected is not None
    assert selected.runtime == "native"
    assert selected.supports("extensions.prompt.observe")
    assert selected.state("extensions.prompt.transform") == "supported"
    api.on("*", record)

    @api.on(UserPromptSubmitEvent)
    def prepare_prompt(event):
        if event.prompt == "list files":
            return transform_prompt("TRANSFORMED-NATIVE-PROMPT")
        if event.prompt == "blocked input":
            return block_prompt("integration policy")

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

            session_files = glob.glob(os.path.join(home, ".tny", "sessions", "*", "*", "session.json"))
            assert len(session_files) == 1, session_files
            session_doc = json.load(open(session_files[0], encoding="utf-8"))
            user_messages = [
                message.get("content")
                for message in session_doc["messages"]
                if message.get("role") == "user"
            ]
            assert any(message.endswith("TRANSFORMED-NATIVE-PROMPT") for message in user_messages), user_messages
            assert not any("list files" in message for message in user_messages), user_messages
            assert session_doc["title"] == "list files", session_doc
            prompt_audit = [
                entry for entry in session_doc.get("extension_audit", [])
                if entry.get("kind") == "prompt"
            ]
            assert prompt_audit and prompt_audit[0]["submitted"] == "list files", prompt_audit
            assert prompt_audit[0]["effective"] == "TRANSFORMED-NATIVE-PROMPT", prompt_audit

            assert os.path.exists(event_log), (stderr, output)
            events = [json.loads(line) for line in open(event_log, encoding="utf-8")]
            kinds = [event["type"] for event in events]
            for required in (
                "session_start",
                "user_prompt_submit",
                "before_agent_start",
                "agent_start",
                "turn_start",
                "message_start",
                "tool_start",
                "tool_end",
                "thinking",
                "text_delta",
                "message_update",
                "message_end",
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

            order = {kind: kinds.index(kind) for kind in (
                "session_start", "user_prompt_submit", "before_agent_start",
                "agent_start", "turn_start", "message_start", "text_delta",
                "message_update", "message_end", "turn_end", "agent_end",
                "agent_settled", "session_end",
            )}
            assert list(order.values()) == sorted(order.values()), order

            blocked = subprocess.run(
                [TNY, "--cwd", workspace, "ask", "--json", "blocked input"],
                env=env,
                capture_output=True,
                timeout=30,
            )
            assert blocked.returncode == 2, (blocked.stdout, blocked.stderr)
            blocked_output = json.loads(blocked.stdout)
            assert blocked_output["exit_code"] == 2, blocked_output
            blocked_session = os.path.join(
                os.path.dirname(os.path.dirname(session_files[0])),
                blocked_output["session_id"], "session.json",
            )
            blocked_doc = json.load(open(blocked_session, encoding="utf-8"))
            assert not blocked_doc.get("messages"), blocked_doc
            assert "blocked input" not in json.dumps(blocked_doc), blocked_doc
            assert blocked_doc["extension_audit"][0]["blocked"] is True, blocked_doc
        print("test_extensions: all assertions passed")
    finally:
        mock.terminate()
        mock.wait(timeout=5)


if __name__ == "__main__":
    main()
