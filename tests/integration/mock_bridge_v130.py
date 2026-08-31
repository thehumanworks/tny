"""Complete v1.0.30 service/state extension for :mod:`mock_bridge`.

Kept separate so the captured live Send fixture in mock_bridge.py stays small
and reviewable.  This module uses only the Python standard library.
"""

import base64
import http.client
import json
import os
import threading
from datetime import datetime, timezone
from urllib.parse import urlsplit

NOW = "2026-08-30T12:00:00Z"
CAPABILITIES = [
    "cursor.catalog",
    "agent.create",
    "agent.resume",
    "agent.management",
    "agent.send",
    "run.wait",
    "run.observe",
    "run.cancel",
    "artifacts.chunked",
    "agent.usage",
]

ROUTES = {
    "SdkBridgeControlService": (
        "Ping",
        "GetVersion",
        "Shutdown",
        "SetToolCallback",
    ),
    "SdkCursorService": ("Me", "ListModels", "ListRepositories"),
    "SdkAgentService": (
        "CreateAgent",
        "ResumeAgent",
        "ReloadAgent",
        "CloseAgent",
        "Send",
        "WaitLiveRun",
        "GetRun",
        "ListRuns",
        "GetRunConversation",
        "ObserveRun",
        "CancelRun",
        "GetAgent",
        "ListAgents",
        "ArchiveAgent",
        "UnarchiveAgent",
        "DeleteAgent",
        "ListAgentMessages",
        "ListArtifacts",
        "DownloadArtifact",
        "GetUsage",
    ),
}


def _varint(value):
    out = bytearray()
    while value > 0x7F:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def _pvar(field, value):
    return _varint(field << 3) + _varint(value)


def _pbytes(field, value):
    value = value.encode() if isinstance(value, str) else value
    return _varint((field << 3) | 2) + _varint(len(value)) + value


def _detail(code, message):
    numbers = {
        "unauthorized": 1,
        "api_key_not_found": 2,
        "feature_unavailable": 5,
        "agent_not_found": 6,
        "run_not_found": 7,
        "validation_error": 8,
        "agent_archived": 16,
        "run_not_cancellable": 17,
        "internal_error": 20,
    }
    raw = b"".join(
        (
            _pbytes(1, "mock-request-id"),
            _pvar(2, numbers.get(code, 20)),
            _pbytes(3, message),
            _pbytes(4, "https://cursor.com/docs/sdk/bridge"),
            _pbytes(5, "mock"),
        )
    )
    return {
        "type": "type.googleapis.com/sdk.v1.SdkErrorDetails",
        "value": base64.b64encode(raw).decode(),
    }


class State:
    def __init__(self, cfg):
        self.cfg = cfg
        self.path = os.path.join(cfg["DIR"], "state.json")
        self.routes_path = os.path.join(cfg["DIR"], "routes.log")
        self.tool_result_path = os.path.join(cfg["DIR"], "custom_tool_result.json")
        self.lock = threading.RLock()
        self.data = self._load()
        self.data["toolCallback"] = None
        self.data["storeCallback"] = self._launch_store_callback()

    def _new(self):
        return {
            "nextLocal": 1,
            "nextCloud": 1,
            "nextRun": 1,
            "agents": {},
            "runs": {},
            "createIdempotency": {},
            "sendIdempotency": {},
            "toolCallback": None,
        }

    def _load(self):
        try:
            with open(self.path, encoding="utf-8") as f:
                value = json.load(f)
            if isinstance(value, dict) and isinstance(value.get("agents"), dict):
                return value
        except (FileNotFoundError, OSError, ValueError):
            pass
        return self._new()

    def _launch_store_callback(self):
        args = self.cfg["sys"].argv[1:]
        self.store_callback_token = ""
        result = {
            "configured": False,
            "url": "",
            "authTokenPresent": False,
            "authTokenArgPresent": False,
            "tokenInArgv": False,
        }
        for i, arg in enumerate(args):
            if arg in ("--store-callback-url", "--store-url") and i + 1 < len(args):
                result.update(configured=True, url=args[i + 1])
            elif arg.startswith(("--store-callback-url=", "--store-url=")):
                result.update(configured=True, url=arg.split("=", 1)[1])
            elif arg in ("--store-callback-auth-token", "--store-auth-token"):
                result["authTokenArgPresent"] = True
            elif arg.startswith(
                ("--store-callback-auth-token=", "--store-auth-token=")
            ):
                result["authTokenArgPresent"] = True
        env_url = os.environ.get("CURSOR_SDK_STORE_CALLBACK_URL", "") or os.environ.get(
            "TNY_MOCK_STORE_CALLBACK_URL", ""
        )
        if env_url:
            result.update(configured=True, url=env_url)
        env_token = os.environ.get(
            "CURSOR_SDK_STORE_CALLBACK_AUTH_TOKEN", ""
        ) or os.environ.get("TNY_MOCK_STORE_CALLBACK_AUTH_TOKEN", "")
        if env_token:
            self.store_callback_token = env_token
            result["authTokenPresent"] = True
            result["tokenInArgv"] = env_token in args
        if result["authTokenArgPresent"] or result["tokenInArgv"]:
            self.cfg["fail"]("launch: store callback token leaked into argv")
        return result

    def save(self):
        with self.lock:
            tmp = self.path + ".tmp"
            persisted = _redact(json.loads(json.dumps(self.data)))
            callback = persisted.get("toolCallback")
            if callback:
                callback["authTokenPresent"] = bool(
                    self.data["toolCallback"].get("authToken")
                )
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(persisted, f, sort_keys=True, indent=2)
            os.replace(tmp, self.path)

    def record_route(self, path):
        with self.lock:
            with open(self.routes_path, "a", encoding="utf-8") as f:
                f.write(path + "\n")


