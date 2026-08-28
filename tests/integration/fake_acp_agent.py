#!/usr/bin/env python3
"""Minimal ACP agent (protocolVersion 1) for tny's integration tests.

Speaks JSON-RPC 2.0 over stdio, one JSON object per line. Per turn it:
  * streams a thought chunk and a few agent_message_chunk pieces,
  * issues one session/request_permission and honours the client's answer,
  * emits tool_call / tool_call_update,
  * answers session/prompt with {"stopReason": "end_turn"}.

session/load is supported and asserts the sessionId matches the one handed
out by session/new (persisted in $FAKE_ACP_STATE so a resume run can check
it across processes). Stdlib only.
"""

import json
import os
import sys

STATE = os.environ.get("FAKE_ACP_STATE")
SESSION_ID = "fake-session-1"
# FAKE_ACP_CHUNKY=1 writes each line in small pieces so the client sees partial
# lines and several messages per read(). FAKE_ACP_DIE=1 exits mid-turn.
CHUNKY = bool(os.environ.get("FAKE_ACP_CHUNKY"))
DIE = bool(os.environ.get("FAKE_ACP_DIE"))
# FAKE_ACP_SLOW_MS: hold the turn open this long before answering, so a
# TUI test can type a second message while the turn is still running.
SLOW_MS = int(os.environ.get("FAKE_ACP_SLOW_MS", "0"))
GROUPED_MODELS = bool(os.environ.get("FAKE_ACP_GROUPED_MODELS"))
NO_MODELS = bool(os.environ.get("FAKE_ACP_NO_MODELS"))
NO_MODEL_VALUES = bool(os.environ.get("FAKE_ACP_NO_MODEL_VALUES"))
REJECT_MODEL = bool(os.environ.get("FAKE_ACP_REJECT_MODEL"))
BAD_MODEL_CONFIRM = bool(os.environ.get("FAKE_ACP_BAD_MODEL_CONFIRM"))
CURRENT_MODEL = "default-model"


def log(msg):
    print(f"fake-agent: {msg}", file=sys.stderr, flush=True)


def send(obj):
    line = json.dumps(obj) + "\n"
    if not CHUNKY:
        sys.stdout.write(line)
        sys.stdout.flush()
        return
    for i in range(0, len(line), 7):
        sys.stdout.write(line[i : i + 7])
        sys.stdout.flush()


def notify(method, params):
    send({"jsonrpc": "2.0", "method": method, "params": params})


def result(msg_id, res):
    send({"jsonrpc": "2.0", "id": msg_id, "result": res})


def error(msg_id, code, message):
    send({"jsonrpc": "2.0", "id": msg_id, "error": {"code": code, "message": message}})


def update(session_id, upd):
    notify("session/update", {"sessionId": session_id, "update": upd})


def read_message():
    line = sys.stdin.readline()
    if not line:
        return None
    line = line.strip()
    if not line:
        return read_message()
    return json.loads(line)


def state_write(key, value):
    if not STATE:
        return
    data = state_read()
    data[key] = value
    with open(STATE, "w") as fh:
        json.dump(data, fh)


def state_read():
    if not STATE or not os.path.exists(STATE):
        return {}
    try:
        with open(STATE) as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return {}


def model_config(current=None):
    """Return both schema-v1.20.0 select shapes used by client tests."""
    current = current or CURRENT_MODEL
    values = [
        {"value": "default-model", "name": "Default model"},
        {"value": "selected-model", "name": "Selected model"},
        {"value": "ws-model", "name": "WebSocket model"},
    ]
    if GROUPED_MODELS:
        config = {
            "id": "engine",
            "name": "Model",
            "category": "model",
            "type": "select",
            "currentValue": current,
            "options": [
                {"group": "stable", "name": "Stable", "options": values[:2]},
                {"group": "remote", "name": "Remote", "options": values[2:]},
            ],
        }
        if NO_MODEL_VALUES:
            config.pop("options")
        return config
    # Deliberately omit category to exercise the conventional id fallback.
    config = {
        "id": "model",
        "name": "Model",
        "type": "select",
        "currentValue": current,
        "options": values,
    }
    if NO_MODEL_VALUES:
        config.pop("options")
    return config


def config_options(current=None):
    # An unrelated select before the model proves the client matches configId
    # in the set response rather than accepting the first option blindly.
    mode = {
        "id": "mode",
        "name": "Mode",
        "category": "mode",
        "type": "select",
        "currentValue": "ask",
        "options": [{"value": "ask", "name": "Ask"}],
    }
    return [mode, model_config(current)]


def session_result(session_id=None):
    res = {} if session_id is None else {"sessionId": session_id}
    if not NO_MODELS:
        res["configOptions"] = config_options()
    return res


def request_permission(session_id, req_id):
    """Ask the client, then block until it answers that exact id."""
    send(
        {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": "session/request_permission",
            "params": {
                "sessionId": session_id,
                "toolCall": {
                    "toolCallId": "tool-1",
                    "title": "rm -rf /tmp/fake",
                    "kind": "execute",
                    "status": "pending",
                },
                "options": [
                    {"optionId": "opt-allow", "name": "Allow", "kind": "allow_once"},
                    {
                        "optionId": "opt-always",
                        "name": "Always",
                        "kind": "allow_always",
                    },
                    {"optionId": "opt-reject", "name": "Reject", "kind": "reject_once"},
                ],
            },
        }
    )
    while True:
        msg = read_message()
        if msg is None:
            return None
        if msg.get("id") == req_id and ("result" in msg or "error" in msg):
            return msg.get("result")
        # ignore anything else the client sends mid-permission


