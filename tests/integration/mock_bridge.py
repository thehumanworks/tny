#!/usr/bin/env python3
"""Mock `cursor-sdk-bridge` for tny integration tests (pure stdlib).

Behaves like the real host as far as tny can observe it:
  * writes a 0600 bearer-token file, then prints `cursor-sdk-bridge ready {json}`
    on stderr (schemaVersion/transport/protocol/url/authTokenFile),
  * serves all 27 adapter-to-bridge RPCs in sdk.v1.0.30 over Connect HTTP/1.1,
    including stateful local/cloud agents and all three server streams,
  * rejects any request without the right `Authorization: Bearer` with 401.

Assertions (cwd, model, agent-id reuse, bearer on Shutdown) are appended to
$TNY_MOCK_DIR/failures.log and answered with a Connect error so tny fails loudly.

State lives in $TNY_MOCK_DIR so a second tny run can check that `--resume`
reuses the agent id created by the first one.
"""

import json
import os
import secrets
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DIR = os.environ.get("TNY_MOCK_DIR") or os.getcwd()
TOKEN_PATH = os.path.join(DIR, "auth.token")
AGENT_PATH = os.path.join(DIR, "agent.txt")
RESUMED_PATH = os.path.join(DIR, "resumed.txt")
FAIL_PATH = os.path.join(DIR, "failures.log")
MODEL = os.environ.get("TNY_MOCK_MODEL", "mock-cursor-model")
EXPECT_CWD = os.path.realpath(os.environ.get("TNY_MOCK_CWD") or os.getcwd())
# When set, ModelSelection.params must carry {"id":"effort","value":<this>}
# on CreateAgent/ResumeAgent and on SendOptions.model; when unset, params
# must be absent everywhere (tny sends them only when --effort is set).
EXPECT_EFFORT = os.environ.get("TNY_MOCK_EXPECT_EFFORT")
TOKEN = secrets.token_hex(16)

ANSWER = "CURSOR-MOCK-OK"


def fail(msg):
    with open(FAIL_PATH, "a") as f:
        f.write(msg + "\n")
    print("mock assertion failed: " + msg, file=sys.stderr, flush=True)


def envelope(flags: int, payload: bytes) -> bytes:
    return bytes([flags]) + len(payload).to_bytes(4, "big") + payload