def extend_handler(base, cfg):
    state = State(cfg)
    fail = cfg["fail"]
    frame = cfg["frame"]
    envelope = cfg["envelope"]
    model_id = cfg["MODEL"]

    class CompleteHandler(base):
        ROUTE_MAP = {
            f"/sdk.v1.{service}/{method}": _snake(method)
            for service, methods in ROUTES.items()
            for method in methods
        }

        def _sdk_error(self, connect_code, message, sdk_code, http=500):
            self._json(
                http,
                {
                    "code": connect_code,
                    "message": message,
                    "details": [_detail(sdk_code, message)],
                },
            )

        def _authed(self):
            if self.headers.get("Authorization") != "Bearer " + cfg["TOKEN"]:
                fail(f"{self.path}: bridge bearer missing or incorrect")
                self._sdk_error(
                    "unauthenticated", "bad bearer", "unauthorized", http=401
                )
                return False
            if self.headers.get("Connect-Protocol-Version") != "1":
                fail(f"{self.path}: missing Connect-Protocol-Version: 1")
                self._sdk_error(
                    "invalid_argument",
                    "bad Connect protocol version",
                    "validation_error",
                    http=400,
                )
                return False
            return True

        def _api_key(self, who, options):
            expected = os.environ.get("CURSOR_API_KEY", "")
            got = options.get("apiKey") if isinstance(options, dict) else None
            if not expected or got != expected:
                fail(f"{who}: options.apiKey is missing or incorrect")
                return False
            return True

        def _stream_out(self, objects, pause_after=None):
            self.send_response(200)
            self.send_header("Content-Type", "application/connect+json")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for index, obj in enumerate(objects):
                part = envelope(0, b"") if obj is None else frame(obj)
                self._chunk(part)
                self.wfile.flush()
                if pause_after == index:
                    cfg["time"].sleep(0.05)
            self._chunk(envelope(2, b"{}"))
            self._chunk(b"")

        def _agent_or_error(self, agent_id):
            agent = state.data["agents"].get(agent_id)
            if not agent:
                self._sdk_error("not_found", "agent not found", "agent_not_found", 404)
            return agent

        def _run_or_error(self, run_id):
            run = state.data["runs"].get(run_id)
            if not run:
                self._sdk_error("not_found", "run not found", "run_not_found", 404)
            return run

        def _operation_options(self, who, req, agent=None):
            options = req.get("options") or {}
            self._api_key(who, options)
            if agent and agent["runtime"] == "local" and options.get("cwd"):
                if os.path.realpath(options["cwd"]) != cfg["EXPECT_CWD"]:
                    fail(f"{who}: options.cwd does not match workspace")
            return options

        def do_POST(self):
            if not self._authed():
                return
            handler = self.ROUTE_MAP.get(self.path)
            if not handler:
                fail(f"unexpected RPC path {self.path!r}")
                self._sdk_error(
                    "unimplemented", "no such method", "feature_unavailable", 404
                )
                return
            state.record_route(self.path)
            method = self.path.rsplit("/", 1)[-1]
            forced = os.environ.get("TNY_MOCK_ERROR_ROUTE", "")
            if forced in (method, self.path):
                self._body()
                self._sdk_error(
                    "unavailable",
                    "forced mock sdk error",
                    os.environ.get("TNY_MOCK_ERROR_CODE", "feature_unavailable"),
                    503,
                )
                return
            getattr(self, handler)()

        # Control/catalog.
        def ping(self):
            self._unary_in()
            self._json(200, {"message": "pong"})

        def version(self):
            self._unary_in()
            self._json(
                200,
                {
                    "bridgeVersion": "1.0.30-mock",
                    "protocolVersion": "sdk.v1",
                    "capabilities": CAPABILITIES,
                    "sdkVersion": "1.0.30-mock",
                    "runtime": "python-mock",
                },
            )

        get_version = version

        def set_tool_callback(self):
            req = self._unary_in()
            url, token = req.get("url", ""), req.get("authToken", "")
            if url:
                parsed = urlsplit(url)
                if parsed.scheme != "http" or parsed.hostname not in (
                    "127.0.0.1",
                    "localhost",
                    "::1",
                ):
                    fail("SetToolCallback: url must be loopback HTTP")
                if not token:
                    fail("SetToolCallback: authToken is missing")
                state.data["toolCallback"] = {"url": url, "authToken": token}
            else:
                if token:
                    fail("SetToolCallback: clearing url retained authToken")
                state.data["toolCallback"] = None
            state.save()
            self._json(200, {})

        def _catalog(self, who):
            req = self._unary_in()
            self._api_key(who, req.get("options") or {})

        def me(self):
            self._catalog("Me")
            self._json(
                200,
                {
                    "user": {
                        "apiKeyName": "mock-key",
                        "userId": "4242",
                        "userEmail": "sdk-mock@example.invalid",
                        "userFirstName": "SDK",
                        "userLastName": "Mock",
                        "createdAt": NOW,
                    }
                },
            )

        def list_models(self):
            self._catalog("ListModels")
            self._json(200, {"items": _models(model_id)})

        def list_repositories(self):
            self._catalog("ListRepositories")
            self._json(
                200,
                {
                    "items": [
                        {"url": "https://github.com/example/mock-one"},
                        {"url": "https://github.com/example/mock-two"},
                    ]
                },
            )

        # Agent lifecycle.
        def _agent_options(self, req):
            who = self.path.rsplit("/", 1)[-1]
            options = req.get("options") or {}
            self._api_key(who, options)
            selected = options.get("model") or {}
            if selected.get("id") != model_id:
                fail(f"{who}: options.model has wrong id")
            else:
                self._check_effort(who, selected)
                self._check_fast(who, selected)
            local, cloud = options.get("local"), options.get("cloud")
            if bool(local) == bool(cloud):
                fail(f"{who}: exactly one runtime is required")
            runtime = "cloud" if cloud else "local"
            if runtime == "local":
                cwd = (local or {}).get("cwd")
                if not isinstance(cwd, list) or not cwd:
                    fail(f"{who}: options.local.cwd is missing")
                elif len(cwd) > 1:
                    fail(f"{who}: options.local.cwd has multiple entries")
                elif os.path.realpath(cwd[0]) != cfg["EXPECT_CWD"]:
                    fail(f"{who}: options.local.cwd does not match workspace")
                store = (local or {}).get("store") or {}
                if store.get("type") == "custom":
                    state.data["storeCallbackConfig"] = {
                        "agentOptions": store,
                        "launch": state.data.get("storeCallback"),
                    }
            else:
                repos = (cloud or {}).get("repos") or []
                if not repos or any(
                    not isinstance(repo, dict) or not repo.get("url") for repo in repos
                ):
                    fail(f"{who}: cloud.repos[].url is required")
            return options, runtime

        def _call_store_on_create(self, agent_id, options):
            if os.environ.get("TNY_MOCK_CALL_STORE_ON_CREATE") != "1":
                return
            callback = state.data.get("storeCallback") or {}
            if not callback.get("configured") or not state.store_callback_token:
                fail("CreateAgent: synchronous store callback was not configured")
                return
            parsed = urlsplit(callback["url"])
            path = parsed.path.rstrip("/") + "/sdk.v1.SdkStoreCallbackService/CallStore"
            local = options.get("local") or {}
            body = json.dumps(
                {
                    "substore": "agents",
                    "method": "create",
                    "input": {
                        "agent": {
                            "agentId": agent_id,
                            "cwd": (local.get("cwd") or [""])[0],
                            "createdAt": NOW,
                        }
                    },
                },
                separators=(",", ":"),
            ).encode()
            conn = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=10)
            try:
                conn.request(
                    "POST",
                    path,
                    body,
                    {
                        "Authorization": "Bearer " + state.store_callback_token,
                        "Connect-Protocol-Version": "1",
                        "Content-Type": "application/json",
                    },
                )
                response = conn.getresponse()
                raw = response.read()
                if response.status != 200:
                    fail(f"CallStore: callback returned HTTP {response.status}")
                    return
                value = json.loads(raw or b"{}")
                if not isinstance(value.get("output"), dict):
                    fail("CallStore: create response lacks output object")
                    return
                with open(
                    os.path.join(cfg["DIR"], "store_callback_result.json"),
                    "w",
                    encoding="utf-8",
                ) as result_file:
                    json.dump(value, result_file, sort_keys=True)
            except (OSError, ValueError) as exc:
                fail(f"CallStore: callback failed ({type(exc).__name__})")
            finally:
                conn.close()

        def _call_store_during_stream(self, agent_id):
            if os.environ.get("TNY_MOCK_CALL_STORE_ON_SEND") != "1":
                return
            callback = state.data.get("storeCallback") or {}
            if not callback.get("configured") or not state.store_callback_token:
                fail("Send: synchronous store callback was not configured")
                return
            parsed = urlsplit(callback["url"])
            path = parsed.path.rstrip("/") + "/sdk.v1.SdkStoreCallbackService/CallStore"
            body = json.dumps(
                {
                    "substore": "agents",
                    "method": "get",
                    "input": {"agentId": agent_id},
                },
                separators=(",", ":"),
            ).encode()
            conn = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=10)
            try:
                conn.request(
                    "POST",
                    path,
                    body,
                    {
                        "Authorization": "Bearer " + state.store_callback_token,
                        "Connect-Protocol-Version": "1",
                        "Content-Type": "application/json",
                    },
                )
                response = conn.getresponse()
                value = json.loads(response.read() or b"{}")
                if response.status != 200 or not isinstance(value.get("output"), dict):
                    fail("CallStore: stream-time get failed")
                    return
                with open(
                    os.path.join(cfg["DIR"], "store_callback_stream_result.json"),
                    "w",
                    encoding="utf-8",
                ) as result_file:
                    json.dump(value, result_file, sort_keys=True)
            except (OSError, ValueError) as exc:
                fail(f"CallStore: stream callback failed ({type(exc).__name__})")
            finally:
                conn.close()

        def create_agent(self):
            req = self._unary_in()
            options, runtime = self._agent_options(req)
            idem = req.get("idempotencyKey", "")
            with state.lock:
                if idem and idem in state.data["createIdempotency"]:
                    agent_id = state.data["createIdempotency"][idem]
                    agent = state.data["agents"][agent_id]
                else:
                    if runtime == "local" and not state.data["agents"]:
                        agent_id = "agent-mock-0001"
                        state.data["nextLocal"] = 2
                    else:
                        counter = "nextCloud" if runtime == "cloud" else "nextLocal"
                        prefix = "agent-cloud" if runtime == "cloud" else "agent-local"
                        agent_id = f"{prefix}-{state.data.get(counter, 1):04d}"
                        state.data[counter] = state.data.get(counter, 1) + 1
                    agent = _new_agent(agent_id, runtime, options)
                    if (
                        runtime == "local"
                        and ((options.get("local") or {}).get("store") or {}).get(
                            "type"
                        )
                        == "custom"
                    ):
                        self._call_store_on_create(agent_id, options)
                    state.data["agents"][agent_id] = agent
                    if idem:
                        state.data["createIdempotency"][idem] = agent_id
                with open(cfg["AGENT_PATH"], "w", encoding="utf-8") as f:
                    f.write(agent_id)
                state.save()
            self._json(200, {"agentId": agent_id, "model": options["model"]})

        def resume_agent(self):
            req = self._unary_in()
            options, runtime = self._agent_options(req)
            agent = self._agent_or_error(req.get("agentId", ""))
            if not agent:
                return
            if agent.get("archived"):
                self._sdk_error(
                    "failed_precondition", "agent is archived", "agent_archived", 409
                )
                return
            if agent["runtime"] != runtime:
                fail("ResumeAgent: runtime changed")
            agent.update(options=options, closed=False)
            with open(cfg["RESUMED_PATH"], "w", encoding="utf-8") as f:
                f.write(agent["agentId"])
            state.save()
            self._json(200, {"agentId": agent["agentId"], "model": options["model"]})

        def reload_agent(self):
            req = self._unary_in()
            agent = self._agent_or_error(req.get("agentId", ""))
            if agent:
                agent["closed"] = False
                agent["reloadCount"] = agent.get("reloadCount", 0) + 1
                state.save()
                self._json(200, {})

        def close_agent(self):
            req = self._unary_in()
            agent = self._agent_or_error(req.get("agentId", ""))
            if agent:
                agent["closed"] = True
                state.save()
                self._json(200, {})

        # Run behavior.
        def send(self):
            req = self._stream_in()
            agent = self._agent_or_error(req.get("agentId", ""))
            if not agent:
                return
            if agent.get("archived"):
                self._sdk_error(
                    "failed_precondition", "agent is archived", "agent_archived", 409
                )
                return
            if agent["runtime"] == "local":
                self._call_store_during_stream(agent["agentId"])
            message, options = req.get("message") or {}, req.get("options") or {}
            if not message.get("text"):
                fail("Send: message.text is required")
            if not options.get("enableDeltas"):
                fail("Send: options.enableDeltas is not true")
            selected = options.get("model")
            if cfg["EXPECT_EFFORT"] or os.environ.get("TNY_MOCK_FAST") == "1":
                if not isinstance(selected, dict):
                    fail("Send: effort/fast requires options.model")
                else:
                    self._check_effort("Send", selected)
                    self._check_fast("Send", selected)
            elif selected is not None:
                fail("Send: unexpected options.model")
            if agent["runtime"] == "cloud" and options.get("local"):
                fail("Send: cloud agent received local options")
            if agent["runtime"] == "local" and options.get("cloud"):
                fail("Send: local agent received cloud options")
            defer_custom = bool(
                os.environ.get("TNY_MOCK_INVOKE_CUSTOM_TOOL")
                and agent["runtime"] == "local"
            )
            run = self._get_or_create_run(agent, req, selected, defer_custom)
            if defer_custom and not run.get("customComplete"):
                # A real Send stream identifies the run before it can ask the
                # adapter to execute a tool. Emit and flush that durable init
                # event first so a concurrent CancelRun has a usable runId.
                self.send_response(200)
                self.send_header("Content-Type", "application/connect+json")
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                first = dict(run["events"][0])
                first.pop("offset", None)
                self._chunk(frame(first))
                self._chunk(envelope(0, b""))
                self.wfile.flush()

                custom = self._call_custom_tool(agent, run["runId"])
                with state.lock:
                    events = _run_events(agent["agentId"], run["runId"], custom)
                    run["events"] = [
                        dict(event, offset=f"offset:{index + 1}")
                        for index, event in enumerate(events)
                    ]
                    run["customComplete"] = True
                    if run["status"] == "RUN_LIFECYCLE_STATUS_RUNNING":
                        run["status"] = "RUN_LIFECYCLE_STATUS_FINISHED"
                        run["result"] = cfg["ANSWER"]
                    state.save()
                    remaining = [dict(event) for event in run["events"][1:]]
                    status = run["status"]
                    snapshot = _run_snapshot(run)
                for event in remaining:
                    event.pop("offset", None)
                    self._chunk(frame(event))
                self._chunk(
                    frame(
                        {
                            "result": {
                                "agentId": agent["agentId"],
                                "runId": run["runId"],
                                "status": status,
                                "result": snapshot,
                            }
                        }
                    )
                )
                self._chunk(
                    frame(
                        {"done": {"agentId": agent["agentId"], "runId": run["runId"]}}
                    )
                )
                self._chunk(envelope(2, b"{}"))
                self._chunk(b"")
                return
            objects = []
            for index, event in enumerate(run["events"]):
                event = dict(event)
                event.pop("offset", None)
                objects.append(event)
                if index == 0:
                    objects.append(None)
            objects.extend(
                (
                    {
                        "result": {
                            "agentId": agent["agentId"],
                            "runId": run["runId"],
                            "status": run["status"],
                            "result": _run_snapshot(run),
                        }
                    },
                    {"done": {"agentId": agent["agentId"], "runId": run["runId"]}},
                )
            )
            self._stream_out(objects, pause_after=2)

        def _get_or_create_run(self, agent, req, selected, defer_custom=False):
            idem = req.get("idempotencyKey", "")
            key = f"{agent['agentId']}:{idem}" if idem else ""
            if key and key in state.data["sendIdempotency"]:
                return state.data["runs"][state.data["sendIdempotency"][key]]
            run_id = f"run-mock-{state.data.get('nextRun', 1)}"
            state.data["nextRun"] = state.data.get("nextRun", 1) + 1
            custom = None if defer_custom else self._call_custom_tool(agent, run_id)
            events = _run_events(agent["agentId"], run_id, custom)
            usage = {
                "inputTokens": 111,
                "outputTokens": 22,
                "cacheReadTokens": 7,
                "cacheWriteTokens": 3,
                "totalTokens": 133,
                "reasoningTokens": 4,
            }
            run = {
                "runId": run_id,
                "agentId": agent["agentId"],
                "status": (
                    "RUN_LIFECYCLE_STATUS_RUNNING"
                    if defer_custom
                    else "RUN_LIFECYCLE_STATUS_FINISHED"
                ),
                "result": "" if defer_custom else cfg["ANSWER"],
                "model": selected or agent["options"]["model"],
                "durationMs": 5,
                "createdAt": NOW,
                "usage": usage,
                "events": [
                    dict(event, offset=f"offset:{index + 1}")
                    for index, event in enumerate(events)
                ],
                "customComplete": not defer_custom,
                "conversation": {
                    "agentId": agent["agentId"],
                    "runId": run_id,
                    "messages": [
                        {"role": "user", "text": req["message"]["text"]},
                        {"role": "assistant", "text": cfg["ANSWER"]},
                    ],
                },
            }
            state.data["runs"][run_id] = run
            agent["runs"].append(run_id)
            agent["messages"].extend(
                _agent_messages(
                    agent["agentId"], run_id, req["message"]["text"], cfg["ANSWER"]
                )
            )
            agent["summary"] = cfg["ANSWER"]
            agent["lastModified"] = (
                datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
            )
            if key:
                state.data["sendIdempotency"][key] = run_id
            state.save()
            return run

        def _call_custom_tool(self, agent, run_id):
            name = os.environ.get("TNY_MOCK_INVOKE_CUSTOM_TOOL", "")
            if not name or agent["runtime"] != "local":
                return None
            callback = state.data.get("toolCallback")
            if not callback:
                fail("Send: custom tool requested without SetToolCallback")
                return None
            tools = agent["options"].get("local", {}).get("customTools", {})
            if name not in tools:
                fail("Send: requested tool absent from local.customTools")
            parsed = urlsplit(callback["url"])
            path = (
                parsed.path.rstrip("/")
                + "/sdk.v1.SdkCustomToolCallbackService/CallCustomTool"
            )
            body = json.dumps(
                {
                    "toolName": name,
                    "args": {"text": "mock custom tool input", "count": 1},
                    "toolCallId": "custom-call-1",
                    "agentId": agent["agentId"],
                },
                separators=(",", ":"),
            ).encode()
            conn = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=10)
            try:
                conn.request(
                    "POST",
                    path,
                    body,
                    {
                        "Authorization": "Bearer " + callback["authToken"],
                        "Connect-Protocol-Version": "1",
                        "Content-Type": "application/json",
                    },
                )
                response = conn.getresponse()
                raw = response.read()
                if response.status != 200:
                    fail(f"CallCustomTool: callback returned HTTP {response.status}")
                    return None
                value = json.loads(raw or b"{}")
                if not isinstance(value.get("result"), dict):
                    fail("CallCustomTool: response lacks result object")
                    return None
                record = {
                    "toolName": name,
                    "agentId": agent["agentId"],
                    "runId": run_id,
                    "result": value["result"],
                }
                with open(state.tool_result_path, "w", encoding="utf-8") as f:
                    json.dump(record, f, sort_keys=True)
                return record
            except (OSError, ValueError) as exc:
                fail(f"CallCustomTool: callback failed ({type(exc).__name__})")
                return None
            finally:
                conn.close()

        def wait_live_run(self):
            req = self._unary_in()
            run = self._run_or_error(req.get("runId", ""))
            if run:
                self._json(200, {"result": _run_snapshot(run)})

        def get_run(self):
            req = self._unary_in()
            run = self._run_or_error(req.get("runId", ""))
            if run:
                self._operation_options(
                    "GetRun", req, state.data["agents"].get(run["agentId"])
                )
                self._json(200, {"run": _run_snapshot(run)})

        def list_runs(self):
            req = self._unary_in()
            agent = self._agent_or_error(req.get("agentId", ""))
            if not agent:
                return
            options = self._operation_options("ListRuns", req, agent)
            items = [
                _run_snapshot(state.data["runs"][run_id])
                for run_id in reversed(agent["runs"])
            ]
            items, cursor = _page(items, options)
            self._json(200, {"items": items, "nextCursor": cursor})

        def get_run_conversation(self):
            req = self._unary_in()
            run = self._run_or_error(req.get("runId", ""))
            if run:
                self._json(
                    200,
                    {
                        "conversationJson": json.dumps(
                            run["conversation"], separators=(",", ":")
                        )
                    },
                )

        def observe_run(self):
            req = self._stream_in()
            run = self._run_or_error(req.get("runId", ""))
            if not run:
                return
            try:
                start = int(req.get("afterOffset", "").split(":", 1)[1])
            except (IndexError, ValueError):
                start = 0
            events = run["events"][start:]
            events.extend(
                (
                    {
                        "result": {
                            "agentId": run["agentId"],
                            "runId": run["runId"],
                            "status": run["status"],
                            "result": _run_snapshot(run),
                        },
                        "offset": f"offset:{len(run['events']) + 1}",
                    },
                    {
                        "done": {"agentId": run["agentId"], "runId": run["runId"]},
                        "offset": f"offset:{len(run['events']) + 2}",
                    },
                )
            )
            self._stream_out(events)

        def cancel_run(self):
            req = self._unary_in()
            run = self._run_or_error(req.get("runId", ""))
            if not run:
                return
            if req.get("agentId") and req["agentId"] != run["agentId"]:
                fail("CancelRun: agentId hint does not own run")
            if run["status"] == "RUN_LIFECYCLE_STATUS_CANCELLED":
                self._sdk_error(
                    "failed_precondition",
                    "run not cancellable",
                    "run_not_cancellable",
                    409,
                )
                return
            run.update(status="RUN_LIFECYCLE_STATUS_CANCELLED", result="")
            state.save()
            self._json(200, {})

        # Agent management/artifacts.
        def get_agent(self):
            req = self._unary_in()
            agent = self._agent_or_error(req.get("agentId", ""))
            if agent:
                self._operation_options("GetAgent", req, agent)
                self._json(200, {"agent": _agent_info(agent)})

        def list_agents(self):
            req = self._unary_in()
            options = self._operation_options("ListAgents", req)
            runtime = options.get("runtime", "RUNTIME_UNSPECIFIED")
            items = []
            for agent in reversed(list(state.data["agents"].values())):
                if agent["archived"] and not options.get("includeArchived", False):
                    continue
                if runtime in ("RUNTIME_LOCAL", 2) and agent["runtime"] != "local":
                    continue
                if runtime in ("RUNTIME_CLOUD", 3) and agent["runtime"] != "cloud":
                    continue
                info = _agent_info(agent)
                if (
                    options.get("cwd")
                    and info.get("local", {}).get("cwd") != options["cwd"]
                ):
                    continue
                items.append(info)
            items, cursor = _page(items, options)
            self._json(200, {"items": items, "nextCursor": cursor})

        def _archive(self, value):
            req = self._unary_in()
            agent = self._agent_or_error(req.get("agentId", ""))
            if agent:
                self._operation_options(self.path.rsplit("/", 1)[-1], req, agent)
                agent["archived"] = value
                state.save()
                self._json(200, {})

        def archive_agent(self):
            self._archive(True)

        def unarchive_agent(self):
            self._archive(False)

        def delete_agent(self):
            req = self._unary_in()
            agent = self._agent_or_error(req.get("agentId", ""))
            if not agent:
                return
            self._operation_options("DeleteAgent", req, agent)
            for run_id in agent["runs"]:
                state.data["runs"].pop(run_id, None)
            state.data["agents"].pop(agent["agentId"], None)
            state.save()
            self._json(200, {})

        def list_agent_messages(self):
            req = self._unary_in()
            agent = self._agent_or_error(req.get("agentId", ""))
            if not agent:
                return
            options = self._operation_options("ListAgentMessages", req, agent)
            start = int(options.get("offset", 0) or 0)
            limit = min(max(int(options.get("limit", 50) or 50), 1), 100)
            self._json(200, {"messages": agent["messages"][start : start + limit]})

        def list_artifacts(self):
            req = self._unary_in()
            agent = self._agent_or_error(req.get("agentId", ""))
            if not agent:
                return
            items = []
            for path, encoded in sorted(agent["artifacts"].items()):
                items.append(
                    {
                        "path": path,
                        "sizeBytes": str(len(base64.b64decode(encoded))),
                        "updatedAt": NOW,
                    }
                )
            self._json(200, {"artifacts": items})

        def download_artifact(self):
            req = self._stream_in()
            agent = self._agent_or_error(req.get("agentId", ""))
            if not agent:
                return
            encoded = agent["artifacts"].get(req.get("path", ""))
            if encoded is None:
                self._sdk_error(
                    "not_found", "artifact not found", "validation_error", 404
                )
                return
            forced = os.environ.get("TNY_MOCK_ARTIFACT_BASE64")
            if forced is not None:
                self._stream_out([{"data": forced}])
                return
            if os.environ.get("TNY_MOCK_ARTIFACT_OVERSIZE") == "1":
                large = base64.b64encode(b"x" * 4_300_000).decode()
                self._stream_out([{"data": large}, {"data": large}])
                return
            raw = base64.b64decode(encoded)
            self._stream_out(
                [
                    {"data": base64.b64encode(raw[i : i + 7]).decode()}
                    for i in range(0, len(raw), 7)
                ]
            )

        def get_usage(self):
            req = self._unary_in()
            agent = self._agent_or_error(req.get("agentId", ""))
            if not agent:
                return
            if agent["runtime"] != "cloud":
                self._sdk_error(
                    "unavailable", "usage is cloud-only", "feature_unavailable", 400
                )
                return
            run_ids = agent["runs"]
            if req.get("runId"):
                run_ids = [run_id for run_id in run_ids if run_id == req["runId"]]
            runs = []
            totals = {key: 0 for key in _USAGE_KEYS}
            for run_id in run_ids:
                usage = state.data["runs"][run_id]["usage"]
                for key in _USAGE_KEYS:
                    totals[key] += int(usage.get(key, 0))
                runs.append(
                    {
                        "runId": run_id,
                        "usage": _json_usage(usage),
                        "cost": {"rawCostCents": 1.5, "chargedCents": 1.0},
                    }
                )
            self._json(
                200,
                {
                    "usage": {
                        "usage": _json_usage(totals),
                        "cost": {
                            "rawCostCents": 1.5 * len(runs),
                            "chargedCents": 1.0 * len(runs),
                        },
                        "runs": runs,
                    }
                },
            )

    assert len(CompleteHandler.ROUTE_MAP) == 27
    cfg["COMPLETE_MOCK_STATE"] = state
    return CompleteHandler


