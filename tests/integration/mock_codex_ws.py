#!/usr/bin/env python3
"""Mock `codex app-server` WebSocket host for tny integration tests.

Stdlib only: the RFC6455 server handshake and frame codec are implemented by
hand (hashlib/base64/socket/struct). Scripted transcript, per
docs/backends/codex-app-server.md:

  connection 1: initialize -> initialized -> thread/start -> turn/start
                -> agentMessage deltas spelling CODEX-MOCK-OK
                -> item/commandExecution/requestApproval (must be answered)
                -> commandExecution item -> token count -> turn/completed
  connection 2: initialize -> thread/resume carrying the same threadId

Assertions (any failure prints MOCK-FAIL and sets exit status 1):
  * no Origin header on the upgrade (a real app-server answers 403)
  * Authorization: Bearer <MOCK_TOKEN> when MOCK_TOKEN is set
  * client frames are masked, text, one JSON object each
  * no "jsonrpc" member on the wire
  * turn/start shape: threadId + input[0] = {type:text, text, text_elements}
  * the approval request comes back as a result with a decision
"""
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def knob_delay(name):
    """MOCK_INIT_DELAY_MS / MOCK_THREAD_DELAY_MS: latency injection for the
    tny latency benchmarks. Default 0 keeps the test suite timing-neutral."""
    ms = int(os.environ.get(name, "0"))
    if ms:
        time.sleep(ms / 1000.0)
MARKER = "CODEX-MOCK-OK"
THREAD_ID = "thr_mock_0001"
APPROVAL_ID = 9001

# MOCK_EXPECT_EFFORT: when set, at least one turn/start must carry exactly
# this `effort`; a different value on any turn is a failure. When unset, no
# turn may carry one (tny sends it only when --effort is set).
EXPECT_EFFORT = os.environ.get("MOCK_EXPECT_EFFORT")
# MOCK_STEER_WAIT_MS: after the first deltas, hold the turn open this long
# and accept turn/steer (docs/adr/0011). The request must carry the active
# turn id as expectedTurnId and UserInput[] — a stale id is rejected the way
# the app-server rejects it, and the steered text is echoed as STEER-OK.
STEER_WAIT_MS = int(os.environ.get("MOCK_STEER_WAIT_MS", "0"))
# MOCK_STEER_REJECT (docs/adr/0012): "now" answers every valid turn/steer
# with a JSON-RPC error (a non-steerable turn); "late" swallows the request,
# completes the turn, and only then sends the error — the response-after-
# turn-completed ordering a real socket allows. Only the FIRST turn waits
# for steers, so the re-queued text can run as a normal second turn.
STEER_REJECT = os.environ.get("MOCK_STEER_REJECT", "")
# MOCK_EXPECT_RESEND: this exact text must arrive later as a turn/start
# prompt — proof that a rejected steer was re-queued, not lost.
EXPECT_RESEND = os.environ.get("MOCK_EXPECT_RESEND")
STEER_SEEN = []
EFFORT_SEEN = []
PROMPTS = []
TURN_NO = [0]

FAILURES = []


def note(msg):
    print("MOCK-NOTE %s" % msg, file=sys.stderr, flush=True)


def fail(msg):
    FAILURES.append(msg)
    print("MOCK-FAIL %s" % msg, file=sys.stderr, flush=True)


# ---------------------------------------------------------------- handshake