def run_prompt(msg):
    session_id = msg["params"].get("sessionId", SESSION_ID)
    blocks = msg["params"].get("prompt", [])
    asked = " ".join(b.get("text", "") for b in blocks if b.get("type") == "text")
    state_write("last_prompt", asked)
    state_write("model_at_prompt", CURRENT_MODEL)
    if SLOW_MS:
        import time

        time.sleep(SLOW_MS / 1000.0)
    # echo what was asked so a test can prove which prompt each turn carried
    update(
        session_id,
        {
            "sessionUpdate": "agent_message_chunk",
            "content": {"type": "text", "text": f"[asked: {asked}] "},
        },
    )

    update(
        session_id,
        {
            "sessionUpdate": "agent_thought_chunk",
            "content": {"type": "text", "text": "thinking about it"},
        },
    )
    for piece in ["Hello ", "from ", "the ", "fake ", "ACP ", "agent."]:
        update(
            session_id,
            {
                "sessionUpdate": "agent_message_chunk",
                "content": {"type": "text", "text": piece},
            },
        )
    if DIE:
        log("exiting mid-turn on purpose")
        sys.stdout.flush()
        os._exit(3)

    update(
        session_id,
        {
            "sessionUpdate": "tool_call",
            "toolCallId": "tool-1",
            "title": "danger",
            "kind": "execute",
            "status": "in_progress",
        },
    )
    outcome = request_permission(session_id, 9001)
    state_write("permission_outcome", outcome)
    log(f"permission outcome: {outcome}")
    picked = (outcome or {}).get("outcome", {})
    ok = picked.get("outcome") == "selected" and picked.get("optionId") in (
        "opt-allow",
        "opt-always",
    )
    update(
        session_id,
        {
            "sessionUpdate": "tool_call_update",
            "toolCallId": "tool-1",
            "status": "completed" if ok else "failed",
            "content": [
                {
                    "type": "content",
                    "content": {"type": "text", "text": f"outcome={outcome}"},
                }
            ],
        },
    )
    update(
        session_id,
        {
            "sessionUpdate": "plan",
            "entries": [
                {
                    "content": "finish the turn",
                    "priority": "high",
                    "status": "completed",
                }
            ],
        },
    )
    tail = " DENIED." if not ok else " ALLOWED."
    update(
        session_id,
        {
            "sessionUpdate": "agent_message_chunk",
            "content": {"type": "text", "text": tail},
        },
    )
    result(msg["id"], {"stopReason": "end_turn"})


def main():
    global CURRENT_MODEL
    while True:
        try:
            msg = read_message()
        except ValueError as exc:
            log(f"bad json: {exc}")
            continue
        if msg is None:
            return 0
        method = msg.get("method")
        if method is None:
            continue  # a stray response
        params = msg.get("params") or {}

        if method == "initialize":
            state_write("initialize_version", params.get("protocolVersion"))
            state_write("client_caps", params.get("clientCapabilities"))
            result(
                msg["id"],
                {
                    "protocolVersion": 1,
                    "agentCapabilities": {
                        "loadSession": True,
                        "promptCapabilities": {"image": False},
                    },
                    "authMethods": [],
                    "agentInfo": {"name": "fake-acp-agent", "version": "0.0.1"},
                },
            )
        elif method == "session/new":
            state_write("new_cwd", params.get("cwd"))
            state_write("session_id", SESSION_ID)
            state_write("loaded", False)
            result(msg["id"], session_result(SESSION_ID))
        elif method == "session/load":
            want = state_read().get("session_id", SESSION_ID)
            got = params.get("sessionId")
            state_write("load_requested", got)
            if got != want:
                error(msg["id"], -32602, f"unknown sessionId {got!r}")
                continue
            state_write("loaded", True)
            update(
                got,
                {
                    "sessionUpdate": "agent_message_chunk",
                    "content": {"type": "text", "text": "(replayed history)"},
                },
            )
            result(msg["id"], session_result())
        elif method == "session/set_config_option":
            if REJECT_MODEL:
                error(msg["id"], -32602, "model selection rejected by fixture")
                continue
            expected_id = "engine" if GROUPED_MODELS else "model"
            value = params.get("value")
            state_write("set_config_id", params.get("configId"))
            state_write("set_config_value", value)
            if (
                params.get("sessionId") != SESSION_ID
                or params.get("configId") != expected_id
            ):
                error(msg["id"], -32602, "invalid model configuration request")
                continue
            advertised = {"default-model", "selected-model", "ws-model"}
            if value not in advertised:
                error(msg["id"], -32602, "unknown model")
                continue
            CURRENT_MODEL = value
            confirmed = "default-model" if BAD_MODEL_CONFIRM else CURRENT_MODEL
            result(msg["id"], {"configOptions": config_options(confirmed)})
        elif method == "session/prompt":
            run_prompt(msg)
        elif method == "session/cancel":
            state_write("cancelled", True)
        elif "id" in msg:
            error(msg["id"], -32601, f"unknown method {method}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