def _snake(name):
    out = []
    for char in name:
        if char.isupper() and out:
            out.append("_")
        out.append(char.lower())
    return "".join(out)


def _redact(value):
    if isinstance(value, list):
        return [_redact(item) for item in value]
    if not isinstance(value, dict):
        return value
    result = {}
    for key, item in value.items():
        if key in ("apiKey", "authToken", "clientSecret"):
            result[key] = "<redacted>" if item else ""
        else:
            result[key] = _redact(item)
    return result


def _models(model_id):
    values = [
        {"value": value, "displayName": value.title()}
        for value in ("low", "medium", "high", "max")
    ]
    return [
        {
            "id": model_id,
            "displayName": "Mock Cursor Model",
            "description": "Deterministic integration model",
            "parameters": [
                {"id": "effort", "displayName": "Reasoning Effort", "values": values},
                {
                    "id": "fast",
                    "displayName": "Fast",
                    "values": [{"value": "true"}, {"value": "false"}],
                },
            ],
            "variants": [
                {
                    "params": [{"id": "fast", "value": "false"}],
                    "displayName": "Default",
                    "isDefault": True,
                },
                {
                    "params": [{"id": "fast", "value": "true"}],
                    "displayName": "Fast",
                    "description": "Mock fast tier",
                },
            ],
        },
        {"id": "mock-cursor-model-2", "displayName": "Mock Model 2"},
    ]