def frame(obj) -> bytes:
    return envelope(0, json.dumps(obj).encode())


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    # ---- plumbing ----
    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _err(self, code, message, http=500):
        self._json(http, {"code": code, "message": message})

    def _chunk(self, data: bytes):
        self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")

    def _authed(self) -> bool:
        if self.headers.get("Authorization") != "Bearer " + TOKEN:
            self._json(401, {"code": "unauthenticated", "message": "bad bearer"})
            return False
        if self.headers.get("Connect-Protocol-Version") != "1":
            fail(f"{self.path}: missing Connect-Protocol-Version: 1")
        return True

    def _body(self) -> bytes:
        return self.rfile.read(int(self.headers.get("Content-Length", "0")))

    def _unary_in(self):
        ctype = self.headers.get("Content-Type", "")
        if not ctype.startswith("application/json"):
            fail(f"{self.path}: unary Content-Type is {ctype!r}")
        return json.loads(self._body() or b"{}")

    def _stream_in(self):
        ctype = self.headers.get("Content-Type", "")
        if not ctype.startswith("application/connect+json"):
            fail(f"{self.path}: stream Content-Type is {ctype!r}")
        raw = self._body()
        if len(raw) < 5:
            fail(f"{self.path}: request is not an enveloped frame")
            return {}
        flags, size = raw[0], int.from_bytes(raw[1:5], "big")
        if flags != 0 or size != len(raw) - 5:
            fail(f"{self.path}: bad request envelope flags={flags} size={size}")
        return json.loads(raw[5 : 5 + size] or b"{}")

    # ---- routes ----
    def do_POST(self):
        if not self._authed():
            return
        route = {
            "/sdk.v1.SdkBridgeControlService/Ping": self.ping,
            "/sdk.v1.SdkBridgeControlService/GetVersion": self.version,
            "/sdk.v1.SdkBridgeControlService/Shutdown": self.shutdown,
            "/sdk.v1.SdkCursorService/ListModels": self.list_models,
            "/sdk.v1.SdkAgentService/CreateAgent": self.create_agent,
            "/sdk.v1.SdkAgentService/ResumeAgent": self.resume_agent,
            "/sdk.v1.SdkAgentService/Send": self.send,
            "/sdk.v1.SdkAgentService/CancelRun": self.cancel_run,
        }.get(self.path)
        if not route:
            fail(f"unexpected RPC path {self.path!r}")
            self._err("unimplemented", "no such method", http=404)
            return
        route()

    def ping(self):
        self._unary_in()
        self._json(200, {})

    def version(self):
        self._unary_in()
        self._json(
            200,
            {
                "bridgeVersion": "1.0.28-mock",
                "sdkVersion": "0.0.0-mock",
                "runtime": "python-mock",
            },
        )

    def list_models(self):
        req = self._unary_in()
        # CursorRequestOptions: the per-call key is required for catalog RPCs
        if not (req.get("options") or {}).get("apiKey"):
            fail("ListModels: options.apiKey is missing")
        # SdkModel.parameters: ModelParameterDefinition list. tny resolves
        # --effort against this catalog instead of guessing ids/values.
        self._json(
            200,
            {
                "items": [
                    {
                        "id": MODEL,
                        "parameters": [
                            {
                                "id": "effort",
                                "displayName": "Reasoning Effort",
                                "values": [
                                    {"value": "low"},
                                    {"value": "medium"},
                                    {"value": "high"},
                                    {"value": "max"},
                                ],
                            }
                        ],
                    },
                    {"id": "mock-cursor-model-2"},
                ]
            },
        )

    def _check_effort(self, who, model):
        params = model.get("params") or []
        effort = [p for p in params if p.get("id") == "effort"]
        if not EXPECT_EFFORT:
            if effort:
                fail(f"{who}: unexpected effort param {effort!r}")
            return
        want = {"id": "effort", "value": EXPECT_EFFORT}
        if want not in effort:
            fail(f"{who}: params {params!r} does not carry {want!r}")

    def _check_fast(self, who, model):
        # TNY_MOCK_FAST=1: the run used --fast, so the ModelSelection must
        # carry the per-model fast param; otherwise it must be absent so the
        # model's own default variant applies.
        params = model.get("params") or []
        fast = [p for p in params if p.get("id") == "fast"]
        if os.environ.get("TNY_MOCK_FAST") == "1":
            if fast != [{"id": "fast", "value": "true"}]:
                fail(
                    f"{who}: fast param is {fast!r}, want "
                    "[{'id': 'fast', 'value': 'true'}] (--fast)"
                )
        elif fast:
            fail(f"{who}: fast param {fast!r} without --fast; omit it")
        known = [p for p in params if p.get("id") not in ("effort", "fast")]
        if known:
            fail(f"{who}: unexpected ModelSelection.params {known!r}")

    def _check_options(self, req):
        who = self.path.rsplit("/", 1)[-1]
        opts = req.get("options") or {}
        if not isinstance(opts, dict):
            fail(f"{who}: options is not an object")
            return
        model = opts.get("model") or {}
        if not isinstance(model, dict) or model.get("id") != MODEL:
            fail(
                f"{who}: options.model is {opts.get('model')!r}, "
                f"want ModelSelection {{'id': {MODEL!r}}}"
            )
        else:
            self._check_effort(who, model)
            self._check_fast(who, model)
        local = opts.get("local") or {}
        cwd = local.get("cwd")
        if not isinstance(cwd, list) or not cwd:
            fail(f"{who}: options.local.cwd is {cwd!r}")
        elif len(cwd) > 1:
            fail(
                f"{who}: options.local.cwd has {len(cwd)} entries; "
                "send at most one (extras belong in local.dirs)"
            )
        elif os.path.realpath(cwd[0]) != EXPECT_CWD:
            fail(f"{who}: options.local.cwd[0] is {cwd[0]!r}, want {EXPECT_CWD!r}")
        if not opts.get("apiKey"):
            fail(f"{who}: options.apiKey is missing")

    def create_agent(self):
        req = self._unary_in()
        self._check_options(req)
        agent = "agent-mock-0001"
        with open(AGENT_PATH, "w") as f:
            f.write(agent)
        self._json(200, {"agentId": agent})

    def resume_agent(self):
        req = self._unary_in()
        self._check_options(req)
        want = open(AGENT_PATH).read().strip() if os.path.exists(AGENT_PATH) else ""
        got = req.get("agentId")
        if not want:
            fail("ResumeAgent called before any CreateAgent")
        elif got != want:
            fail(f"ResumeAgent: agentId is {got!r}, want the created {want!r}")
        else:
            with open(RESUMED_PATH, "w") as f:
                f.write(got)
        self._json(200, {"agentId": got or want})

    def send(self):
        req = self._stream_in()
        agent = open(AGENT_PATH).read().strip() if os.path.exists(AGENT_PATH) else ""
        if req.get("agentId") != agent:
            fail(f"Send: agentId is {req.get('agentId')!r}, want {agent!r}")
        message = req.get("message") or {}
        if not isinstance(message, dict) or not message.get("text"):
            fail(f"Send: no message.text in {req!r}")
        options = req.get("options") or {}
        if not options.get("enableDeltas"):
            fail("Send: options.enableDeltas is not true")
        send_model = options.get("model")
        if EXPECT_EFFORT or os.environ.get("TNY_MOCK_FAST") == "1":
            if not isinstance(send_model, dict):
                fail(
                    f"Send: options.model is {send_model!r}; effort/fast must "
                    "ride on SendOptions.model.params"
                )
            else:
                self._check_effort("Send", send_model)
                self._check_fast("Send", send_model)
        elif send_model is not None:
            fail(
                f"Send: unexpected options.model {send_model!r} without --effort/--fast"
            )

        self.send_response(200)
        self.send_header("Content-Type", "application/connect+json")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        # SdkMessage is a `type` discriminator plus the @cursor/sdk event as a
        # Struct payload in `message` (sdk_messages.proto). The payload shapes
        # below were captured live from cursor-sdk-bridge v1.0.28: tool calls
        # carry flat name/status/args, results are wrapped as
        # {"status":"success"|"error","value":{…}}, `running` frames re-emit
        # while a tool executes, and RunResult renders int64 token counts as
        # protojson strings.
        run = "run-mock-1"
        agent_id = req.get("agentId")

        def sdk(kind, payload):
            return frame(
                {
                    "sdkMessage": {
                        "type": kind,
                        "message": {
                            "type": kind,
                            "agent_id": agent_id,
                            "run_id": run,
                            **payload,
                        },
                    }
                }
            )

        read_running = sdk(
            "tool_call",
            {
                "call_id": "tc1",
                "name": "read",
                "status": "running",
                "args": {"path": "README.md"},
            },
        )
        out = [
            sdk("status", {"status": "RUNNING"}),
            envelope(0, b""),  # keepalive: tny must ignore it
            sdk(
                "assistant",
                {
                    "message": {
                        "role": "assistant",
                        "content": [{"type": "text", "text": ANSWER[:7]}],
                    }
                },
            ),
            sdk("thinking", {"text": "considering"}),
            read_running,
            read_running,  # re-emitted while the tool runs: render once
            sdk(
                "tool_call",
                {
                    "call_id": "tc1",
                    "name": "read",
                    "status": "completed",
                    "args": {"path": "README.md"},
                    "result": {
                        "status": "success",
                        "value": {"content": "hello\n", "totalLines": 2, "fileSize": 6},
                    },
                },
            ),
            sdk(
                "assistant",
                {
                    "message": {
                        "role": "assistant",
                        "content": [{"type": "text", "text": ANSWER[7:]}],
                    }
                },
            ),
            sdk("status", {"status": "RUNNING", "message": "wrapping up"}),
            sdk(
                "usage",
                {"usage": {"inputTokens": 111, "outputTokens": 22, "totalTokens": 133}},
            ),
            sdk("status", {"status": "FINISHED"}),
            frame(
                {
                    "result": {
                        "agentId": agent_id,
                        "runId": run,
                        "status": "RUN_LIFECYCLE_STATUS_FINISHED",
                        "result": {
                            "runId": run,
                            "agentId": agent_id,
                            "status": "RUN_LIFECYCLE_STATUS_FINISHED",
                            "result": ANSWER,
                            "model": {"id": MODEL},
                            "durationMs": "5",
                            "usage": {"inputTokens": "111", "outputTokens": "22"},
                        },
                    }
                }
            ),
            frame({"done": {"agentId": agent_id, "runId": run}}),
            envelope(2, b"{}"),
        ]
        for i, part in enumerate(out):
            self._chunk(part)
            self.wfile.flush()
            if i == 2:
                time.sleep(0.05)  # force tny through the would-block retry path
        self._chunk(b"")

    def cancel_run(self):
        self._unary_in()
        self._json(200, {})

    def shutdown(self):
        self._unary_in()  # _authed() already proved the bearer reached Shutdown
        self._json(200, {})
        threading.Timer(0.2, lambda: os._exit(0)).start()


