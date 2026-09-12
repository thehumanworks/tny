#!/usr/bin/env python3
"""Captured pending-image queue and the explicit preview control op (ADR 0096).

Every assertion is made on what the loopback provider actually received, with
base64/hashlib oracles that never call into tny. Two groups:

* ``SharedQueueTests`` uses only shared C (typed tools), so it runs natively and
  under the wasm build: a file rewritten inside the same tool batch cannot
  change the pixels that batch already captured.
* ``ControlPreviewTests`` drives the real detached runner's control socket with
  a throwaway client written here — the shape ``tny image generate --preview``
  will use later. wasm has no session socket, so that group is explicitly
  pending there rather than silently skipped.

No preview CLI flag exists yet; this slice covers the queue, the receiver and
the control primitive only.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TNY = str(Path(os.environ.get("TNY", ROOT / "build/tny")).resolve())
WASM = "wasm" in TNY
REFUSAL = "image input is disabled for this provider by settings.json image_input"


def png(tag: bytes, width: int = 1) -> bytes:
    """A complete, distinct PNG. The tag makes the bytes unmistakable."""

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    rows = b"".join(b"\0" + b"\x20\x60\xa0" * width for _ in range(1))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">II5B", width, 1, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows, 1))
        + chunk(b"tEXt", b"tny\0" + tag)
        + chunk(b"IEND", b"")
    )


PNG_A = png(b"first-generation")
PNG_B = png(b"second-generation", width=2)


def sha(data: bytes) -> str:
    """Independent hash oracle; it never calls into tny."""
    return hashlib.sha256(data).hexdigest()


# The control client a generated-image preview will use. Written into the
# throwaway HOME so the fixture ships no extra tracked file.
CLIENT = r"""
import json, os, socket, sys

plan = json.load(open(sys.argv[1]))
sock = os.environ.get("TNY_SESSION_SOCK", "")
out = {"sock": sock, "replies": []}
role = plan.get("role", "tool")
for step in plan["steps"]:
    if step["kind"] == "write":
        open(step["path"], "wb").write(bytes.fromhex(step["hex"]))
        continue
    request = {"op": step["op"], "id": step["id"]}
    if "path" in step:
        request["path"] = step["path"]
    if "expected" in step:
        request["expected_sha256"] = step["expected"]
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(20)
    s.connect(sock)
    wire = json.dumps({"op": "hello", "role": role}) + "\n"
    wire += json.dumps(request) + "\n"
    s.sendall(wire.encode())
    buf = b""
    reply = None
    while reply is None:
        got = s.recv(65536)
        if not got:
            break
        buf += got
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if not line.strip():
                continue
            try:
                message = json.loads(line)
            except ValueError:
                continue
            if message.get("id") == step["id"] or message.get("ev") == "error":
                reply = message
                break
    s.close()
    out["replies"].append(reply)
if plan.get("record"):
    open(plan["record"], "w").write(json.dumps(out))
print("PVCLIENT " + json.dumps(out))
"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, status, ct, body):
        self.send_response(status)
        self.send_header("Content-Type", ct)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        state = self.server.state
        raw = self.rfile.read(int(self.headers["Content-Length"]))
        body = json.loads(raw)
        state["chat"].append(body)
        index = len(state["chat"]) - 1
        hold = state.get("hold_request")
        if hold is not None and index == hold:
            state["holding"].set()
            state["release"].wait(20)
        calls = state["tool_calls"] if index == 0 else None
        if calls:
            delta = {
                "tool_calls": [
                    {
                        "index": i,
                        "id": f"preview-call-{i}",
                        "type": "function",
                        "function": {
                            "name": call["name"],
                            "arguments": json.dumps(call["arguments"]),
                        },
                    }
                    for i, call in enumerate(calls)
                ]
            }
            reason = "tool_calls"
        else:
            delta, reason = {"content": "MOCK-OK"}, "stop"
        frames = [
            {"choices": [{"index": 0, "delta": delta, "finish_reason": None}]},
            {"choices": [{"index": 0, "delta": {}, "finish_reason": reason}]},
        ]
        self.reply(
            200,
            "text/event-stream",
            (
                "".join(f"data: {json.dumps(f)}\n\n" for f in frames)
                + "data: [DONE]\n\n"
            ).encode(),
        )