def _new_agent(agent_id, runtime, options):
    agent = {
        "agentId": agent_id,
        "runtime": runtime,
        "name": options.get("name", ""),
        "summary": "",
        "options": options,
        "status": "AGENT_INFO_STATUS_FINISHED",
        "archived": False,
        "closed": False,
        "createdAt": NOW,
        "lastModified": NOW,
        "messages": [],
        "runs": [],
        "artifacts": {},
    }
    if runtime == "cloud":
        content = f"artifact from {agent_id}\n".encode()
        agent["artifacts"]["mock/result.txt"] = base64.b64encode(content).decode()
    return agent


def _agent_info(agent):
    info = {
        "agentId": agent["agentId"],
        "name": agent["name"],
        "summary": agent["summary"],
        "lastModified": agent["lastModified"],
        "status": agent["status"],
        "createdAt": agent["createdAt"],
        "archived": agent["archived"],
    }
    if agent["runtime"] == "cloud":
        cloud = agent["options"]["cloud"]
        info["cloud"] = {
            "env": cloud.get(
                "env", {"type": "CLOUD_ENVIRONMENT_TYPE_CLOUD", "name": "mock"}
            ),
            "repos": [repo["url"] for repo in cloud.get("repos", [])],
            "metadata": cloud.get("metadata", {}),
        }
    else:
        info["local"] = {"cwd": agent["options"]["local"]["cwd"][0]}
    return info


