#!/usr/bin/env python3
"""Mock OpenAI-compatible provider for tny integration tests.

Serves BOTH wires (docs/adr/0016):
  POST /v1/responses          — the default Responses API wire (typed SSE)
  POST /v1/chat/completions   — the legacy chat wire (wire_api "chat")
  GET  /v1/models

Both wires run the same scenario and validate the request strictly:
Turn 1 streams a list_files tool call with the JSON arguments fragmented
across SSE events; turn 2 streams a text answer mentioning what the tool
returned. By default SSE rides chunked transfer-encoding and the responses
stream is deliberately re-chunked at arbitrary byte boundaries so event
reassembly is exercised end to end (real transports split anywhere). A chunk
width at least as large as the body selects reusable Content-Length framing.

Env knobs:
  MOCK_EXPECT_WIRE    responses|chat — the other endpoint 400s (proves tny
                      picked the right wire, not just a working one)
  MOCK_EXPECT_EFFORT  every request must carry exactly this effort
                      (chat: reasoning_effort, responses: reasoning.effort);
                      unset means the field must be absent
  MOCK_SLOW_MS        delay before the first response (TUI steer tests)
  MOCK_EXPECT_STEER   the follow-up request must END with a user message of
                      exactly this text (steer rides after the tool result)
  MOCK_FAIL_RESPONSE  responses wire: turn 1 ends in a response.failed
                      event carrying this message (error-path test)
  MOCK_CHAT_ERROR     chat wire: HTTP 200 SSE contains a top-level provider
                      error followed by the ordinary stream terminator
  MOCK_INCOMPLETE     responses wire: the final answer ends in
                      response.incomplete (token cutoff) — the partial text
                      is preserved and the stop reason remains non-success
  MOCK_TRUNCATED_TERMINAL
                      responses wire: omit the final chunk and close after the
                      terminal event, proving the dead socket is discarded
  MOCK_DROP_REUSED_ONCE
                      responses wire: close the first tool-output POST before
                      headers, exercising the reused-connection read retry
  MOCK_CONNECTION_CLOSE
                      send complete responses with Connection: close
  MOCK_CHUNK_WIDTH    responses wire HTTP chunk width (default 17); a value at
                      least as large as the body selects Content-Length
  MOCK_CUSTOM_TOOL    request this custom tool once, then validate its output
  MOCK_CUSTOM_ARGUMENTS
                      JSON arguments for MOCK_CUSTOM_TOOL

The responses wire streams TWO parallel tool calls (list_files +
glob_files). The second one's output_item.added carries only the item id;
call_id, name arrive in output_item.done, whose empty `arguments` must
never wipe the delta-assembled string. Junk events ride along (items
without a type, deltas without/with empty payloads, an unknown
output_index) — tny must skip them without crashing. Every responses
stream normally ends with a valid final chunk and remains reusable. The
explicit truncated-terminal mode closes abruptly after the terminal event:
completeness still comes from response.completed/failed, not the transport.

Usage: mock_openai.py [port] [certfile keyfile]
With certfile/keyfile the mock serves HTTPS (used by test_https.py).
"""

import json
import os
import socket
import ssl
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

