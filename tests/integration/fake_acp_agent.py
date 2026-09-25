#!/usr/bin/env python3
"""Bounded stdio ACP fixture, optionally driving the client-owned MCP bridge.

No external API or credentials. ACP_FIXTURE_MODE chooses protocol failures;
ACP_FIXTURE_CALLS supplies real MCP tools/call arguments. State records protocol
facts, never the bridge command/environment or inherited credential values.
"""

from __future__ import annotations

import json
import os
import re
import selectors
import signal
import subprocess
import sys
import time
from pathlib import Path

from code_mode_fixture import code_call

MODE = os.environ.get("ACP_FIXTURE_MODE", "normal")
STATE = (
    Path(os.environ["ACP_FIXTURE_STATE_DIR"]) / f"agent-{os.getpid()}.json"
    if os.environ.get("ACP_FIXTURE_STATE_DIR")
    else Path(os.environ["ACP_FIXTURE_STATE"])
)
SID = "fixture-session-1"
MODEL = "default-model"
MODE_VALUE = "default"
EFFORT_VALUE = "default"
MCP: subprocess.Popen | None = None
MCP_SEQUENCE = 0
MCP_BUFFER = bytearray()
MCP_SELECTOR = selectors.DefaultSelector()


def record(key, value):
    try:
        data = json.loads(STATE.read_text())
    except (OSError, ValueError):
        data = {}
    data[key] = value
    temporary = STATE.with_suffix(".tmp")
    temporary.write_text(json.dumps(data))
    temporary.replace(STATE)


def send(value):
    raw = (json.dumps(value, ensure_ascii=False) + "\n").encode()
    width = int(os.environ.get("ACP_FIXTURE_SPLIT", "0"))
    if width:
        for offset in range(0, len(raw), width):
            os.write(sys.stdout.fileno(), raw[offset : offset + width])
            # Force read boundaries, including inside multi-byte UTF-8.
            time.sleep(0.0001)
    else:
        sys.stdout.buffer.write(raw)
        sys.stdout.buffer.flush()


def result(identifier, value):
    send({"jsonrpc": "2.0", "id": identifier, "result": value})


def error(identifier, code, message):
    send(
        {
            "jsonrpc": "2.0",
            "id": identifier,
            "error": {"code": code, "message": message},
        }
    )


def session_update(value, session=SID):
    send(
        {
            "jsonrpc": "2.0",
            "method": "session/update",
            "params": {"sessionId": session, "update": value},
        }
    )


def update(text, session=SID):
    send(
        {
            "jsonrpc": "2.0",
            "method": "session/update",
            "params": {
                "sessionId": session,
                "update": {
                    "sessionUpdate": "agent_message_chunk",
                    "content": {"type": "text", "text": text},
                },
            },
        }
    )


def read():
    line = sys.stdin.buffer.readline()
    return json.loads(line) if line else None


def callback(method, params):
    identifier = "fixture-callback"
    send({"jsonrpc": "2.0", "id": identifier, "method": method, "params": params})
    while (message := read()) is not None:
        if message.get("id") == identifier:
            return message
        if message.get("method") == "session/cancel":
            record("cancelled", True)
            return None
    raise RuntimeError("client closed before answering callback")


def model_options(current=None):
    choice = {
        "id": "engine" if MODE == "grouped" else "model",
        "name": "Model",
        "type": "select",
        "currentValue": current or MODEL,
        "options": json.loads(os.environ.get("ACP_FIXTURE_CATALOG", "null"))
        or [
            {"value": "default-model", "name": "Default"},
            {"value": "selected-model", "name": "Selected"},
        ],
    }
    if MODE == "grouped":
        choice["category"] = "model"
        choice["options"] = [{"group": "fixture", "options": choice["options"]}]
    if MODE == "no-model-values":
        choice.pop("options")
    configs = [
        {
            "id": "mode",
            "name": "Mode",
            "type": "select",
            "category": "mode",
            "currentValue": MODE_VALUE,
            "options": [
                {"value": "default", "name": "Default"},
                {"value": "bypassPermissions", "name": "Bypass"},
            ],
        },
        choice,
    ]
    if os.environ.get("ACP_FIXTURE_EFFORT") == "1":
        configs.append(
            {
                "id": "reasoning",
                "name": "Thought level",
                "type": "select",
                "category": "thought_level",
                "currentValue": EFFORT_VALUE,
                "options": [
                    {"value": value, "name": value}
                    for value in ("default", "low", "high")
                ],
            }
        )
    return configs