class PreviewBase(unittest.TestCase):
    isolate = "0"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="tny-preview-queue-")
        self.home = Path(self.tmp.name)
        self.ws = self.home / "ws"
        self.ws.mkdir()
        self.shot = self.ws / "generated.png"
        self.shot.write_bytes(PNG_A)
        self.outside = self.home / "elsewhere.png"
        self.outside.write_bytes(PNG_A)
        self.client = self.home / "pv_client.py"
        self.client.write_text(CLIENT)
        self.state = {
            "chat": [],
            "tool_calls": None,
            "hold_request": None,
            "holding": threading.Event(),
            "release": threading.Event(),
        }
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.state = self.state
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = f"http://127.0.0.1:{self.server.server_port}"
        self.env = {
            "HOME": str(self.home),
            "PATH": os.environ["PATH"],
            "LANG": "C.UTF-8",
            "OPENAI_BASE_URL": self.url + "/v1",
            "OPENAI_API_KEY": "fixture-openai-key",
            "OPENAI_WIRE_API": "chat",
        }
        if self.isolate is not None:
            self.env["TNY_ISOLATE"] = self.isolate

    def tearDown(self):
        self.state["release"].set()
        self.server.shutdown()
        self.server.server_close()
        self.tmp.cleanup()

    # ---- helpers ----

    def settings(self, obj):
        (self.home / ".tny").mkdir(exist_ok=True)
        (self.home / ".tny/settings.json").write_text(json.dumps(obj))

    def run_tny(self, *args, env=None, timeout=90):
        return subprocess.run(
            [TNY, "--cwd", str(self.ws), *args],
            cwd=self.ws,
            env=dict(self.env, **(env or {})),
            capture_output=True,
            text=True,
            timeout=timeout,
        )

    def image_parts(self, body):
        parts = []
        for message in body["messages"]:
            content = message.get("content")
            if isinstance(content, list):
                parts += [p for p in content if p.get("type") == "image_url"]
        return parts

    def image_bytes(self, body):
        """Exact pixels this request carried, in order, decoded here."""
        out = []
        for part in self.image_parts(body):
            url = part["image_url"]["url"]
            head, _, payload = url.partition(",")
            self.assertTrue(head.endswith(";base64"), head)
            out.append((head, base64.b64decode(payload)))
        return out

    def attachment_texts(self, body):
        texts = []
        for message in body["messages"]:
            content = message.get("content")
            if isinstance(content, list) and any(
                p.get("type") == "image_url" for p in content
            ):
                texts += [p["text"] for p in content if p.get("type") == "text"]
        return texts

    def tool_results(self, body):
        return [m["content"] for m in body["messages"] if m.get("role") == "tool"]