def read_head(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            return None, b""
        data += chunk
        if len(data) > 65536:
            return None, b""
    head, rest = data.split(b"\r\n\r\n", 1)
    lines = head.decode("latin-1").split("\r\n")
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    return headers, rest


def refuse(conn, status):
    conn.sendall(("HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                  % status).encode())


def handshake(conn):
    headers, rest = read_head(conn)
    if headers is None:
        fail("malformed or truncated HTTP upgrade")
        return None
    if "origin" in headers:
        refuse(conn, "403 Forbidden")
        fail("client sent an Origin header (codex answers 403)")
        return None
    token = os.environ.get("MOCK_TOKEN")
    if token:
        if headers.get("authorization") != "Bearer %s" % token:
            refuse(conn, "401 Unauthorized")
            fail("missing or wrong Authorization bearer on the upgrade")
            return None
        note("bearer accepted")
    key = headers.get("sec-websocket-key")
    if not key or headers.get("upgrade", "").lower() != "websocket":
        refuse(conn, "400 Bad Request")
        fail("upgrade request missing Sec-WebSocket-Key/Upgrade")
        return None
    accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    conn.sendall(("HTTP/1.1 101 Switching Protocols\r\n"
                  "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
    return rest


# ------------------------------------------------------------- frame codec

class WS:
    def __init__(self, conn, pre=b""):
        self.conn = conn
        self.buf = pre
        self.closed = False

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.conn.recv(8192)
            if not chunk:
                raise EOFError
            self.buf += chunk

    def recv_frame(self):
        self._need(2)
        b0, b1 = self.buf[0], self.buf[1]
        op = b0 & 0x0F
        masked = bool(b1 & 0x80)
        ln = b1 & 0x7F
        off = 2
        if ln == 126:
            self._need(4)
            ln = struct.unpack(">H", self.buf[2:4])[0]
            off = 4
        elif ln == 127:
            self._need(10)
            ln = struct.unpack(">Q", self.buf[2:10])[0]
            off = 10
        mask = b"\0\0\0\0"
        if masked:
            self._need(off + 4)
            mask = self.buf[off:off + 4]
            off += 4
        else:
            fail("client frame was not masked (RFC6455 requires it)")
        if ln > 16 * 1024 * 1024:
            raise EOFError
        self._need(off + ln)
        payload = bytearray(self.buf[off:off + ln])
        self.buf = self.buf[off + ln:]
        for i in range(len(payload)):
            payload[i] ^= mask[i % 4]
        return op, bytes(payload)

    def _send(self, op, data=b""):
        n = len(data)
        if n < 126:
            hdr = struct.pack(">BB", 0x80 | op, n)
        elif n < 65536:
            hdr = struct.pack(">BBH", 0x80 | op, 126, n)
        else:
            hdr = struct.pack(">BBQ", 0x80 | op, 127, n)
        self.conn.sendall(hdr + data)

    def send_json(self, obj):
        self._send(0x1, json.dumps(obj).encode())

    def recv_json(self):
        """Next client JSON object, or None on close/EOF."""
        while True:
            try:
                op, payload = self.recv_frame()
            except (EOFError, ConnectionError, socket.timeout):
                return None
            if op == 0x8:  # close
                self.closed = True
                try:
                    self._send(0x8, struct.pack(">H", 1000))
                except OSError:
                    pass
                return None
            if op == 0x9:  # ping
                self._send(0xA, payload)
                continue
            if op == 0xA:
                continue
            if op != 0x1:
                fail("client sent a non-text frame (opcode %d)" % op)
                continue
            try:
                msg = json.loads(payload.decode("utf-8"))
            except (ValueError, UnicodeDecodeError):
                fail("client sent a frame that is not valid JSON")
                continue
            if not isinstance(msg, dict):
                fail("client frame was not a JSON object")
                continue
            if "jsonrpc" in msg:
                fail("client sent a jsonrpc member (app-server omits it)")
            return msg


# --------------------------------------------------------------- transcript

def run_turn(ws, req_id, msg):
    params = msg.get("params") or {}
    if params.get("threadId") != THREAD_ID:
        fail("turn/start threadId=%r, expected %r" % (params.get("threadId"), THREAD_ID))
    inp = params.get("input")
    if not isinstance(inp, list) or not inp or not isinstance(inp[0], dict):
        fail("turn/start input is not a non-empty array of objects: %r" % (inp,))
        inp = [{}]
    if inp[0].get("type") != "text":
        fail("turn/start input[0].type=%r, expected 'text'" % inp[0].get("type"))
    if not isinstance(inp[0].get("text"), str) or not inp[0].get("text"):
        fail("turn/start input[0].text missing")
    if inp[0].get("text_elements") != []:
        fail("turn/start input[0].text_elements missing or not an empty array")
    PROMPTS.append(inp[0].get("text"))
    TURN_NO[0] += 1
    turn_no = TURN_NO[0]
    effort = params.get("effort")
    if effort is not None:
        if effort == EXPECT_EFFORT:
            EFFORT_SEEN.append(effort)
            note("turn/start effort=%s ok" % effort)
        else:
            fail("turn/start effort=%r, want %r" % (effort, EXPECT_EFFORT))
    note("turn/start ok (prompt=%r)" % inp[0].get("text"))

    turn_id = "turn_mock_%d" % turn_no
    ws.send_json({"id": req_id, "result": {"turn": {"id": turn_id}}})
    ws.send_json({"method": "turn/started", "params": {"turn": {"id": turn_id}}})

    item = "item_msg_%d" % turn_no
    ws.send_json({"method": "item/started",
                  "params": {"item": {"id": item, "type": "agentMessage"}}})
    for i in range(0, len(MARKER), 4):
        ws.send_json({"method": "item/reasoning/delta",
                      "params": {"itemId": "item_think_1", "delta": "thinking…"}})
        ws.send_json({"method": "item/agentMessage/delta",
                      "params": {"itemId": item, "delta": MARKER[i:i + 4]}})
    ws.send_json({"method": "item/completed",
                  "params": {"item": {"id": item, "type": "agentMessage",
                                      "text": MARKER}}})

    late_steer_ids = []
    if STEER_WAIT_MS and turn_no == 1:
        deadline = time.time() + STEER_WAIT_MS / 1000.0
        ws.conn.settimeout(0.2)
        while time.time() < deadline:
            try:
                m = ws.recv_json()
            except Exception:
                m = None
            if m is None:
                continue
            if m.get("method") != "turn/steer":
                note("ignored %s while waiting for turn/steer" % m.get("method"))
                continue
            p = m.get("params") or {}
            sid = m.get("id")
            if p.get("threadId") != THREAD_ID:
                fail("turn/steer threadId=%r" % p.get("threadId"))
            if p.get("expectedTurnId") != turn_id:
                ws.send_json({"id": sid, "error": {"code": -32600,
                              "message": "expectedTurnId does not match the active turn"}})
                note("turn/steer rejected (stale turn id %r)" % p.get("expectedTurnId"))
                continue
            si = p.get("input")
            if (not isinstance(si, list) or not si or si[0].get("type") != "text"
                    or not si[0].get("text") or si[0].get("text_elements") != []):
                fail("turn/steer input is not UserInput[] text: %r" % (si,))
                continue
            STEER_SEEN.append(si[0]["text"])
            if STEER_REJECT == "now":
                # a non-steerable turn (/review, manual /compact) rejects the
                # request but the turn itself keeps running (docs/adr/0012)
                ws.send_json({"id": sid, "error": {"code": -32600,
                              "message": "turn is not steerable"}})
                note("turn/steer rejected now (text=%r)" % si[0]["text"])
                continue
            if STEER_REJECT == "late":
                # answer only after turn/completed: response ordering on the
                # socket is independent of the turn notifications
                late_steer_ids.append(sid)
                note("turn/steer held for a late rejection (text=%r)" % si[0]["text"])
                continue
            note("turn/steer ok (text=%r)" % si[0]["text"])
            ws.send_json({"id": sid, "result": {"turnId": turn_id}})
            # the host replays steered input as a userMessage item (tny hides it)
            ws.send_json({"method": "item/completed",
                          "params": {"item": {"id": "item_user_2", "type": "userMessage",
                                              "text": si[0]["text"]}}})
            ws.send_json({"method": "item/started",
                          "params": {"item": {"id": "item_msg_2", "type": "agentMessage"}}})
            ws.send_json({"method": "item/agentMessage/delta",
                          "params": {"itemId": "item_msg_2",
                                     "delta": "STEER-OK:" + si[0]["text"]}})
            ws.send_json({"method": "item/completed",
                          "params": {"item": {"id": "item_msg_2", "type": "agentMessage",
                                              "text": "STEER-OK:" + si[0]["text"]}}})
        ws.conn.settimeout(30)

    ws.send_json({"method": "item/commandExecution/requestApproval",
                  "id": APPROVAL_ID,
                  "params": {"itemId": "item_cmd_1", "command": ["ls", "-la"],
                             "cwd": "/tmp", "reason": "command needs approval"}})
    decision = None
    while decision is None:
        reply = ws.recv_json()
        if reply is None:
            fail("connection closed before the approval was answered")
            return
        if reply.get("id") != APPROVAL_ID:
            continue
        if "error" in reply:
            fail("approval answered with an error: %r" % reply.get("error"))
            return
        result = reply.get("result")
        if not isinstance(result, dict):
            fail("approval result was not an object: %r" % (result,))
            return
        decision = result.get("decision")
    if decision not in ("accept", "acceptForSession", "decline"):
        fail("approval decision=%r is not an app-server decision" % (decision,))
    else:
        note("approval answered decision=%s" % decision)

    cmd_item = {"id": "item_cmd_%d" % turn_no, "type": "commandExecution",
                "command": ["ls", "-la"], "cwd": "/tmp"}
    ws.send_json({"method": "item/started", "params": {"item": dict(cmd_item)}})
    done = dict(cmd_item)
    done.update({"status": "completed", "exitCode": 0})
    ws.send_json({"method": "item/completed", "params": {"item": done}})
    if STEER_REJECT:
        # a per-turn marker so the test can tell turn 2 answered the re-send
        ws.send_json({"method": "item/completed",
                      "params": {"item": {"id": "item_done_%d" % turn_no,
                                          "type": "agentMessage",
                                          "text": "TURN%d-DONE" % turn_no}}})
    ws.send_json({"method": "turn/tokenCount",
                  "params": {"usage": {"input_tokens": 123, "output_tokens": 45}}})
    ws.send_json({"method": "turn/completed",
                  "params": {"turn": {"id": turn_id, "status": "completed"}}})
    for sid in late_steer_ids:
        ws.send_json({"id": sid, "error": {"code": -32600,
                      "message": "expectedTurnId does not match the active turn"}})
        note("turn/steer rejected late (after turn/completed)")


def serve(conn, index):
    conn.settimeout(30)
    rest = handshake(conn)
    if rest is None:
        return
    ws = WS(conn, rest)
    initialized = False
    # MOCK_BUSY_CONN=<n>: on that connection, answer the first thread/start and
    # the first turn/start with -32001 so the client's backoff+retry is exercised.
    busy = index == int(os.environ.get("MOCK_BUSY_CONN", "0"))
    busied = set()
    while True:
        msg = ws.recv_json()
        if msg is None:
            return
        method = msg.get("method")
        req_id = msg.get("id")
        if method is None:
            continue  # a response to one of our requests, handled inline
        if method != "initialize" and not initialized and req_id is not None:
            ws.send_json({"id": req_id, "error": {"code": -32002,
                                                  "message": "Not initialized"}})
            fail("%s arrived before initialize" % method)
            continue
        if busy and method in ("thread/start", "turn/start") and method not in busied:
            busied.add(method)
            note("answering %s with -32001 (overload drill)" % method)
            ws.send_json({"id": req_id, "error": {"code": -32001,
                                                  "message": "Server overloaded; retry later."}})
            continue
        if method == "initialize":
            knob_delay("MOCK_INIT_DELAY_MS")
            if initialized:
                fail("initialize sent twice on one connection")
            info = (msg.get("params") or {}).get("clientInfo") or {}
            if info.get("name") != "tny":
                fail("clientInfo.name=%r, expected 'tny'" % info.get("name"))
            initialized = True
            note("initialize ok (client=%r v%r)" % (info.get("name"), info.get("version")))
            ws.send_json({"id": req_id, "result": {"userAgent": "mock-codex/0.0.1",
                                                   "platformFamily": "unix",
                                                   "platformOs": "mock"}})
        elif method == "initialized":
            note("initialized notification received")
        elif method == "thread/start":
            knob_delay("MOCK_THREAD_DELAY_MS")
            if index == 2:
                fail("connection 2 used thread/start instead of thread/resume")
            # MOCK_FAST_CONN=<n>: that connection ran with --fast and must
            # carry serviceTier "priority" (the pre-rename spelling tny pins
            # on the wire); every other connection must omit the field.
            tier = (msg.get("params") or {}).get("serviceTier")
            if index == int(os.environ.get("MOCK_FAST_CONN", "0")):
                if tier != "priority":
                    fail("thread/start serviceTier=%r, expected 'priority' (--fast)"
                         % (tier,))
                else:
                    note("thread/start serviceTier=priority ok")
            elif tier is not None:
                fail("thread/start carried serviceTier=%r without --fast" % (tier,))
            note("thread/start ok")
            ws.send_json({"id": req_id, "result": {"thread": {"id": THREAD_ID}}})
        elif method == "thread/resume":
            knob_delay("MOCK_THREAD_DELAY_MS")
            got = (msg.get("params") or {}).get("threadId")
            if got != THREAD_ID:
                fail("thread/resume threadId=%r, expected %r" % (got, THREAD_ID))
            else:
                note("thread/resume ok (threadId=%s)" % got)
            ws.send_json({"id": req_id, "result": {"thread": {"id": THREAD_ID}}})
        elif method == "turn/start":
            run_turn(ws, req_id, msg)
        elif method == "model/list":
            # catalog with per-model reasoning efforts; "hidden" must be
            # skipped by tny's normalization (docs/backends/codex-app-server.md)
            note("model/list served")
            ws.send_json({"id": req_id, "result": {"data": [
                {"id": "mock-codex-model", "displayName": "Mock Codex",
                 "supportedReasoningEfforts": [
                     {"reasoningEffort": "low"},
                     {"reasoningEffort": "medium"},
                     {"reasoningEffort": "high"},
                     {"reasoningEffort": "xhigh"}],
                 "defaultReasoningEffort": "medium"},
                {"id": "mock-hidden-model", "hidden": True}]}})
        elif method == "turn/interrupt":
            ws.send_json({"id": req_id, "result": {}})
        elif req_id is not None:
            ws.send_json({"id": req_id, "error": {"code": -32601,
                                                  "message": "unknown method"}})


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    want = int(os.environ.get("MOCK_CONNECTIONS", "2"))
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(4)
    srv.settimeout(60)
    print("ready on %d" % srv.getsockname()[1], flush=True)
    for index in range(1, want + 1):
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            fail("timed out waiting for connection %d" % index)
            break
        try:
            serve(conn, index)
        except (ConnectionError, socket.timeout, OSError) as exc:
            fail("connection %d aborted: %s" % (index, exc))
        finally:
            conn.close()
    if EXPECT_EFFORT and not EFFORT_SEEN:
        fail("expected turn/start effort=%r but no turn carried one" % EXPECT_EFFORT)
    if EXPECT_RESEND and EXPECT_RESEND not in PROMPTS[1:]:
        fail("rejected steer text %r never came back as a turn/start prompt "
             "(prompts seen: %r)" % (EXPECT_RESEND, PROMPTS))
    print("MOCK-DONE failures=%d" % len(FAILURES), flush=True)
    sys.exit(1 if FAILURES else 0)


if __name__ == "__main__":
    main()