EXPECT_WIRE = os.environ.get("MOCK_EXPECT_WIRE")
EXPECT_EFFORT = os.environ.get("MOCK_EXPECT_EFFORT")
SLOW_MS = int(os.environ.get("MOCK_SLOW_MS", "0"))
EXPECT_STEER = os.environ.get("MOCK_EXPECT_STEER")
# substring every system preamble must contain (docs/adr/0022 remote banner)
EXPECT_INSTRUCTIONS = os.environ.get("MOCK_EXPECT_INSTRUCTIONS")
REJECT_INSTRUCTIONS = os.environ.get("MOCK_REJECT_INSTRUCTIONS")
FAIL_RESPONSE = os.environ.get("MOCK_FAIL_RESPONSE")
CHAT_ERROR = os.environ.get("MOCK_CHAT_ERROR")
# MOCK_PARALLEL: turn 1 streams THREE parallel tool calls in the gateway
# shape from the field: the third call reuses the second call's "index" but
# carries its own fresh "id" (index-keyed assembly used to merge the two and
# drop an id, unpairing the transcript).
PARALLEL = os.environ.get("MOCK_PARALLEL") == "1"
SENSITIVE = os.environ.get("MOCK_SENSITIVE") == "1"
HTTP_STATUS = int(os.environ.get("MOCK_HTTP_STATUS", "0"))
ERROR_SECRET = os.environ.get("MOCK_ERROR_SECRET", "mock status failure")
TRUNCATED_TERMINAL = os.environ.get("MOCK_TRUNCATED_TERMINAL") == "1"
DROP_REUSED_ONCE = os.environ.get("MOCK_DROP_REUSED_ONCE") == "1"
CONNECTION_CLOSE = os.environ.get("MOCK_CONNECTION_CLOSE") == "1"
CHUNK_WIDTH = int(os.environ.get("MOCK_CHUNK_WIDTH", "17"))
if CHUNK_WIDTH < 1:
    raise ValueError("MOCK_CHUNK_WIDTH must be positive")
_drop_reused_done = False
EXPECT_EXTENSION_REWRITE = os.environ.get("MOCK_EXPECT_EXTENSION_REWRITE") == "1"
EXPECT_TOOL_OUTPUT = os.environ.get("MOCK_EXPECT_TOOL_OUTPUT")
CUSTOM_TOOL = os.environ.get("MOCK_CUSTOM_TOOL")
CUSTOM_ARGUMENTS = os.environ.get("MOCK_CUSTOM_ARGUMENTS")


def sse(obj):
    return f"data: {json.dumps(obj)}\n\n".encode()


def sse_typed(obj):
    """Responses events carry both the event: line and a type member."""
    return (f"event: {obj['type']}\ndata: {json.dumps(obj)}\n\n").encode()


class BadRequest(Exception):
    pass


def need(cond, msg):
    if not cond:
        raise BadRequest(msg)


