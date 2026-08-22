#!/usr/bin/env python3
"""Mock OpenAI-compatible provider for tny integration tests.

Serves BOTH wires (docs/adr/0014):
  POST /v1/responses          — the default Responses API wire (typed SSE)
  POST /v1/chat/completions   — the legacy chat wire (wire_api "chat")
  GET  /v1/models

Both wires run the same scenario and validate the request strictly:
Turn 1 streams a list_files tool call with the JSON arguments fragmented
across SSE events; turn 2 streams a text answer mentioning what the tool
returned. SSE rides chunked transfer-encoding and the responses stream is
deliberately re-chunked at arbitrary byte boundaries so event reassembly
is exercised end to end (real transports split anywhere).

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
  MOCK_INCOMPLETE     responses wire: the final answer ends in
                      response.incomplete (token cutoff) — the partial
                      text must still finish the turn cleanly

The responses wire streams TWO parallel tool calls (list_files +
glob_files). The second one's output_item.added carries only the item id;
call_id, name arrive in output_item.done, whose empty `arguments` must
never wipe the delta-assembled string. Junk events ride along (items
without a type, deltas without/with empty payloads, an unknown
output_index) — tny must skip them without crashing. Every responses
stream ends with an abrupt connection close after the terminal event (no
final chunk): completeness comes from response.completed/failed, not the
transport.

Usage: mock_openai.py [port] [certfile keyfile]
With certfile/keyfile the mock serves HTTPS (used by test_https.py).
"""
import json
import os
import ssl
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