class SharedQueueTests(PreviewBase):
    """Shared C only: runs natively and under the wasm build."""

    def test_ssh_capture_does_not_reread_the_staged_file(self):
        if WASM:
            self.skipTest(
                "SSH is a native process transport; wasm refusal is checked separately"
            )
        self.settings({"image_input": {"openai": True}})
        bin_dir = self.home / "bin"
        bin_dir.mkdir()
        fake = bin_dir / "ssh"
        marker = self.home / "stage-mutated"
        fake.write_text(f"""#!{os.path.realpath(sys.executable)}
import pathlib, subprocess, sys
args = sys.argv[1:]
if args[-1] == "true" or "exit" in args:
    sys.exit(0)
if "preview-stage-mutation" in args[-1]:
    files = list(pathlib.Path({str(self.home)!r}).glob("**/ssh-images/*"))
    for file in files:
        file.write_bytes(bytes.fromhex({PNG_B.hex()!r}))
    pathlib.Path({str(marker)!r}).write_text(str(len(files)))
sys.exit(subprocess.call(["sh", "-c", args[-1]]))
""")
        fake.chmod(0o755)
        self.env["PATH"] = str(bin_dir) + os.pathsep + self.env["PATH"]
        self.state["tool_calls"] = [
            {"name": "read_image", "arguments": {"path": str(self.shot)}},
            {
                "name": "terminal",
                "arguments": {"command": "printf preview-stage-mutation"},
            },
        ]
        r = self.run_tny(
            "--ssh", "fixture.invalid", "--ssh-cwd", str(self.ws), "ask", "read image"
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(marker.read_text(), "1")
        self.assertEqual(
            [b for _, b in self.image_bytes(self.state["chat"][1])], [PNG_A]
        )

    def test_captured_bytes_survive_a_rewrite_in_the_same_batch(self):
        # read_image captures the pixels, then the SAME path is rewritten with
        # something that is not an image at all — inside one batch, before the
        # flush. Re-reading the path at flush would either send the wrong
        # bytes or fail to send any.
        self.settings({"image_input": {"openai": True}})
        self.state["tool_calls"] = [
            {"name": "read_image", "arguments": {"path": str(self.shot)}},
            {
                "name": "write_file",
                "arguments": {"path": str(self.shot), "content": "not an image"},
            },
        ]
        r = self.run_tny("ask", "--no-save", "look at the screenshot")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(self.state["chat"]), 2, r.stderr)
        self.assertEqual(self.image_bytes(self.state["chat"][0]), [])
        carried = self.image_bytes(self.state["chat"][1])
        self.assertEqual(len(carried), 1, carried)
        self.assertEqual(carried[0][0], "data:image/png;base64")
        self.assertEqual(sha(carried[0][1]), sha(PNG_A))
        self.assertEqual(carried[0][1], PNG_A)
        # the file on disk really was replaced
        self.assertEqual(self.shot.read_bytes(), b"not an image")
        # manual-only batches keep their established wording
        self.assertEqual(
            self.attachment_texts(self.state["chat"][1]),
            ["Image attached by read_image."],
        )

    def test_configured_false_still_refuses_the_queue(self):
        self.settings({"image_input": {"openai": False}})
        self.state["tool_calls"] = [
            {"name": "read_image", "arguments": {"path": str(self.shot)}}
        ]
        r = self.run_tny("ask", "--no-save", "look at the screenshot")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(self.state["chat"]), 2)
        self.assertEqual(self.image_bytes(self.state["chat"][1]), [])
        results = self.tool_results(self.state["chat"][1])
        self.assertTrue(results[0].startswith("error:"), results)