def session_result(fresh):
    value = {"sessionId": SID} if fresh else {}
    if MODE == "legacy-model":
        value["configOptions"] = [
            item for item in model_options() if item.get("id") != "model"
        ]
        value["models"] = {
            "currentModelId": MODEL,
            "availableModels": [
                {"modelId": name, "name": name}
                for name in ("default-model", "selected-model")
            ],
        }
    elif MODE != "no-models":
        value["configOptions"] = model_options()
    return value


def mcp_request(method, params):
    global MCP_SEQUENCE
    assert MCP is not None and MCP.stdin is not None and MCP.stdout is not None
    MCP_SEQUENCE += 1
    payload = {"jsonrpc": "2.0", "id": MCP_SEQUENCE, "method": method, "params": params}
    MCP.stdin.write((json.dumps(payload) + "\n").encode())
    MCP.stdin.flush()
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        while b"\n" in MCP_BUFFER:
            line, _, remaining = MCP_BUFFER.partition(b"\n")
            MCP_BUFFER[:] = remaining
            if not line:
                continue
            answer = json.loads(line)
            if answer.get("id") == MCP_SEQUENCE:
                return answer
        if not MCP_SELECTOR.select(max(0, deadline - time.monotonic())):
            break
        chunk = os.read(MCP.stdout.fileno(), 65536)
        if not chunk:
            raise RuntimeError(f"MCP bridge EOF during {method}")
        MCP_BUFFER.extend(chunk)
        if len(MCP_BUFFER) > 8 * 1024 * 1024:
            raise RuntimeError("MCP fixture response exceeded bound")
    raise RuntimeError(f"MCP bridge timed out during {method}")


def start_mcp(servers):
    global MCP
    if MCP is not None:
        return
    record("mcp_count", len(servers))
    assert len(servers) == 1, "expected one client-owned MCP bridge"
    spec = servers[0]
    assert "type" not in spec or spec["type"] == "stdio", spec.get("type")
    child_env = dict(os.environ)
    for entry in spec.get("env", []):
        child_env[entry["name"]] = entry["value"]
    MCP = subprocess.Popen(
        [spec["command"], *spec.get("args", [])],
        env=child_env,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=sys.stderr,
    )
    record("mcp_pid", MCP.pid)
    assert MCP.stdout is not None and MCP.stdin is not None
    MCP_SELECTOR.register(MCP.stdout, selectors.EVENT_READ)
    response = mcp_request(
        "initialize",
        {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "acp-fixture", "version": "1"},
        },
    )
    assert "result" in response, response
    record("mcp_initialize", response["result"])
    MCP.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
    MCP.stdin.flush()
    response = mcp_request("tools/list", {})
    assert "result" in response, response
    record("tools", response["result"]["tools"])