def unpaired_tool_calls(messages):
    """What a strict provider validates: every id in an assistant message's
    tool_calls needs a role:tool message with that tool_call_id before the
    next assistant message, ids must be unique, arguments must be JSON."""
    problems = []
    for i, m in enumerate(messages):
        if m.get("role") != "assistant" or not m.get("tool_calls"):
            continue
        ids = [tc["id"] for tc in m["tool_calls"]]
        if len(ids) != len(set(ids)):
            problems.append(f"duplicate tool_call ids {ids}")
        for tc in m["tool_calls"]:
            try:
                json.loads(tc["function"]["arguments"] or "{}")
            except ValueError:
                problems.append(
                    "arguments of %s are not JSON: %r"
                    % (tc["id"], tc["function"]["arguments"])
                )
        seen = set()
        for mm in messages[i + 1 :]:
            if mm.get("role") == "assistant":
                break
            if mm.get("role") == "tool":
                seen.add(mm.get("tool_call_id"))
        for cid in ids:
            if cid not in seen:
                problems.append(f"no tool output found for function call {cid}")
    return problems


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def log_message(self, *a):
        pass

    def _chunk(self, data: bytes):
        self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")
        # Keep-alive responses cannot rely on connection teardown to flush the
        # fixture's writer. This is intentionally explicit because hosted
        # macOS/Python combinations otherwise defer chunks until the client's
        # stale-response deadline even though the handler has produced them.
        self.wfile.flush()

    def _cors(self):
        # the browser wasm build (docs/adr/0017) calls this mock cross-origin;
        # a CORS-open gateway is exactly what the page documents as required
        self.send_header("Access-Control-Allow-Origin", "*")

    def _finish_close(self):
        if CONNECTION_CLOSE and not DROP_REUSED_ONCE:
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.connection.close()

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self._cors()
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if CONNECTION_CLOSE and not DROP_REUSED_ONCE:
            self.send_header("Connection", "close")
            self.close_connection = True
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()
        self._finish_close()

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header(
            "Access-Control-Allow-Headers", "authorization, content-type, accept"
        )
        self.send_header("Access-Control-Max-Age", "600")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _reject(self, msg):
        self._json(400, {"error": {"message": msg}})

    def _start_stream(self, content_length: int | None = None):
        self.send_response(200)
        self._cors()
        self.send_header("Content-Type", "text/event-stream")
        if content_length is None:
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Content-Length", str(content_length))
        if CONNECTION_CLOSE and not DROP_REUSED_ONCE:
            self.send_header("Connection", "close")
            self.close_connection = True
        self.end_headers()

    def do_GET(self):
        if self.path.endswith("/models"):
            self._json(200, {"data": [{"id": "mock-model-1"}, {"id": "mock-model-2"}]})
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(n))
        if HTTP_STATUS:
            self._json(HTTP_STATUS, {"error": {"message": ERROR_SECRET}})
            return
        try:
            if self.path.endswith("/chat/completions"):
                need(
                    EXPECT_WIRE != "responses",
                    "hit /chat/completions but the responses wire was expected",
                )
                self._post_chat(req)
            elif self.path.endswith("/responses"):
                need(
                    EXPECT_WIRE != "chat",
                    "hit /responses but the chat wire was expected",
                )
                self._post_responses(req)
            else:
                self._reject(f"unknown endpoint {self.path}")
        except BadRequest as e:
            self._reject(str(e))

    # ---- legacy chat wire ----

    def _post_chat(self, req):
        need(
            req.get("reasoning_effort") == EXPECT_EFFORT,
            f"reasoning_effort is {req.get('reasoning_effort')!r}, "
            f"want {EXPECT_EFFORT!r}",
        )
        need("input" not in req, "chat request must not carry input items")
        msgs = req.get("messages")
        need(isinstance(msgs, list) and msgs, "messages missing")
        need(msgs[0].get("role") == "system", "no system preamble")
        for t in req.get("tools", []):
            need("function" in t, "chat tools must nest under function")
        has_tool_result = any(m.get("role") == "tool" for m in msgs)
        need(not unpaired_tool_calls(msgs), "; ".join(unpaired_tool_calls(msgs)))
        if not has_tool_result and SLOW_MS:
            time.sleep(SLOW_MS / 1000.0)
        if has_tool_result and EXPECT_STEER:
            last = msgs[-1]
            need(
                last.get("role") == "user" and last.get("content") == EXPECT_STEER,
                f"steer: last message is {last!r}, want user {EXPECT_STEER!r}",
            )

        structured = req.get("response_format")
        if structured is not None:
            need(structured.get("type") == "json_schema", f"bad {structured}")
            need("schema" in structured["json_schema"], f"bad {structured}")
            need("name" in structured["json_schema"], f"bad {structured}")

        if CHAT_ERROR:
            frames = [
                {
                    "error": {"code": 502, "message": CHAT_ERROR},
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "error"}],
                }
            ]
        elif SENSITIVE and not has_tool_result:
            frames = [
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {
                                "role": "assistant",
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "id": "sensitive_1",
                                        "type": "function",
                                        "function": {
                                            "name": "write_file",
                                            "arguments": '{"path":"permission.txt","content":"allowed"}',
                                        },
                                    }
                                ],
                            },
                        }
                    ]
                },
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]},
            ]
        elif PARALLEL and not has_tool_result:
            frames = [
                # call 1: id+name first, arguments fragmented, keyed by index
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {
                                "role": "assistant",
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "id": "par_A",
                                        "type": "function",
                                        "function": {
                                            "name": "read_file",
                                            "arguments": "",
                                        },
                                    }
                                ],
                            },
                        }
                    ]
                },
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "function": {"arguments": '{"path":"a.txt"}'},
                                    }
                                ]
                            },
                        }
                    ]
                },
                # call 2: complete in one chunk
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {
                                "tool_calls": [
                                    {
                                        "index": 1,
                                        "id": "par_B",
                                        "type": "function",
                                        "function": {
                                            "name": "read_file",
                                            "arguments": '{"path":"b.txt"}',
                                        },
                                    }
                                ]
                            },
                        }
                    ]
                },
                # call 3: REUSES index 1 with a fresh id (the gateway bug)
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {
                                "tool_calls": [
                                    {
                                        "index": 1,
                                        "id": "par_C",
                                        "type": "function",
                                        "function": {
                                            "name": "list_files",
                                            "arguments": '{"path":"."}',
                                        },
                                    }
                                ]
                            },
                        }
                    ]
                },
                {
                    "choices": [
                        {"index": 0, "delta": {}, "finish_reason": "tool_calls"}
                    ],
                    "usage": {"prompt_tokens": 100, "completion_tokens": 30},
                },
            ]
        elif PARALLEL:
            by_id = {
                m.get("tool_call_id"): m["content"]
                for m in req["messages"]
                if m.get("role") == "tool"
            }
            ok = (
                "aaa" in by_id.get("par_A", "")
                and "bbb" in by_id.get("par_B", "")
                and "a.txt" in by_id.get("par_C", "")
            )
            text = "PARALLEL-OK" if ok else f"PARALLEL-BAD {by_id!r}"
            frames = [
                {"choices": [{"index": 0, "delta": {"content": text}}]},
                {
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                    "usage": {"prompt_tokens": 200, "completion_tokens": 20},
                },
            ]
        elif not has_tool_result:
            frames = [
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {
                                "role": "assistant",
                                "tool_calls": [
                                    {
                                        "index": 0,
                                        "id": "call_1",
                                        "type": "function",
                                        "function": {
                                            "name": "list_files",
                                            "arguments": "",
                                        },
                                    }
                                ],
                            },
                        }
                    ]
                },
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {
                                "tool_calls": [
                                    {"index": 0, "function": {"arguments": '{"pa'}}
                                ]
                            },
                        }
                    ]
                },
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {
                                "tool_calls": [
                                    {"index": 0, "function": {"arguments": 'th": "."}'}}
                                ]
                            },
                        }
                    ]
                },
                {
                    "choices": [
                        {"index": 0, "delta": {}, "finish_reason": "tool_calls"}
                    ],
                    "usage": {"prompt_tokens": 100, "completion_tokens": 10},
                },
            ]
        else:
            tool_msg = next(m for m in msgs if m.get("role") == "tool")
            text = self._answer_text(req, tool_msg["content"], structured is not None)
            frames = []
            for i in range(0, len(text), 7):
                frames.append(
                    {"choices": [{"index": 0, "delta": {"content": text[i : i + 7]}}]}
                )
            frames.append(
                {
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                    "usage": {"prompt_tokens": 200, "completion_tokens": 20},
                }
            )
        pieces = [*(sse(f) for f in frames), b"data: [DONE]\n\n"]
        if CONNECTION_CLOSE and not DROP_REUSED_ONCE:
            wire = b"".join(pieces)
            self._start_stream(len(wire))
            self.wfile.write(wire)
            self.wfile.flush()
            self._finish_close()
        else:
            self._start_stream()
            for piece in pieces:
                self._chunk(piece)
            self._chunk(b"")  # final chunk

    # ---- Responses API wire ----

    def _post_responses(self, req):
        global _drop_reused_done
        need("messages" not in req, "responses request must not carry messages")
        need(req.get("store") is False, "responses request must send store:false")
        need(req.get("stream") is True, "responses request must stream")
        need(
            "reasoning_effort" not in req,
            "reasoning_effort is a chat member; responses use reasoning.effort",
        )
        effort = (req.get("reasoning") or {}).get("effort")
        need(
            effort == EXPECT_EFFORT,
            f"reasoning.effort is {effort!r}, want {EXPECT_EFFORT!r}",
        )
        instructions = req.get("instructions")
        need(
            isinstance(instructions, str) and "tny" in instructions,
            "instructions must carry the system preamble",
        )
        if EXPECT_INSTRUCTIONS:
            for part in EXPECT_INSTRUCTIONS.split("\n"):
                if part:
                    need(
                        part in instructions,
                        f"instructions lack {part!r}",
                    )
        if REJECT_INSTRUCTIONS:
            for part in REJECT_INSTRUCTIONS.split("\n"):
                if part:
                    need(
                        part not in instructions,
                        f"instructions must not contain {part!r}",
                    )
        items = req.get("input")
        need(isinstance(items, list) and items, "input items missing")
        for t in req.get("tools", []):
            need(
                t.get("type") == "function" and "name" in t and "function" not in t,
                f"responses tools must be flat: {t}",
            )

        structured = (req.get("text") or {}).get("format")
        if structured is not None:
            need(structured.get("type") == "json_schema", f"bad {structured}")
            need("json_schema" not in structured, f"not flattened: {structured}")
            need("schema" in structured and "name" in structured, f"bad {structured}")

        outputs = [i for i in items if i.get("type") == "function_call_output"]
        if outputs and EXPECT_TOOL_OUTPUT is not None:
            need(
                outputs[0].get("output") == EXPECT_TOOL_OUTPUT,
                f"effective tool output is {outputs[0].get('output')!r}, "
                f"want {EXPECT_TOOL_OUTPUT!r}",
            )
        if DROP_REUSED_ONCE and outputs and not _drop_reused_done:
            # Deterministic read-side stale keep-alive: the first request has
            # completed cleanly, this reused POST reaches the server, and the
            # connection dies before a response byte. The client's one retry
            # must reopen and replay this same POST.
            _drop_reused_done = True
            self.close_connection = True
            return
        if not outputs and SLOW_MS:
            time.sleep(SLOW_MS / 1000.0)
        if outputs and EXPECT_STEER:
            last = items[-1]
            need(
                last.get("role") == "user" and last.get("content") == EXPECT_STEER,
                f"steer: last item is {last!r}, want user {EXPECT_STEER!r}",
            )

        # noise every stream carries: an item with no type, deltas with a
        # missing / empty payload, and a delta for an output_index that was
        # never announced — all must be skipped without crashing
        junk = [
            {
                "type": "response.output_item.added",
                "output_index": 7,
                "item": {"id": "mystery"},
            },
            {"type": "response.output_text.delta", "output_index": 7},
            {"type": "response.output_text.delta", "output_index": 7, "delta": ""},
            {"type": "response.reasoning_summary_text.delta", "output_index": 7},
            {
                "type": "response.function_call_arguments.delta",
                "item_id": "fc_ghost",
                "output_index": 9,
                "delta": '{"x":1}',
            },
            {
                "type": "response.function_call_arguments.delta",
                "item_id": "fc_ghost",
                "output_index": 9,
            },
        ]

        if not outputs:
            if FAIL_RESPONSE:
                events = [
                    {"type": "response.created", "response": {"status": "in_progress"}},
                    {
                        "type": "response.failed",
                        "response": {
                            "status": "failed",
                            "error": {"code": "server_error", "message": FAIL_RESPONSE},
                        },
                    },
                ]
            elif CUSTOM_TOOL:
                custom_arguments = CUSTOM_ARGUMENTS or (
                    os.environ.get("MOCK_HALLUCINATED_ARGUMENTS")
                    if CUSTOM_TOOL == "terminal"
                    else None
                )
                item = {
                    "type": "function_call",
                    "id": "fc_custom",
                    "call_id": "custom_1",
                    "name": CUSTOM_TOOL,
                    "arguments": custom_arguments or '{"value":"hello"}',
                }
                events = [
                    {"type": "response.created", "response": {"status": "in_progress"}},
                    {
                        "type": "response.output_item.added",
                        "output_index": 0,
                        "item": item,
                    },
                    {
                        "type": "response.output_item.done",
                        "output_index": 0,
                        "item": dict(item, status="completed"),
                    },
                    {
                        "type": "response.completed",
                        "response": {
                            "status": "completed",
                            "usage": {"input_tokens": 10, "output_tokens": 2},
                        },
                    },
                ]
            elif SENSITIVE:
                item = {
                    "type": "function_call",
                    "id": "fc_sensitive",
                    "call_id": "sensitive_1",
                    "name": "write_file",
                    "arguments": '{"path":"permission.txt","content":"allowed"}',
                }
                events = [
                    {"type": "response.created", "response": {"status": "in_progress"}},
                    {
                        "type": "response.output_item.added",
                        "output_index": 0,
                        "item": item,
                    },
                    {
                        "type": "response.output_item.done",
                        "output_index": 0,
                        "item": dict(item, status="completed"),
                    },
                    {
                        "type": "response.completed",
                        "response": {
                            "status": "completed",
                            "usage": {"input_tokens": 100, "output_tokens": 10},
                        },
                    },
                ]
            else:
                # call 1: full metadata up front, arguments fragmented in
                # deltas, output_item.done repeats the complete string
                item1 = {
                    "type": "function_call",
                    "id": "fc_1",
                    "call_id": "call_1",
                    "name": "list_files",
                    "arguments": "",
                }
                done1 = dict(item1, arguments='{"path": "."}', status="completed")
                # call 2: added announces only the item; call_id and name
                # arrive in done, whose EMPTY arguments must not wipe the
                # delta-assembled string
                done2 = {
                    "type": "function_call",
                    "id": "fc_2",
                    "call_id": "call_2",
                    "name": "glob_files",
                    "arguments": "",
                    "status": "completed",
                }
                events = [
                    {"type": "response.created", "response": {"status": "in_progress"}},
                    {
                        "type": "response.output_item.added",
                        "output_index": 0,
                        "item": item1,
                    },
                    {
                        "type": "response.output_item.added",
                        "output_index": 1,
                        "item": {"type": "function_call", "id": "fc_2"},
                    },
                    *junk,
                    {
                        "type": "response.function_call_arguments.delta",
                        "item_id": "fc_1",
                        "output_index": 0,
                        "delta": '{"pa',
                    },
                    {
                        "type": "response.function_call_arguments.delta",
                        "item_id": "fc_2",
                        "output_index": 1,
                        "delta": '{"pattern": ',
                    },
                    {
                        "type": "response.function_call_arguments.delta",
                        "item_id": "fc_1",
                        "output_index": 0,
                        "delta": 'th": "."}',
                    },
                    {
                        "type": "response.function_call_arguments.delta",
                        "item_id": "fc_2",
                        "output_index": 1,
                        "delta": '"*.txt"}',
                    },
                    {
                        "type": "response.function_call_arguments.done",
                        "item_id": "fc_1",
                        "output_index": 0,
                        "arguments": '{"path": "."}',
                    },
                    {
                        "type": "response.output_item.done",
                        "output_index": 0,
                        "item": done1,
                    },
                    {
                        "type": "response.output_item.done",
                        "output_index": 1,
                        "item": done2,
                    },
                    {
                        "type": "response.completed",
                        "response": {
                            "status": "completed",
                            "usage": {"input_tokens": 100, "output_tokens": 10},
                        },
                    },
                ]
        else:
            # both function_calls must have been echoed back, exact
            # arguments included, and both results answered
            calls = {i["call_id"]: i for i in items if i.get("type") == "function_call"}
            if CUSTOM_TOOL:
                need(
                    set(calls) == {"custom_1"},
                    f"custom call not echoed exactly: {sorted(calls)}",
                )
                need(
                    calls["custom_1"].get("name") == CUSTOM_TOOL,
                    f"custom tool name changed: {calls['custom_1']}",
                )
                need(
                    [o.get("call_id") for o in outputs] == ["custom_1"],
                    f"custom output wrong: {outputs}",
                )
            elif SENSITIVE:
                need(
                    set(calls) == {"sensitive_1"},
                    f"sensitive call not echoed exactly: {sorted(calls)}",
                )
                need(
                    [o.get("call_id") for o in outputs] == ["sensitive_1"],
                    f"sensitive output wrong: {outputs}",
                )
            else:
                need(
                    set(calls) == {"call_1", "call_2"},
                    f"function_call items not echoed exactly: {sorted(calls)}",
                )
                expected_call_1 = (
                    '{"path":"."}' if EXPECT_EXTENSION_REWRITE else '{"path": "."}'
                )
                need(
                    calls["call_1"].get("name") == "list_files"
                    and calls["call_1"].get("arguments") == expected_call_1,
                    f"call_1 mangled: {calls['call_1']}",
                )
                need(
                    calls["call_2"].get("name") == "glob_files"
                    and calls["call_2"].get("arguments") == '{"pattern": "*.txt"}',
                    f"call_2 mangled: {calls['call_2']}",
                )
                need(
                    [o.get("call_id") for o in outputs] == ["call_1", "call_2"],
                    f"outputs wrong: {outputs}",
                )
            text = self._answer_text(
                req, outputs[0].get("output", ""), structured is not None
            )
            events = [
                {"type": "response.created", "response": {"status": "in_progress"}},
                {
                    "type": "response.output_item.added",
                    "output_index": 0,
                    "item": {
                        "type": "message",
                        "id": "msg_1",
                        "role": "assistant",
                        "content": [],
                    },
                },
                *junk,
                {
                    "type": "response.reasoning_summary_text.delta",
                    "output_index": 0,
                    "delta": "pondering the listing",
                },
            ]
            for i in range(0, len(text), 7):
                events.append(
                    {
                        "type": "response.output_text.delta",
                        "item_id": "msg_1",
                        "output_index": 0,
                        "delta": text[i : i + 7],
                    }
                )
            events.append(
                {
                    "type": "response.output_text.done",
                    "item_id": "msg_1",
                    "output_index": 0,
                    "text": text,
                }
            )
            if os.environ.get("MOCK_INCOMPLETE"):
                events.append(
                    {
                        "type": "response.incomplete",
                        "response": {
                            "status": "incomplete",
                            "incomplete_details": {"reason": "max_output_tokens"},
                        },
                    }
                )
            else:
                events.append(
                    {
                        "type": "response.completed",
                        "response": {
                            "status": "completed",
                            "usage": {"input_tokens": 200, "output_tokens": 20},
                        },
                    }
                )

        # One byte stream, re-chunked at an arbitrary width so SSE events split
        # mid-line, mid-JSON, and mid-UTF-8 across reads. Most tests keep this
        # HTTP/1.1 connection valid and reusable; otherwise every SDK scenario
        # pays the platform's stale-socket retry deadline. One explicit fixture
        # mode retains the abrupt, unterminated close regression.
        wire = b"".join(sse_typed(e) for e in events)
        # The deterministic reused-read retry requires a clean first response;
        # it takes precedence when a broader CI lane requests truncated
        # terminal fixtures for other scenarios.
        truncated_terminal = TRUNCATED_TERMINAL and not DROP_REUSED_ONCE
        use_content_length = not truncated_terminal and (
            CONNECTION_CLOSE or CHUNK_WIDTH >= len(wire)
        )
        if use_content_length:
            # Content-Length avoids a hosted-macOS/Python interaction that can
            # defer the tiny chunked terminator until the client's stale-read
            # deadline. Large-chunk CI mode does not exercise split boundaries
            # anyway, and the reusable connection remains valid. Default and
            # dedicated transport fixtures retain chunked boundary coverage.
            self._start_stream(len(wire))
            self.wfile.write(wire)
            self.wfile.flush()
            self._finish_close()
        else:
            self._start_stream()
            for i in range(0, len(wire), CHUNK_WIDTH):
                self._chunk(wire[i : i + CHUNK_WIDTH])
            if not truncated_terminal:
                self._chunk(b"")
            self.close_connection = truncated_terminal

    def _answer_text(self, req, tool_output, structured):
        if SENSITIVE:
            return f"PERMISSION-OK {tool_output}"
        nfiles = len([line for line in tool_output.splitlines() if line.strip()])
        if structured:
            return json.dumps({"count": nfiles, "note": "MOCK-OK"})
        tier = req.get("service_tier", "unset")
        text = f"The workspace contains {nfiles} entries. MOCK-OK. tier={tier}"
        if EXPECT_STEER:
            text += " STEER-OK"
        return text


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    # Keep-alive fixtures must not monopolize the accept loop: browser parity
    # opens a second page while Chromium may retain the first page's idle
    # connection. A threaded fixture mirrors a real provider and lets both
    # clients progress independently.
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    if len(sys.argv) > 3:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=sys.argv[2], keyfile=sys.argv[3])
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    # Port 0 lets callers hand allocation to the kernel, avoiding the racy
    # bind-close-spawn sequence which can lose a chosen port to another process.
    print(f"ready on {srv.server_address[1]}", flush=True)
    srv.serve_forever()