# The original fixture above remains deliberately readable because it documents
# the live Send payload shapes.  The v1.0.30 extension supplies the complete
# service/state surface and overrides the legacy route dispatcher.
from mock_bridge_v130 import extend_handler

COMPLETE_MOCK_STATE = None
Handler = extend_handler(Handler, globals())


def main():
    os.makedirs(DIR, exist_ok=True)
    assert COMPLETE_MOCK_STATE is not None
    COMPLETE_MOCK_STATE.save()
    with open(os.path.join(DIR, "launch.json"), "w", encoding="utf-8") as f:
        json.dump(
            {
                "storeCallback": COMPLETE_MOCK_STATE.data.get("storeCallback"),
                "workspace": EXPECT_CWD,
                "apiKeyPresent": bool(os.environ.get("CURSOR_API_KEY")),
            },
            f,
            sort_keys=True,
        )
    fd = os.open(TOKEN_PATH, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.write(fd, (TOKEN + "\n").encode())
    os.close(fd)

    with open(os.path.join(DIR, "pid.txt"), "w") as f:
        f.write(str(os.getpid()))
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    srv.daemon_threads = True
    port = srv.socket.getsockname()[1]
    ready = {
        "schemaVersion": 1,
        "transport": "tcp",
        "protocol": "connect",
        "url": f"http://127.0.0.1:{port}",
        "host": "127.0.0.1",
        "port": port,
        "authTokenFile": TOKEN_PATH,
        "pid": os.getpid(),
    }
    print("cursor-sdk-bridge ready " + json.dumps(ready), file=sys.stderr, flush=True)
    print("mock bridge listening", file=sys.stderr, flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    socket.setdefaulttimeout(30)
    main()