def prompt(message):
    record("prompted", True)
    record("model_at_prompt", MODEL)
    record("prompt", message["params"]["prompt"])
    record("prompt_started_at", time.monotonic())
    text = "\n".join(part.get("text", "") for part in message["params"]["prompt"])
    tags = re.findall(r"ACP-MANAGED:([a-zA-Z0-9_-]+)", text)
    tag = tags[-1] if tags else ""
    record("tag", tag)
    scenario = json.loads(os.environ.get("ACP_FIXTURE_SCENARIOS", "{}")).get(tag, {})
    if MODE == "prompt-error":
        error(message["id"], -32000, "controlled prompt error")
        return
    if scenario.get("fail_while") and Path(scenario["fail_while"]).exists():
        error(message["id"], -32000, "controlled managed fixture failure")
        return
    if scenario.get("descendant"):
        child = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(60)"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        record("descendant_pid", child.pid)
    if scenario.get("release"):
        deadline = time.monotonic() + 20
        while not Path(scenario["release"]).exists():
            if time.monotonic() >= deadline:
                raise RuntimeError("managed fixture release deadline exceeded")
            time.sleep(0.02)
    if scenario.get("delay"):
        time.sleep(min(float(scenario["delay"]), 5))
    if MODE == "crash":
        update("before crash")
        os._exit(3)
    if MODE == "malformed":
        sys.stdout.write("{broken json}\n")
        sys.stdout.flush()
        return
    if MODE == "oversized":
        # No newline: the receiver must enforce its bound before parsing.
        sys.stdout.write("x" * (9 * 1024 * 1024))
        sys.stdout.flush()
        return
    if MODE == "cancel":
        update("CANCEL-READY")
        record("cancel_ready", True)
        while (incoming := read()) is not None:
            if incoming.get("method") == "session/cancel":
                record("cancelled", True)
                result(message["id"], {"stopReason": "cancelled"})
                return
        return
    if MODE == "permission":
        answer = callback(
            "session/request_permission",
            {
                "sessionId": SID,
                "toolCall": {
                    "toolCallId": "permission-1",
                    "title": "fixture write",
                    "kind": "edit",
                },
                "options": [
                    {"optionId": "yes", "name": "Allow", "kind": "allow_once"},
                    {"optionId": "no", "name": "Deny", "kind": "reject_once"},
                ],
            },
        )
        record("permission", answer)
    if MODE == "callbacks":
        checks = []
        for method, params in [
            ("not/a/real/method", {"sessionId": SID}),
            (
                "fs/read_text_file",
                {"sessionId": "wrong-session", "path": "/does-not-exist"},
            ),
            ("terminal/create", {"sessionId": SID, "command": "true"}),
        ]:
            checks.append(callback(method, params))
        record("callbacks", checks)
        update("WRONG-SESSION-TEXT", "wrong-session")
    if MODE == "mcp-half-close":
        assert MCP is not None and MCP.stdin is not None and MCP.stdout is not None
        MCP.stdin.write(
            b'{"jsonrpc":"2.0","id":900,"method":"tools/list","params":{}}\n'
        )
        MCP.stdin.close()
        MCP.stdin = None
        raw = bytearray()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if not MCP_SELECTOR.select(max(0, deadline - time.monotonic())):
                raise RuntimeError("MCP half-close did not drain/exit")
            piece = os.read(MCP.stdout.fileno(), 65536)
            if not piece:
                break
            raw.extend(piece)
        answers = [json.loads(line) for line in raw.splitlines()]
        record("half_close", answers)
    calls = scenario.get("calls", json.loads(os.environ.get("ACP_FIXTURE_CALLS", "[]")))
    if calls:
        assert MCP is not None
        answers = []
        for index, call in enumerate(calls):
            for placeholder, answer_index in (("$previous_id", -1), ("$first_id", 0)):
                if placeholder in json.dumps(call):
                    previous = answers[answer_index]["result"]["content"]
                    previous_text = "\n".join(part.get("text", "") for part in previous)
                    previous_data = json.JSONDecoder().raw_decode(
                        previous_text[previous_text.index("{") :]
                    )[0]
                    identifier = previous_data.get("run_id") or previous_data["id"]
                    call = json.loads(json.dumps(call).replace(placeholder, identifier))
            if os.environ.get("ACP_FIXTURE_TOOL_UPDATES") == "1":
                session_update(
                    {
                        "sessionUpdate": "tool_call",
                        "toolCallId": f"external-{index}",
                        "title": "tool",
                        "status": "in_progress",
                    }
                )
            record("tool_call_started", call["name"])
            wire_name, wire_args = code_call(call["name"], call.get("arguments", {}))
            answers.append(
                mcp_request(
                    "tools/call",
                    {"name": wire_name, "arguments": json.loads(wire_args)},
                )
            )
            record("tool_results", answers)
            if os.environ.get("ACP_FIXTURE_TOOL_UPDATES") == "1":
                session_update(
                    {
                        "sessionUpdate": "tool_call_update",
                        "toolCallId": f"external-{index}",
                        "title": "tool",
                        "status": "completed",
                    }
                )
        record("tool_results", answers)
    for usage in json.loads(os.environ.get("ACP_FIXTURE_USAGE", "[]")):
        session_update({"sessionUpdate": "usage_update", **usage})
    update("ACP-OK café 🐕")
    record("prompt_finished_at", time.monotonic())
    result(message["id"], {"stopReason": "end_turn"})