EXPECT_WIRE = os.environ.get("MOCK_EXPECT_WIRE")
EXPECT_EFFORT = os.environ.get("MOCK_EXPECT_EFFORT")
SLOW_MS = int(os.environ.get("MOCK_SLOW_MS", "0"))
EXPECT_STEER = os.environ.get("MOCK_EXPECT_STEER")
FAIL_RESPONSE = os.environ.get("MOCK_FAIL_RESPONSE")


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


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _chunk(self, data: bytes):
        self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _reject(self, msg):
        self._json(400, {"error": {"message": msg}})

    def _start_stream(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

    def do_GET(self):
        if self.path.endswith("/models"):
            self._json(200, {"data": [{"id": "mock-model-1"},
                                      {"id": "mock-model-2"}]})
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(n))
        try:
            if self.path.endswith("/chat/completions"):
                need(EXPECT_WIRE != "responses",
                     "hit /chat/completions but the responses wire was expected")
                self._post_chat(req)
            elif self.path.endswith("/responses"):
                need(EXPECT_WIRE != "chat",
                     "hit /responses but the chat wire was expected")
                self._post_responses(req)
            else:
                self._reject(f"unknown endpoint {self.path}")
        except BadRequest as e:
            self._reject(str(e))

    # ---- legacy chat wire ----

    def _post_chat(self, req):
        need(req.get("reasoning_effort") == EXPECT_EFFORT,
             f"reasoning_effort is {req.get('reasoning_effort')!r}, "
             f"want {EXPECT_EFFORT!r}")
        need("input" not in req, "chat request must not carry input items")
        msgs = req.get("messages")
        need(isinstance(msgs, list) and msgs, "messages missing")
        need(msgs[0].get("role") == "system", "no system preamble")
        for t in req.get("tools", []):
            need("function" in t, "chat tools must nest under function")
        has_tool_result = any(m.get("role") == "tool" for m in msgs)
        if not has_tool_result and SLOW_MS:
            time.sleep(SLOW_MS / 1000.0)
        if has_tool_result and EXPECT_STEER:
            last = msgs[-1]
            need(last.get("role") == "user" and last.get("content") == EXPECT_STEER,
                 f"steer: last message is {last!r}, want user {EXPECT_STEER!r}")

        structured = req.get("response_format")
        if structured is not None:
            need(structured.get("type") == "json_schema", f"bad {structured}")
            need("schema" in structured["json_schema"], f"bad {structured}")
            need("name" in structured["json_schema"], f"bad {structured}")

        self._start_stream()
        if not has_tool_result:
            frames = [
                {"choices": [{"index": 0, "delta": {"role": "assistant",
                    "tool_calls": [{"index": 0, "id": "call_1", "type": "function",
                                    "function": {"name": "list_files", "arguments": ""}}]}}]},
                {"choices": [{"index": 0, "delta": {
                    "tool_calls": [{"index": 0, "function": {"arguments": "{\"pa"}}]}}]},
                {"choices": [{"index": 0, "delta": {
                    "tool_calls": [{"index": 0, "function": {"arguments": "th\": \".\"}"}}]}}]},
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}],
                 "usage": {"prompt_tokens": 100, "completion_tokens": 10}},
            ]
        else:
            tool_msg = next(m for m in msgs if m.get("role") == "tool")
            text = self._answer_text(req, tool_msg["content"],
                                     structured is not None)
            frames = []
            for i in range(0, len(text), 7):
                frames.append({"choices": [{"index": 0, "delta": {"content": text[i:i+7]}}]})
            frames.append({"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                           "usage": {"prompt_tokens": 200, "completion_tokens": 20}})
        for f in frames:
            self._chunk(sse(f))
        self._chunk(b"data: [DONE]\n\n")
        self._chunk(b"")  # final chunk

    # ---- Responses API wire ----

    def _post_responses(self, req):
        need("messages" not in req, "responses request must not carry messages")
        need(req.get("store") is False, "responses request must send store:false")
        need(req.get("stream") is True, "responses request must stream")
        need("reasoning_effort" not in req,
             "reasoning_effort is a chat member; responses use reasoning.effort")
        effort = (req.get("reasoning") or {}).get("effort")
        need(effort == EXPECT_EFFORT,
             f"reasoning.effort is {effort!r}, want {EXPECT_EFFORT!r}")
        instructions = req.get("instructions")
        need(isinstance(instructions, str) and "tny" in instructions,
             "instructions must carry the system preamble")
        items = req.get("input")
        need(isinstance(items, list) and items, "input items missing")
        for t in req.get("tools", []):
            need(t.get("type") == "function" and "name" in t and "function" not in t,
                 f"responses tools must be flat: {t}")

        structured = (req.get("text") or {}).get("format")
        if structured is not None:
            need(structured.get("type") == "json_schema", f"bad {structured}")
            need("json_schema" not in structured, f"not flattened: {structured}")
            need("schema" in structured and "name" in structured,
                 f"bad {structured}")

        outputs = [i for i in items if i.get("type") == "function_call_output"]
        if not outputs and SLOW_MS:
            time.sleep(SLOW_MS / 1000.0)
        if outputs and EXPECT_STEER:
            last = items[-1]
            need(last.get("role") == "user" and last.get("content") == EXPECT_STEER,
                 f"steer: last item is {last!r}, want user {EXPECT_STEER!r}")

        # noise every stream carries: an item with no type, deltas with a
        # missing / empty payload, and a delta for an output_index that was
        # never announced — all must be skipped without crashing
        junk = [
            {"type": "response.output_item.added", "output_index": 7,
             "item": {"id": "mystery"}},
            {"type": "response.output_text.delta", "output_index": 7},
            {"type": "response.output_text.delta", "output_index": 7, "delta": ""},
            {"type": "response.reasoning_summary_text.delta", "output_index": 7},
            {"type": "response.function_call_arguments.delta",
             "item_id": "fc_ghost", "output_index": 9, "delta": "{\"x\":1}"},
            {"type": "response.function_call_arguments.delta",
             "item_id": "fc_ghost", "output_index": 9},
        ]

        self._start_stream()
        if not outputs:
            if FAIL_RESPONSE:
                events = [
                    {"type": "response.created", "response": {"status": "in_progress"}},
                    {"type": "response.failed",
                     "response": {"status": "failed",
                                  "error": {"code": "server_error",
                                            "message": FAIL_RESPONSE}}},
                ]
            else:
                # call 1: full metadata up front, arguments fragmented in
                # deltas, output_item.done repeats the complete string
                item1 = {"type": "function_call", "id": "fc_1",
                         "call_id": "call_1", "name": "list_files", "arguments": ""}
                done1 = dict(item1, arguments="{\"path\": \".\"}", status="completed")
                # call 2: added announces only the item; call_id and name
                # arrive in done, whose EMPTY arguments must not wipe the
                # delta-assembled string
                done2 = {"type": "function_call", "id": "fc_2",
                         "call_id": "call_2", "name": "glob_files",
                         "arguments": "", "status": "completed"}
                events = [
                    {"type": "response.created", "response": {"status": "in_progress"}},
                    {"type": "response.output_item.added", "output_index": 0,
                     "item": item1},
                    {"type": "response.output_item.added", "output_index": 1,
                     "item": {"type": "function_call", "id": "fc_2"}},
                    *junk,
                    {"type": "response.function_call_arguments.delta",
                     "item_id": "fc_1", "output_index": 0, "delta": "{\"pa"},
                    {"type": "response.function_call_arguments.delta",
                     "item_id": "fc_2", "output_index": 1,
                     "delta": "{\"pattern\": "},
                    {"type": "response.function_call_arguments.delta",
                     "item_id": "fc_1", "output_index": 0, "delta": "th\": \".\"}"},
                    {"type": "response.function_call_arguments.delta",
                     "item_id": "fc_2", "output_index": 1, "delta": "\"*.txt\"}"},
                    {"type": "response.function_call_arguments.done",
                     "item_id": "fc_1", "output_index": 0,
                     "arguments": "{\"path\": \".\"}"},
                    {"type": "response.output_item.done", "output_index": 0,
                     "item": done1},
                    {"type": "response.output_item.done", "output_index": 1,
                     "item": done2},
                    {"type": "response.completed",
                     "response": {"status": "completed",
                                  "usage": {"input_tokens": 100, "output_tokens": 10}}},
                ]
        else:
            # both function_calls must have been echoed back, exact
            # arguments included, and both results answered
            calls = {i["call_id"]: i for i in items
                     if i.get("type") == "function_call"}
            need(set(calls) == {"call_1", "call_2"},
                 f"function_call items not echoed exactly: {sorted(calls)}")
            need(calls["call_1"].get("name") == "list_files" and
                 calls["call_1"].get("arguments") == "{\"path\": \".\"}",
                 f"call_1 mangled: {calls['call_1']}")
            need(calls["call_2"].get("name") == "glob_files" and
                 calls["call_2"].get("arguments") == "{\"pattern\": \"*.txt\"}",
                 f"call_2 mangled: {calls['call_2']}")
            need([o.get("call_id") for o in outputs] == ["call_1", "call_2"],
                 f"outputs wrong: {outputs}")
            text = self._answer_text(req, outputs[0].get("output", ""),
                                     structured is not None)
            events = [{"type": "response.created",
                       "response": {"status": "in_progress"}},
                      {"type": "response.output_item.added", "output_index": 0,
                       "item": {"type": "message", "id": "msg_1",
                                "role": "assistant", "content": []}},
                      *junk,
                      {"type": "response.reasoning_summary_text.delta",
                       "output_index": 0, "delta": "pondering the listing"}]
            for i in range(0, len(text), 7):
                events.append({"type": "response.output_text.delta",
                               "item_id": "msg_1", "output_index": 0,
                               "delta": text[i:i+7]})
            events.append({"type": "response.output_text.done",
                           "item_id": "msg_1", "output_index": 0, "text": text})
            if os.environ.get("MOCK_INCOMPLETE"):
                events.append({"type": "response.incomplete",
                               "response": {"status": "incomplete",
                                            "incomplete_details":
                                                {"reason": "max_output_tokens"}}})
            else:
                events.append({"type": "response.completed",
                               "response": {"status": "completed",
                                            "usage": {"input_tokens": 200,
                                                      "output_tokens": 20}}})

        # one byte stream, re-chunked at an arbitrary width so SSE events
        # split mid-line, mid-JSON, and mid-UTF-8 across reads. After the
        # terminal event the connection closes abruptly — NO final chunk:
        # completeness is response.completed/failed, not a clean transport
        # close, and tny must reopen the connection for the next POST.
        wire = b"".join(sse_typed(e) for e in events)
        for i in range(0, len(wire), 17):
            self._chunk(wire[i:i+17])
        self.close_connection = True

    def _answer_text(self, req, tool_output, structured):
        nfiles = len([l for l in tool_output.splitlines() if l.strip()])
        if structured:
            return json.dumps({"count": nfiles, "note": "MOCK-OK"})
        tier = req.get("service_tier", "unset")
        text = f"The workspace contains {nfiles} entries. MOCK-OK. tier={tier}"
        if EXPECT_STEER:
            text += " STEER-OK"
        return text


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    srv = HTTPServer(("127.0.0.1", port), Handler)
    if len(sys.argv) > 3:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=sys.argv[2], keyfile=sys.argv[3])
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print(f"ready on {port}", flush=True)
    srv.serve_forever()