_USAGE_KEYS = (
    "inputTokens",
    "outputTokens",
    "cacheReadTokens",
    "cacheWriteTokens",
    "totalTokens",
    "reasoningTokens",
)


def _json_usage(usage):
    return {key: str(usage.get(key, 0)) for key in _USAGE_KEYS}


def _run_snapshot(run):
    return {
        "runId": run["runId"],
        "agentId": run["agentId"],
        "status": run["status"],
        "result": run["result"],
        "model": run["model"],
        "durationMs": str(run["durationMs"]),
        "git": {"branches": []},
        "createdAt": run["createdAt"],
        "usage": _json_usage(run["usage"]),
    }


def _page(items, options):
    cursor = options.get("cursor", "")
    try:
        start = int(cursor.split(":", 1)[1]) if cursor.startswith("offset:") else 0
    except ValueError:
        start = 0
    limit = min(max(int(options.get("limit", 50) or 50), 1), 100)
    page = items[start : start + limit]
    next_cursor = f"offset:{start + limit}" if start + limit < len(items) else ""
    return page, next_cursor


def _agent_messages(agent_id, run_id, prompt, answer):
    return [
        {
            "type": "user",
            "uuid": f"msg-user-{run_id}",
            "agentId": agent_id,
            "message": {"text": prompt},
        },
        {
            "type": "assistant",
            "uuid": f"msg-assistant-{run_id}",
            "agentId": agent_id,
            "message": {"text": answer},
        },
    ]