def main():
    global MODEL, MODE_VALUE, EFFORT_VALUE
    record("argv", sys.argv[1:])
    record("process_cwd", os.getcwd())
    record("pid", os.getpid())
    record("cleanup_receipt_env_present", bool(os.environ.get("TNY_ACP_CLEANUP_FILE")))
    record(
        "credential_env_present",
        [
            key
            for key in (
                "OPENAI_API_KEY",
                "CHATGPT_ACCESS_TOKEN",
                "CHATGPT_ACCOUNT_ID",
                "TNY_JOB_API_KEY",
                "TNY_JOB_BASE_URL",
            )
            if os.environ.get(key)
        ],
    )
    while (message := read()) is not None:
        method, params = message.get("method"), message.get("params", {})
        if method == "initialize":
            record("initialize", params)
            if MODE == "init-timeout":
                continue
            result(
                message["id"],
                {
                    "protocolVersion": 1,
                    "agentCapabilities": {
                        "loadSession": MODE != "no-load",
                        "promptCapabilities": {
                            "image": os.environ.get("ACP_FIXTURE_IMAGE") == "1"
                        },
                    },
                    "authMethods": [],
                    "agentInfo": {
                        "name": os.environ.get("ACP_FIXTURE_NAME", "fixture"),
                        "version": os.environ.get("ACP_FIXTURE_VERSION", "1"),
                    },
                },
            )
        elif method in ("session/new", "session/load"):
            record(
                "load_requested" if method == "session/load" else "new_cwd",
                params.get("sessionId")
                if method == "session/load"
                else params.get("cwd"),
            )
            assert os.path.isabs(params["cwd"]), "ACP cwd must be absolute"
            if MODE == "auth":
                error(message["id"], -32000, "fixture authentication required")
                continue
            if MODE == "unsupported-new":
                error(message["id"], -32601, "fixture session/new unsupported")
                continue
            record("session_meta", params.get("_meta", {}))
            record(
                "mcp_in_load" if method == "session/load" else "mcp_in_new",
                len(params.get("mcpServers", [])),
            )
            if os.environ.get("ACP_FIXTURE_MCP") == "1":
                start_mcp(params["mcpServers"])
            result(message["id"], session_result(method == "session/new"))
        elif method == "session/set_config_option":
            record("set_config", params)
            if params["configId"] in ("engine", "model"):
                record("set_model_config", params)
            if MODE == "reject-model":
                error(message["id"], -32602, "fixture model selection rejected")
                continue
            if params["configId"] == "mode":
                MODE_VALUE = params["value"]
                record("selected_mode", MODE_VALUE)
            elif params["configId"] == "reasoning":
                EFFORT_VALUE = params["value"]
                record("selected_effort", EFFORT_VALUE)
            else:
                MODEL = params["value"]
            result(
                message["id"],
                {
                    "configOptions": model_options(
                        "default-model" if MODE == "unconfirmed-model" else None
                    )
                },
            )
        elif method == "session/set_model":
            MODEL = params["modelId"]
            record("set_model", params)
            result(message["id"], {})
        elif method == "session/prompt":
            prompt(message)
        elif method == "session/cancel":
            record("cancelled", True)
        elif "id" in message:
            error(message["id"], -32601, "fixture method unsupported")
    return 0


if __name__ == "__main__":
    # The fixture cannot hang an entire suite even if the transport regresses.
    signal.alarm(25)
    try:
        sys.exit(main())
    finally:
        if MCP is not None:
            if MCP.stdin is not None:
                MCP.stdin.close()
            try:
                MCP.wait(timeout=2)
            except subprocess.TimeoutExpired:
                MCP.kill()
                MCP.wait(timeout=2)
        MCP_SELECTOR.close()