class ControlPreviewTests(PreviewBase):
    """The real detached runner plus a tool-role control client.

    The detached runner needs a saved session, so these runs deliberately omit
    --no-save: an ephemeral turn has no socket at all (ADR 0053).
    """

    isolate = None  # the runner, and therefore TNY_SESSION_SOCK, is required

    def setUp(self):
        if WASM:
            # Not "not applicable": the browser/node build has no fork and no
            # AF_UNIX runner, so this group has no VM to run in yet.
            self.skipTest(
                "pending on wasm: the session control socket is unsupported there"
            )
        super().setUp()

    def plan(self, steps, *, role="tool", record=None):
        path = self.home / "plan.json"
        path.write_text(json.dumps({"steps": steps, "role": role, "record": record}))
        return path

    def terminal_call(self, plan_path):
        return [
            {
                "name": "terminal",
                "arguments": {
                    "command": f"{sys.executable} {self.client} {plan_path}",
                    "timeout_s": 60,
                },
            }
        ]

    def replies(self, body_index=1):
        """The control replies the tool child printed, from the transcript."""
        results = self.tool_results(self.state["chat"][body_index])
        self.assertTrue(results, self.state["chat"][body_index]["messages"])
        marker = "PVCLIENT "
        for result in results:
            if marker in result:
                line = result[result.index(marker) + len(marker) :]
                line = line.splitlines()[0]
                return json.loads(line)
        self.fail(f"no control client output in {results}")

    def test_preview_queued_bytes_reach_the_next_request_once(self):
        self.settings({"image_input": {"openai": True}})
        # two generations write the SAME output path in one batch
        plan = self.plan(
            [
                {
                    "kind": "op",
                    "op": "image_preview",
                    "id": "pv-a",
                    "path": str(self.shot),
                    "expected": sha(PNG_A),
                },
                {"kind": "write", "path": str(self.shot), "hex": PNG_B.hex()},
                {
                    "kind": "op",
                    "op": "image_preview",
                    "id": "pv-b",
                    "path": str(self.shot),
                    "expected": sha(PNG_B),
                },
            ]
        )
        self.state["tool_calls"] = self.terminal_call(plan)
        r = self.run_tny("ask", "generate and review")
        self.assertEqual(r.returncode, 0, r.stderr)
        out = self.replies()
        self.assertEqual([x["status"] for x in out["replies"]], ["queued", "queued"])
        self.assertTrue(all(x["ok"] for x in out["replies"]), out)
        self.assertNotIn("error_code", out["replies"][0])

        self.assertEqual(len(self.state["chat"]), 2, r.stderr)
        self.assertEqual(self.image_bytes(self.state["chat"][0]), [])
        carried = self.image_bytes(self.state["chat"][1])
        self.assertEqual([sha(b) for _, b in carried], [sha(PNG_A), sha(PNG_B)])
        self.assertEqual([b for _, b in carried], [PNG_A, PNG_B])
        self.assertEqual(
            self.attachment_texts(self.state["chat"][1]),
            ["Images queued by explicitly requested generation/edit preview."],
        )

    def test_hard_cancel_reports_non_delivery_and_resume_has_no_old_bytes(self):
        self.cancelled_preview(parked=False)

    def test_hard_cancel_while_permission_parked_drains_non_delivery(self):
        self.cancelled_preview(parked=True)

    def cancelled_preview(self, *, parked):
        record = self.home / "queued.json"
        release = self.home / "release-tool"
        plan = self.plan(
            [
                {
                    "kind": "op",
                    "op": "image_preview",
                    "id": "before-cancel",
                    "path": str(self.shot),
                    "expected": sha(PNG_A),
                }
            ],
            record=str(record),
        )
        calls = self.terminal_call(plan)
        if parked:
            calls[0]["arguments"]["command"] += (
                f"; while [ ! -e {release} ]; do sleep 0.02; done"
            )
            calls.append(
                {"name": "terminal", "arguments": {"command": "printf harmless"}}
            )
        else:
            calls[0]["arguments"]["command"] += "; sleep 30"
        self.settings(
            {
                "image_input": {"openai": True},
                "permission": {"*": "allow", "bash": {"printf harmless": "ask"}},
            }
        )
        self.state["tool_calls"] = calls
        mode = "ask" if parked else "yolo"
        # This checks cancellation ownership, not the optional OS sandbox.
        (self.ws / ".tny.json").write_text(json.dumps({"sandbox": "off"}))
        r = self.run_tny("--permission-mode", mode, "ask", "-B", "queue and wait")
        self.assertEqual(r.returncode, 0, r.stderr)
        sid = r.stdout.strip()
        deadline = time.monotonic() + 10
        while not record.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(record.exists(), "preview did not reach the receiver")
        receipt = json.loads(record.read_text())
        self.assertEqual(receipt["replies"][0]["status"], "queued")
        events = []
        with socket.socket(socket.AF_UNIX) as owner:
            owner.settimeout(10)
            owner.connect(receipt["sock"])
            owner.sendall(b'{"op":"hello","role":"owner"}\n')
            if not parked:
                owner.sendall(b'{"op":"cancel","hard":true}\n')
            with owner.makefile("rb") as stream:
                for line in stream:
                    event = json.loads(line)
                    events.append(event)
                    if parked and event.get("ev") == "hello":
                        release.touch()  # let the first tool finish, then park the second
                    if parked and event.get("ev") == "permission":
                        owner.sendall(b'{"op":"cancel","hard":true}\n')
                    if event.get("ev") == "turn_end":
                        break
        self.assertEqual(events[-1].get("stop"), "interrupted", events)
        self.assertTrue(
            any(
                "IMAGE_PREVIEW_NOT_DELIVERED" in e.get("text", "") for e in events[:-1]
            ),
            events,
        )
        self.assertEqual(len(self.state["chat"]), 1)
        # Resume the same durable session, after the old runner releases its lock.
        deadline = time.monotonic() + 10
        while Path(receipt["sock"]).exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.state["tool_calls"] = None
        r = self.run_tny("ask", "--resume", sid, "clean second turn")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(self.state["chat"]), 2)
        self.assertEqual(self.image_bytes(self.state["chat"][1]), [])
        self.assertNotIn(
            base64.b64encode(PNG_A).decode(), json.dumps(self.state["chat"][1])
        )

    def test_complete_control_strings_are_validated_before_admission(self):
        self.settings({"image_input": {"openai": True}})
        base = {
            "kind": "op",
            "op": "image_preview",
            "id": "valid",
            "path": str(self.shot),
            "expected": sha(PNG_A),
        }
        steps = []
        for field in ("op", "id", "path", "expected"):
            bad = dict(base, id=f"bad-{field}")
            bad[field] += "\0suffix"
            steps.append(bad)
        for field, value in (
            ("expected", "a" * 65),
            ("expected", "a" * 63),
            ("id", "i" * 1024),
            ("path", "p" * 4096),
        ):
            bad = dict(base, id=f"length-{field}")
            bad[field] = value
            steps.append(bad)
        # The legacy manual operation also rejects malformed complete fields;
        # valid manual requests keep their unchanged reply shape.
        for field in ("op", "id", "path"):
            bad = dict(base, op="image_attach", id=f"manual-{field}")
            bad[field] += "\0suffix"
            steps.append(bad)
        for field in ("id", "path", "expected"):
            for value in (None, 12, [], {}):
                bad = dict(base, id=f"type-{field}")
                bad[field] = value
                steps.append(bad)
        steps.append(base)
        self.state["tool_calls"] = self.terminal_call(self.plan(steps))
        r = self.run_tny("ask", "validate defensive inputs")
        self.assertEqual(r.returncode, 0, r.stderr)
        replies = self.replies()["replies"]
        for reply in replies[:-1]:
            self.assertIsNotNone(reply)
            self.assertFalse(reply.get("ok", False), reply)
            self.assertNotEqual(reply.get("status"), "queued", reply)
        self.assertEqual(replies[-1]["status"], "queued")
        self.assertEqual(
            [b for _, b in self.image_bytes(self.state["chat"][1])], [PNG_A]
        )

    def test_role_with_decoded_nul_is_not_tool_role(self):
        self.settings({"image_input": {"openai": True}})
        step = {
            "kind": "op",
            "op": "image_preview",
            "id": "role",
            "path": str(self.shot),
            "expected": sha(PNG_A),
        }
        self.state["tool_calls"] = self.terminal_call(
            self.plan([step], role="tool\0suffix")
        )
        r = self.run_tny("ask", "validate role")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.replies()["replies"], [None])
        self.assertEqual(self.image_bytes(self.state["chat"][1]), [])

    def test_mixed_manual_and_preview_batch_sends_both_once(self):
        self.settings({"image_input": {"openai": True}})
        manual = self.ws / "manual.png"
        manual.write_bytes(PNG_B)
        plan = self.plan(
            [
                {
                    "kind": "op",
                    "op": "image_attach",
                    "id": "pv-manual",
                    "path": str(manual),
                },
                {
                    "kind": "op",
                    "op": "image_preview",
                    "id": "pv-preview",
                    "path": str(self.shot),
                    "expected": sha(PNG_A),
                },
            ]
        )
        self.state["tool_calls"] = self.terminal_call(plan)
        r = self.run_tny("ask", "attach and review")
        self.assertEqual(r.returncode, 0, r.stderr)
        out = self.replies()
        self.assertTrue(out["replies"][0]["ok"], out)
        self.assertNotIn("status", out["replies"][0])  # manual reply unchanged
        self.assertEqual(out["replies"][1]["status"], "queued")
        carried = self.image_bytes(self.state["chat"][1])
        self.assertEqual([b for _, b in carried], [PNG_B, PNG_A])  # queue order
        self.assertEqual(
            self.attachment_texts(self.state["chat"][1]),
            ["Images attached by explicit tool requests."],
        )

    def test_unknown_policy_allows_manual_but_refuses_preview(self):
        self.settings({})  # unknown: no image_input entry at all
        plan = self.plan(
            [
                {
                    "kind": "op",
                    "op": "image_preview",
                    "id": "pv-unknown",
                    "path": str(self.shot),
                    "expected": sha(PNG_A),
                },
                {
                    "kind": "op",
                    "op": "image_attach",
                    "id": "pv-manual",
                    "path": str(self.shot),
                },
            ]
        )
        self.state["tool_calls"] = self.terminal_call(plan)
        r = self.run_tny("ask", "review it")
        self.assertEqual(r.returncode, 0, r.stderr)
        out = self.replies()
        self.assertEqual(out["replies"][0]["status"], "unsupported")
        self.assertEqual(out["replies"][0]["error_code"], "image_input_not_configured")
        self.assertFalse(out["replies"][0]["ok"])
        self.assertTrue(out["replies"][1]["ok"], out)  # manual still works
        carried = self.image_bytes(self.state["chat"][1])
        self.assertEqual([b for _, b in carried], [PNG_A])
        self.assertEqual(
            self.attachment_texts(self.state["chat"][1]),
            ["Image attached by read_image."],
        )
        # the generated artifact is untouched by the refusal
        self.assertEqual(self.shot.read_bytes(), PNG_A)

    def test_configured_false_refuses_preview_and_manual_attach(self):
        self.settings({"image_input": {"openai": False}})
        plan = self.plan(
            [
                {
                    "kind": "op",
                    "op": "image_preview",
                    "id": "pv-false",
                    "path": str(self.shot),
                    "expected": sha(PNG_A),
                },
                {
                    "kind": "op",
                    "op": "image_attach",
                    "id": "pv-manual",
                    "path": str(self.shot),
                },
            ]
        )
        self.state["tool_calls"] = self.terminal_call(plan)
        r = self.run_tny("ask", "review it")
        self.assertEqual(r.returncode, 0, r.stderr)
        out = self.replies()
        self.assertEqual(out["replies"][0]["status"], "unsupported")
        self.assertEqual(out["replies"][0]["error_code"], "image_input_not_configured")
        self.assertIn(REFUSAL, out["replies"][0]["error"])
        self.assertFalse(out["replies"][1]["ok"])
        self.assertIn(REFUSAL, out["replies"][1]["error"])
        self.assertEqual(self.image_bytes(self.state["chat"][1]), [])

    def test_hash_and_root_refusals_never_queue(self):
        self.settings({"image_input": {"openai": True}})
        plan = self.plan(
            [
                {
                    "kind": "op",
                    "op": "image_preview",
                    "id": "pv-hash",
                    "path": str(self.shot),
                    "expected": sha(PNG_B),  # the other generation's hash
                },
                {
                    "kind": "op",
                    "op": "image_preview",
                    "id": "pv-root",
                    "path": str(self.outside),
                    "expected": sha(PNG_A),
                },
                {
                    "kind": "op",
                    "op": "image_preview",
                    "id": "pv-shape",
                    "path": str(self.shot),
                    "expected": "NOTHEX",
                },
            ]
        )
        self.state["tool_calls"] = self.terminal_call(plan)
        r = self.run_tny("ask", "review it")
        self.assertEqual(r.returncode, 0, r.stderr)
        out = self.replies()
        self.assertEqual(out["replies"][0]["status"], "failed")
        self.assertEqual(out["replies"][0]["error_code"], "hash_mismatch")
        self.assertEqual(out["replies"][1]["status"], "failed")
        self.assertEqual(out["replies"][1]["error_code"], "outside_allowed_roots")
        self.assertFalse(out["replies"][2]["ok"], out["replies"][2])
        self.assertNotIn("status", out["replies"][2])  # validation, not admission
        self.assertEqual(self.image_bytes(self.state["chat"][1]), [])

    def test_tool_role_cannot_reach_owner_control(self):
        self.settings({"image_input": {"openai": True}})
        plan = self.plan(
            [{"kind": "op", "op": "cancel", "id": "pv-cancel"}],
        )
        self.state["tool_calls"] = self.terminal_call(plan)
        r = self.run_tny("ask", "review it")
        self.assertEqual(r.returncode, 0, r.stderr)
        out = self.replies()
        self.assertIn("not allowed for this client role", out["replies"][0]["error"])

    def test_streaming_without_a_tool_batch_is_not_queued(self):
        # The first batch only reports the runner's socket path. While the NEXT
        # response is still streaming — turn active, no tool batch — a preview
        # cannot join a request, and must not claim it did.
        self.settings({"image_input": {"openai": True}})
        record = self.home / "sock.json"
        plan = self.plan([], record=str(record))
        self.state["tool_calls"] = self.terminal_call(plan)
        self.state["hold_request"] = 1
        proc = subprocess.Popen(
            [TNY, "--cwd", str(self.ws), "ask", "probe"],
            cwd=self.ws,
            env=self.env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            self.assertTrue(
                self.state["holding"].wait(60), "the second request never arrived"
            )
            deadline = time.time() + 30
            while time.time() < deadline and not record.exists():
                time.sleep(0.05)
            self.assertTrue(record.exists(), "the tool child recorded no socket")
            sock = json.loads(record.read_text())["sock"]
            self.assertTrue(sock, "the terminal child had no TNY_SESSION_SOCK")
            reply = self.control(sock, "image_preview", str(self.shot), sha(PNG_A))
            self.assertEqual(reply["status"], "turn_not_ready")
            self.assertEqual(reply["error_code"], "no_continuable_tool_batch")
            self.assertFalse(reply["ok"])
        finally:
            self.state["release"].set()
            out, err = proc.communicate(timeout=90)
        self.assertEqual(proc.returncode, 0, err)
        for body in self.state["chat"]:
            self.assertEqual(self.image_bytes(body), [])

    def control(self, sock_path, op, path, expected=None):
        request = {"op": op, "id": "direct-1", "path": path}
        if expected is not None:
            request["expected_sha256"] = expected
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(20)
        s.connect(sock_path)
        s.sendall(
            (
                json.dumps({"op": "hello", "role": "tool"})
                + "\n"
                + json.dumps(request)
                + "\n"
            ).encode()
        )
        buf = b""
        while True:
            got = s.recv(65536)
            if not got:
                s.close()
                self.fail("the runner closed the control connection")
            buf += got
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    message = json.loads(line)
                except ValueError:
                    continue
                if message.get("id") == "direct-1":
                    s.close()
                    return message


class ControlPrimitiveTests(unittest.TestCase):
    def test_wasm_helper_branch_returns_data_without_stdio(self):
        # Compile the actual no-socket branch on the native ABI. This checks
        # preview specifically, but is NOT a wasm-runtime parity claim.
        if WASM:
            self.skipTest(
                "native compiler seam; wasm runtime refusal is checked separately"
            )
        with tempfile.TemporaryDirectory(prefix="tny-wasm-control-") as td:
            source = Path(td) / "check.c"
            binary = Path(td) / "check"
            source.write_text(r"""
#include "cli/cmd_control.h"
#include <stdlib.h>
int main(void) {
    tny_control_reply reply = {0};
    unsetenv("TNY_SESSION_SOCK");
    if (tny_control_request(TNY_CONTROL_OP_IMAGE_PREVIEW, "unused.png", "unused", &reply)
        != TNY_CONTROL_EXCHANGE_NO_SOCKET) return 1;
    setenv("TNY_SESSION_SOCK", "/unused.sock", 1);
    if (tny_control_request(TNY_CONTROL_OP_IMAGE_PREVIEW, "unused.png", "unused", &reply)
        != TNY_CONTROL_EXCHANGE_UNSUPPORTED) return 2;
    if (reply.ok || reply.id || reply.answer || reply.error || reply.status || reply.error_code)
        return 3;
    tny_control_reply_free(&reply);
    return 0;
}
""")
            link_gc = (
                "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
            )
            command = [
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-D_DEFAULT_SOURCE",
                "-D_DARWIN_C_SOURCE",
                "-D__EMSCRIPTEN__",
                "-ffunction-sections",
                "-fdata-sections",
                "-Iinclude",
                "-Isrc",
                "-Ithird_party/yyjson",
                str(source),
                "src/cli/cmd_control.c",
                link_gc,
                "-o",
                str(binary),
            ]
            build = subprocess.run(
                command, cwd=ROOT, capture_output=True, text=True, timeout=30
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run(
                [str(binary)], capture_output=True, text=True, timeout=10
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "")
            self.assertEqual(result.stderr, "")

    def test_reply_correlation_requires_the_complete_json_id(self):
        if WASM:
            self.skipTest("AF_UNIX is unavailable in wasm")
        with tempfile.TemporaryDirectory(prefix="tny-control-") as td:
            path = str(Path(td) / "sock")
            errors = []
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(path)
                server.listen(1)
                server.settimeout(5)

                def respond():
                    try:
                        conn, _ = server.accept()
                        with conn, conn.makefile("rb") as stream:
                            json.loads(stream.readline())  # hello
                            request = json.loads(stream.readline())
                            rid = request["id"]
                            replies = [
                                {"id": rid + "\0suffix", "ok": True},
                                {
                                    "id": rid,
                                    "ok": False,
                                    "error": "exact-correlation-sentinel",
                                },
                            ]
                            for reply in replies:
                                conn.sendall((json.dumps(reply) + "\n").encode())
                    except Exception as error:
                        errors.append(str(error))

                thread = threading.Thread(target=respond)
                thread.start()
                env = dict(os.environ, TNY_SESSION_SOCK=path)
                result = subprocess.run(
                    [TNY, "image", "attach", "unused.png"],
                    env=env,
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                thread.join(6)
                self.assertFalse(thread.is_alive())
                self.assertEqual(errors, [])
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("exact-correlation-sentinel", result.stderr)
                self.assertEqual(result.stdout, "")

    @unittest.skipUnless(WASM, "requires the wasm binary")
    def test_wasm_control_cleanly_refuses_without_stdout(self):
        env = dict(os.environ, TNY_SESSION_SOCK="/unused-control.sock")
        result = subprocess.run(
            [TNY, "image", "attach", "unused.png"],
            env=env,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertIn("session control is unavailable in WebAssembly", result.stderr)


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        TNY = str(Path(sys.argv.pop(1)).resolve())
        WASM = "wasm" in TNY
    unittest.main(verbosity=2)