def _run_events(agent_id, run_id, custom=None):
    def sdk(kind, payload):
        return {
            "sdkMessage": {
                "type": kind,
                "message": {
                    "type": kind,
                    "agent_id": agent_id,
                    "run_id": run_id,
                    **payload,
                },
            }
        }

    running = sdk(
        "tool_call",
        {
            "call_id": "tc1",
            "name": "read",
            "status": "running",
            "args": {"path": "README.md"},
        },
    )
    events = [
        sdk("status", {"status": "RUNNING"}),
        sdk(
            "assistant",
            {
                "message": {
                    "role": "assistant",
                    "content": [{"type": "text", "text": "CURSOR-"}],
                }
            },
        ),
        sdk("thinking", {"text": "considering"}),
        running,
        running,
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
    ]
    if custom:
        events.append(
            sdk(
                "tool_call",
                {
                    "call_id": "custom-call-1",
                    "name": custom["toolName"],
                    "status": "completed",
                    "args": {"text": "mock custom tool input", "count": 1},
                    "result": {"status": "success", "value": custom["result"]},
                },
            )
        )
    events.extend(
        (
            sdk(
                "assistant",
                {
                    "message": {
                        "role": "assistant",
                        "content": [{"type": "text", "text": "MOCK-OK"}],
                    }
                },
            ),
            {"interactionUpdate": {"type": "text_delta", "update": {"text": "OK"}}},
            {"step": {"type": "assistant", "step": {"text": "CURSOR-MOCK-OK"}}},
            sdk("status", {"status": "RUNNING", "message": "wrapping up"}),
            sdk(
                "usage",
                {
                    "usage": {
                        "inputTokens": 111,
                        "outputTokens": 22,
                        "totalTokens": 133,
                    }
                },
            ),
            sdk("status", {"status": "FINISHED"}),
        )
    )
    return events
