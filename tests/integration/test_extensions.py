#!/usr/bin/env python3
"""End-to-end Python hooks over the normalized native-provider event stream."""

import json
import glob
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time


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
import signal
import time
from tny_ext import (
    AgentEndEvent,
    BeforeAgentStartEvent,
    PermissionRequestEvent,
    PostToolBatchEvent,
    PostToolFailureEvent,
    PostToolUseEvent,
    PreToolUseEvent,
    ProviderRequestEvent,
    UserPromptSubmitEvent,
    annotate_tool,
    block_prompt,
    context,
    continue_with,
    decide_permission,
    deny_tool,
    replace_tool_result,
    rewrite_tool,
    stop,
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
            "tool_id": getattr(event, "tool_id", ""),
            "permission_id": getattr(event, "permission_id", ""),
            "detail": getattr(event, "message", getattr(event, "text", "")),
            "metadata": dict(getattr(event, "metadata", {})),
            "status": getattr(event, "status", 0),
            "reason": getattr(event, "reason", ""),
            "failed": getattr(event, "failed", -1),
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

    @api.on(PreToolUseEvent)
    def control_tool(event):
        if os.environ.get("TNY_TEST_SLOW_PRE") == "1" and event.tool_name == "write_file":
            open(os.environ["TNY_TEST_SLOW_MARKER"], "w", encoding="utf-8").write("entered")
            time.sleep(1.0)
        if os.environ.get("TNY_TEST_STOP_PRE") == "1" and event.tool_name == "write_file":
            return stop("stop before tool")
        if os.environ.get("TNY_TEST_DENY_TOOL") == "1" and event.tool_name == "write_file":
            return deny_tool("integration deny")
        if os.environ.get("TNY_TEST_INVALID_REWRITE") == "1" and event.tool_name == "write_file":
            return rewrite_tool({"path": "invalid.txt"})
        if os.environ.get("TNY_TEST_REWRITE") == "1" and event.tool_id == "call_1":
            return rewrite_tool({"path": "missing"})

    @api.on(PreToolUseEvent)
    def final_rewrite(event):
        if os.environ.get("TNY_TEST_REWRITE") == "1" and event.tool_id == "call_1":
            return rewrite_tool({"path": "."})

    @api.on(PostToolUseEvent)
    def annotate_result(event):
        if (os.environ.get("TNY_TEST_REPLACE") == "1" or
                os.environ.get("TNY_TEST_ANNOTATE_ONLY") == "1" or
                os.environ.get("TNY_TEST_STOP_POST") == "1") and event.tool_id == "call_1":
            return annotate_tool("integration annotation")

    @api.on(PostToolUseEvent)
    def annotate_result_second(event):
        if (os.environ.get("TNY_TEST_REPLACE") == "1" or
                os.environ.get("TNY_TEST_ANNOTATE_ONLY") == "1") and event.tool_id == "call_1":
            return annotate_tool("integration annotation second", display=False)

    @api.on(PostToolUseEvent)
    def replace_result(event):
        if os.environ.get("TNY_TEST_REPLACE") == "1" and event.tool_id == "call_1":
            return replace_tool_result("REPLACED-TOOL-RESULT")

    @api.on(PostToolUseEvent)
    def stop_after_result(event):
        if os.environ.get("TNY_TEST_STOP_POST") == "1" and event.tool_id == "call_1":
            return stop("stop after first tool")

    @api.on(PostToolFailureEvent)
    def observe_failure(event):
        return annotate_tool("observed tool failure", display=False)

    @api.on(PostToolBatchEvent)
    def stop_after_batch(event):
        if os.environ.get("TNY_TEST_STOP_BATCH") == "1":
            return stop("stop after tool batch")

    @api.on(PermissionRequestEvent)
    def decide(event):
        decision = os.environ.get("TNY_TEST_PERMISSION_DECISION")
        if decision == "stop":
            return stop("stop permission")
        if decision:
            return decide_permission(decision, "integration permission")

    @api.on(ProviderRequestEvent)
    def reject_invalid_observational_action(event):
        if os.environ.get("TNY_TEST_STOP_PROVIDER") == "1":
            return stop("stop before provider request")
        if os.environ.get("TNY_TEST_INVALID_OBSERVE") == "1":
            return replace_tool_result("not accepted here")

    @api.on(AgentEndEvent)
    def continue_once(event):
        if (os.environ.get("TNY_TEST_NO_CONTINUE") != "1" and
                event.continuation_count == 0):
            return continue_with("extension followup")
'''


def main():
    port = free_port()
    mock = subprocess.Popen(
        [sys.executable, MOCK, str(port)],
        env=dict(os.environ, MOCK_EXPECT_WIRE="responses",
                 MOCK_EXPECT_EXTENSION_REWRITE="1",
                 MOCK_EXPECT_TOOL_OUTPUT="REPLACED-TOOL-RESULT"),
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
                TNY_TEST_REWRITE="1",
                TNY_TEST_REPLACE="1",
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
            assert any(
                message.get("kind") == "custom"
                and message.get("content") == "integration annotation"
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
            tool_audit = [
                entry for entry in session_doc.get("extension_audit", [])
                if entry.get("kind") == "tool" and entry.get("id") == "call_1"
            ]
            assert len(tool_audit) == 1, tool_audit
            assert tool_audit[0]["original_arguments"] == '{"path": "."}', tool_audit
            assert tool_audit[0]["effective_arguments"] == '{"path":"."}', tool_audit
            assert tool_audit[0]["effective_result"] == "REPLACED-TOOL-RESULT", tool_audit
            assert tool_audit[0]["original_result"] != tool_audit[0]["effective_result"], tool_audit
            assert tool_audit[0]["annotations"][0]["content"] == "integration annotation", tool_audit
            assert tool_audit[0]["annotations"][0]["display"] is True, tool_audit
            assert tool_audit[0]["annotations"][1] == {
                "extension": "integration",
                "content": "integration annotation second",
                "display": False,
            }, tool_audit

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
                "provider_request",
                "provider_response",
                "pre_tool_use",
                "tool_start",
                "tool_end",
                "post_tool_use",
                "post_tool_batch",
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

            first_pre = next(i for i, event in enumerate(events)
                             if event["type"] == "pre_tool_use" and event["tool_id"] == "call_1")
            first_start = next(i for i, event in enumerate(events)
                               if event["type"] == "tool_start" and event["tool_id"] == "call_1")
            first_end = next(i for i, event in enumerate(events)
                             if event["type"] == "tool_end" and event["tool_id"] == "call_1")
            first_post = next(i for i, event in enumerate(events)
                              if event["type"] == "post_tool_use" and event["tool_id"] == "call_1")
            second_pre = next(i for i, event in enumerate(events)
                              if event["type"] == "pre_tool_use" and event["tool_id"] == "call_2")
            batch = kinds.index("post_tool_batch")
            assert first_pre < first_start < first_end < first_post < second_pre < batch, kinds
            assert kinds.index("provider_request") < kinds.index("provider_response"), kinds
            requests = [event for event in events
                        if event["type"] == "provider_request"]
            responses = [event for event in events
                         if event["type"] == "provider_response"]
            request_attempts = [
                (event["metadata"]["logical_request_id"],
                 event["metadata"]["attempt"])
                for event in requests
            ]
            response_attempts = [
                (event["metadata"]["logical_request_id"],
                 event["metadata"]["attempt"])
                for event in responses
            ]
            assert request_attempts == response_attempts, (requests, responses)
            assert len(request_attempts) == len(set(request_attempts)), request_attempts
            assert all(attempt >= 1 for _, attempt in request_attempts)
            first_attempt_sequences = [
                int(request_id.rsplit(":", 1)[1])
                for request_id, attempt in request_attempts if attempt == 1
            ]
            assert first_attempt_sequences == list(
                range(1, len(first_attempt_sequences) + 1)
            ), first_attempt_sequences
            assert any(attempt > 1 for _, attempt in request_attempts), request_attempts
            assert all(event["metadata"]["stream"] is True for event in requests + responses)
            assert all(event["metadata"]["wire_api"] == "responses"
                       for event in requests + responses)
            assert all(isinstance(event["metadata"]["step"], int)
                       for event in requests)

            order = {kind: kinds.index(kind) for kind in (
                "session_start", "user_prompt_submit", "before_agent_start",
                "agent_start", "turn_start", "message_start", "text_delta",
                "message_update", "message_end", "turn_end", "agent_end",
                "agent_settled", "session_end",
            )}
            assert list(order.values()) == sorted(order.values()), order

            resumed = subprocess.run(
                [TNY, "--cwd", workspace, "ask", "--json", "--resume",
                 output["session_id"], "resume extension session"],
                env=env, capture_output=True, timeout=30)
            assert resumed.returncode == 0, resumed.stderr.decode()
            resumed_doc = json.load(open(session_files[0], encoding="utf-8"))
            assert len([entry for entry in resumed_doc.get("extension_audit", [])
                        if entry.get("kind") == "tool"]) == 1, resumed_doc
            resumed_events = [json.loads(line) for line in
                              open(event_log, encoding="utf-8")]
            assert [event["reason"] for event in resumed_events
                    if event["type"] == "session_start"][-1] == "resume"

            recovery_file = os.path.join(os.path.dirname(session_files[0]),
                                         "recovery.json")
            with open(recovery_file, "w", encoding="utf-8") as stream:
                json.dump({"partial": "RECOVERY-PARTIAL", "at": "test"}, stream)
            recovered = subprocess.run(
                [TNY, "--cwd", workspace, "ask", "--json", "--resume",
                 output["session_id"], "--continue-recovery",
                 "recover extension session"],
                env=env, capture_output=True, timeout=30)
            assert recovered.returncode == 0, recovered.stderr.decode()
            assert not os.path.exists(recovery_file)
            recovered_events = [json.loads(line) for line in
                                open(event_log, encoding="utf-8")]
            assert [event["reason"] for event in recovered_events
                    if event["type"] == "session_start"][-1] == "recovery"

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

            # The sensitive fixture proposes one write. Extension decisions
            # resolve only that live request; abstain falls back to the CLI's
            # normal noninteractive deny path.
            pport = free_port()
            permission_mock = subprocess.Popen(
                [sys.executable, MOCK, str(pport)],
                env=dict(os.environ, MOCK_EXPECT_WIRE="responses", MOCK_SENSITIVE="1"),
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            try:
                assert "ready" in permission_mock.stdout.readline().decode()
                permission_env = dict(env,
                    OPENAI_BASE_URL=f"http://127.0.0.1:{pport}/v1",
                    TNY_TEST_REWRITE="0", TNY_TEST_REPLACE="0")
                target = os.path.join(workspace, "permission.txt")

                def permission_event_count():
                    return sum(
                        1 for line in open(event_log, encoding="utf-8")
                        if json.loads(line)["type"] == "permission_request"
                    )

                before_permissions = permission_event_count()
                allowed = subprocess.run(
                    [TNY, "--permission-mode", "ask", "--cwd", workspace,
                     "ask", "--json", "allow extension permission"],
                    env=dict(permission_env,
                             TNY_TEST_PERMISSION_DECISION="allow_once"),
                    capture_output=True, timeout=30)
                assert allowed.returncode == 0, allowed.stderr.decode()
                assert open(target, encoding="utf-8").read() == "allowed"
                assert permission_event_count() == before_permissions + 1
                os.unlink(target)

                before_permissions = permission_event_count()
                denied = subprocess.run(
                    [TNY, "--permission-mode", "ask", "--cwd", workspace,
                     "ask", "--json", "deny extension permission"],
                    env=dict(permission_env, TNY_TEST_PERMISSION_DECISION="deny"),
                    capture_output=True, timeout=30)
                assert denied.returncode == 2, denied.stderr.decode()
                assert not os.path.exists(target)
                assert permission_event_count() == before_permissions + 1

                before_permissions = permission_event_count()
                abstained = subprocess.run(
                    [TNY, "--permission-mode", "ask", "--cwd", workspace,
                     "ask", "--json", "abstain extension permission"],
                    env=dict(permission_env,
                             TNY_TEST_PERMISSION_DECISION="abstain"),
                    capture_output=True, timeout=30)
                assert abstained.returncode == 2, abstained.stderr.decode()
                assert not os.path.exists(target)
                assert permission_event_count() == before_permissions + 1

                before_permissions = permission_event_count()
                permission_stopped = subprocess.run(
                    [TNY, "--permission-mode", "ask", "--cwd", workspace,
                     "ask", "--json", "stop extension permission"],
                    env=dict(permission_env, TNY_TEST_PERMISSION_DECISION="stop"),
                    capture_output=True, timeout=30)
                assert permission_stopped.returncode == 130, permission_stopped.stderr.decode()
                assert not os.path.exists(target)
                assert permission_event_count() == before_permissions + 1
                permission_stopped_json = json.loads(permission_stopped.stdout)
                permission_stopped_sessions = glob.glob(os.path.join(
                    home, ".tny", "sessions", "*",
                    permission_stopped_json["session_id"], "session.json"))
                assert len(permission_stopped_sessions) == 1
                permission_stopped_doc = json.load(open(
                    permission_stopped_sessions[0], encoding="utf-8"))
                permission_stopped_audit = [
                    entry for entry in permission_stopped_doc.get("extension_audit", [])
                    if entry.get("kind") == "tool"
                ]
                assert permission_stopped_audit[0]["control_extension"] == "integration"
                assert permission_stopped_audit[0]["control_reason"] == "stop permission"

                pre_stopped = subprocess.run(
                    [TNY, "--yolo", "--cwd", workspace, "ask", "--json",
                     "stop before native tool"],
                    env=dict(permission_env, TNY_TEST_STOP_PRE="1"),
                    capture_output=True, timeout=30)
                assert pre_stopped.returncode == 130, pre_stopped.stderr.decode()
                assert not os.path.exists(target)

                tool_denied = subprocess.run(
                    [TNY, "--yolo", "--cwd", workspace, "ask", "--json",
                     "pre-tool deny"],
                    env=dict(permission_env, TNY_TEST_DENY_TOOL="1"),
                    capture_output=True, timeout=30)
                assert tool_denied.returncode == 0, tool_denied.stderr.decode()
                assert not os.path.exists(target)

                before_invalid_events = len(open(event_log, encoding="utf-8").readlines())
                invalid = subprocess.run(
                    [TNY, "--yolo", "--cwd", workspace, "ask", "--json",
                     "invalid rewrite"],
                    env=dict(permission_env, TNY_TEST_INVALID_REWRITE="1"),
                    capture_output=True, timeout=30)
                assert invalid.returncode == 0, invalid.stderr.decode()
                assert not os.path.exists(os.path.join(workspace, "invalid.txt"))
                invalid_json = json.loads(invalid.stdout)
                invalid_sessions = glob.glob(os.path.join(
                    home, ".tny", "sessions", "*", invalid_json["session_id"],
                    "session.json"))
                assert len(invalid_sessions) == 1, invalid_sessions
                invalid_doc = json.load(open(invalid_sessions[0], encoding="utf-8"))
                invalid_audit = [entry for entry in invalid_doc.get("extension_audit", [])
                                 if entry.get("kind") == "tool"]
                assert len(invalid_audit) == 1, invalid_audit
                assert invalid_audit[0]["effective_arguments"] == '{"path":"invalid.txt"}'
                assert invalid_audit[0]["original_ok"] is False
                assert invalid_audit[0]["effective_ok"] is False
                assert invalid_audit[0]["annotations"][0]["display"] is False
                invalid_events = [json.loads(line) for line in
                                  open(event_log, encoding="utf-8").readlines()[before_invalid_events:]]
                invalid_batches = [event for event in invalid_events
                                   if event["type"] == "post_tool_batch"]
                assert len(invalid_batches) == 1, invalid_batches
                assert invalid_batches[0]["failed"] == 1, invalid_batches

                # SIGINT arriving while Python is in a bounded pre-tool hook
                # is consumed by the runtime probe before the write executes.
                marker = os.path.join(home, "slow-pre-entered")
                slow = subprocess.Popen(
                    [TNY, "--yolo", "--cwd", workspace, "ask", "--json",
                     "cancel slow pre-tool"],
                    env=dict(permission_env, TNY_TEST_SLOW_PRE="1",
                             TNY_TEST_SLOW_MARKER=marker),
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                deadline = time.time() + 5
                while not os.path.exists(marker) and time.time() < deadline:
                    time.sleep(0.01)
                assert os.path.exists(marker), "slow pre-tool hook never started"
                slow.send_signal(signal.SIGINT)
                slow_out, slow_err = slow.communicate(timeout=10)
                assert slow.returncode == 130, (slow_out, slow_err)
                assert not os.path.exists(target), "cancelled hook still executed write"
            finally:
                permission_mock.terminate()
                permission_mock.wait(timeout=5)

            # A post-hook stop suppresses lower-priority annotation/replacement,
            # finalizes the remaining batch deterministically, and does not POST
            # another model request.
            sport = free_port()
            stop_mock = subprocess.Popen(
                [sys.executable, MOCK, str(sport)],
                env=dict(os.environ, MOCK_EXPECT_WIRE="responses"),
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            try:
                assert "ready" in stop_mock.stdout.readline().decode()
                stopped = subprocess.run(
                    [TNY, "--yolo", "--cwd", workspace, "ask", "--json",
                     "stop in post tool"],
                    env=dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{sport}/v1",
                             TNY_TEST_REWRITE="0", TNY_TEST_REPLACE="0",
                             TNY_TEST_STOP_POST="1"),
                    capture_output=True, timeout=30)
                assert stopped.returncode == 130, stopped.stderr.decode()
                stopped_json = json.loads(stopped.stdout)
                assert not any(
                    message.get("content") == "integration annotation"
                    for message in stopped_json["extension_messages"]
                ), stopped_json
                assert len(stopped_json["tool_calls"]) == 2, stopped_json
                assert stopped_json["tool_calls"][0]["status"] == "success"
                assert stopped_json["tool_calls"][1]["status"] == "error"

                invalid_observe = subprocess.run(
                    [TNY, "--yolo", "--cwd", workspace, "ask", "--json",
                     "invalid provider action"],
                    env=dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{sport}/v1",
                             TNY_TEST_REWRITE="0", TNY_TEST_REPLACE="0",
                             TNY_TEST_INVALID_OBSERVE="1"),
                    capture_output=True, timeout=30)
                assert invalid_observe.returncode == 0, invalid_observe.stderr.decode()
                assert b"invalid_action" in invalid_observe.stderr, invalid_observe.stderr

                annotated = subprocess.run(
                    [TNY, "--yolo", "--cwd", workspace, "ask", "--json",
                     "annotation-only audit"],
                    env=dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{sport}/v1",
                             TNY_TEST_REWRITE="0", TNY_TEST_REPLACE="0",
                             TNY_TEST_ANNOTATE_ONLY="1"),
                    capture_output=True, timeout=30)
                assert annotated.returncode == 0, annotated.stderr.decode()
                annotated_json = json.loads(annotated.stdout)
                annotated_sessions = glob.glob(os.path.join(
                    home, ".tny", "sessions", "*", annotated_json["session_id"],
                    "session.json"))
                assert len(annotated_sessions) == 1, annotated_sessions
                annotated_doc = json.load(open(annotated_sessions[0], encoding="utf-8"))
                annotated_audit = [entry for entry in annotated_doc.get("extension_audit", [])
                                   if entry.get("kind") == "tool" and
                                   entry.get("id") == "call_1"]
                assert len(annotated_audit) == 1, annotated_audit
                assert len(annotated_audit[0]["annotations"]) == 2, annotated_audit
                assert annotated_audit[0]["original_result"] == annotated_audit[0]["effective_result"]
                assert "replacement_extension" not in annotated_audit[0]

                transformed = subprocess.run(
                    [TNY, "--yolo", "--cwd", workspace, "ask", "--json",
                     "transformed json summary"],
                    env=dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{sport}/v1",
                             TNY_TEST_REWRITE="0", TNY_TEST_REPLACE="1",
                             TNY_TEST_NO_CONTINUE="1"),
                    capture_output=True, timeout=30)
                assert transformed.returncode == 0, transformed.stderr.decode()
                transformed_json = json.loads(transformed.stdout)
                assert len(transformed_json["tool_calls"]) == 2, transformed_json
                assert transformed_json["tool_calls"][0] == {
                    "name": "list_files",
                    "status": "success",
                    "original_status": "success",
                    "result_transformed": True,
                }, transformed_json
                assert transformed_json["tool_calls"][1] == {
                    "name": "glob_files", "status": "success"
                }, transformed_json

                provider_stopped = subprocess.run(
                    [TNY, "--yolo", "--cwd", workspace, "ask", "--json",
                     "stop provider request"],
                    env=dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{sport}/v1",
                             TNY_TEST_REWRITE="0", TNY_TEST_REPLACE="0",
                             TNY_TEST_STOP_PROVIDER="1"),
                    capture_output=True, timeout=30)
                assert provider_stopped.returncode == 130, provider_stopped.stderr.decode()

                batch_stopped = subprocess.run(
                    [TNY, "--yolo", "--cwd", workspace, "ask", "--json",
                     "stop after provider tool batch"],
                    env=dict(env, OPENAI_BASE_URL=f"http://127.0.0.1:{sport}/v1",
                             TNY_TEST_REWRITE="0", TNY_TEST_REPLACE="0",
                             TNY_TEST_STOP_BATCH="1", TNY_TEST_NO_CONTINUE="1"),
                    capture_output=True, timeout=30)
                assert batch_stopped.returncode == 130, batch_stopped.stderr.decode()
                batch_stopped_json = json.loads(batch_stopped.stdout)
                assert [tool["status"] for tool in batch_stopped_json["tool_calls"]] == [
                    "success", "success"
                ], batch_stopped_json
            finally:
                stop_mock.terminate()
                stop_mock.wait(timeout=5)

            # Raw HTTP and SSE error bodies may echo credentials or request
            # content. Only fixed categories/status metadata reach diagnostics,
            # Python, or persistence.
            secret = "EXTENSION_PROVIDER_SECRET_SENTINEL"
            for extra in (
                {"MOCK_HTTP_STATUS": "500", "MOCK_ERROR_SECRET": secret},
                {"MOCK_FAIL_RESPONSE": secret},
            ):
                eport = free_port()
                error_mock = subprocess.Popen(
                    [sys.executable, MOCK, str(eport)],
                    env=dict(os.environ, MOCK_EXPECT_WIRE="responses", **extra),
                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
                try:
                    assert "ready" in error_mock.stdout.readline().decode()
                    failed = subprocess.run(
                        [TNY, "--cwd", workspace, "ask", "--json",
                         "provider failure redaction"],
                        env=dict(env,
                                 OPENAI_BASE_URL=f"http://127.0.0.1:{eport}/v1",
                                 TNY_TEST_REWRITE="0", TNY_TEST_REPLACE="0"),
                        capture_output=True, timeout=30)
                    assert failed.returncode == 2, (failed.stdout, failed.stderr)
                    assert secret.encode() not in failed.stdout + failed.stderr
                    assert secret not in open(event_log, encoding="utf-8").read()
                    for session_file in glob.glob(
                            os.path.join(home, ".tny", "sessions", "*", "*", "session.json")):
                        assert secret not in open(session_file, encoding="utf-8").read()
                finally:
                    error_mock.terminate()
                    error_mock.wait(timeout=5)
        print("test_extensions: all assertions passed")
    finally:
        mock.terminate()
        mock.wait(timeout=5)


if __name__ == "__main__":
    main()
