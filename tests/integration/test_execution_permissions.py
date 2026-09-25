#!/usr/bin/env python3
"""Owner permission replies cross a real runner and execution child."""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import time
import unittest
from pathlib import Path

import test_execution_code_mode as code_fixture
from test_image_preview_queue import PNG_A

TNY, WASM = code_fixture.TNY, code_fixture.WASM


@unittest.skipIf(WASM, "requires native runner/execution processes")
class ExecutionPermissions(unittest.TestCase):
    setUp = code_fixture.ExecutionCodeMode.setUp
    close_server = code_fixture.ExecutionCodeMode.close_server

    def launch(self, *, twice=False):
        code = 'print(tools.call("write_file", \'{"path":"effect.txt","content":"first"}\'))'
        self.server.call_name = "run_code"
        self.server.arguments = {"code": code}
        if twice:
            self.server.sequence = [
                {"code": code},
                {"code": code.replace("first", "second")},
            ]
        result = subprocess.run(
            [
                TNY,
                "--cwd",
                str(self.workspace),
                "--provider",
                "openai",
                "--wire-api",
                "responses",
                "--permission-mode",
                "ask",
                "ask",
                "-B",
                "permission fixture",
            ],
            env=dict(self.env, TNY_ISOLATE="1"),
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        sid = result.stdout.strip()
        session = next((self.home / ".tny/sessions").glob(f"*/{sid}/session.json"))
        self.addCleanup(self.stop_session, sid)
        candidates = [
            session.parent / "sock",
            self.home / f"tny-{os.getuid()}" / f"{sid}.sock",
            Path("/tmp") / f"tny-{os.getuid()}" / f"{sid}.sock",
        ]
        self.socket_path = next(path for path in candidates if path.exists())

    def stop_session(self, sid):
        subprocess.run(
            [TNY, "--cwd", str(self.workspace), "session", "stop", sid, "--kill"],
            env=self.env,
            capture_output=True,
            timeout=10,
        )

    def connect(self):
        owner = socket.socket(socket.AF_UNIX)
        owner.settimeout(15)
        try:
            owner.connect(str(self.socket_path))
            owner.sendall(b'{"op":"hello","role":"owner"}\n')
            return owner
        except BaseException:
            owner.close()
            raise

    @staticmethod
    def send(owner, value):
        owner.sendall((json.dumps(value) + "\n").encode())

    def finish(self, owner, decision, *, delay=0, wrong_id=False):
        requests = []
        with owner.makefile("rb") as stream:
            for line in stream:
                event = json.loads(line)
                if event.get("ev") == "permission":
                    requests.append(event)
                    self.assertFalse(
                        (self.workspace / "effect.txt").exists() and len(requests) == 1
                    )
                    if delay:
                        time.sleep(delay)
                        delay = 0
                    if wrong_id:
                        self.send(
                            owner,
                            {
                                "op": "perm",
                                "id": event["id"] + "-wrong",
                                "decision": "allow",
                            },
                        )
                        time.sleep(0.1)
                        self.assertFalse((self.workspace / "effect.txt").exists())
                        wrong_id = False
                    self.send(
                        owner, {"op": "perm", "id": event["id"], "decision": decision}
                    )
                if event.get("ev") == "turn_end":
                    return event, requests
        self.fail("runner disconnected without a terminal event")

    def test_allow_once_and_always_preserve_session_grant_scope(self):
        for decision, count in (("allow", 2), ("allow_always", 1)):
            with self.subTest(decision=decision):
                if (self.workspace / "effect.txt").exists():
                    (self.workspace / "effect.txt").unlink()
                self.server.bodies.clear()
                self.server.outputs.clear()
                self.launch(twice=True)
                with self.connect() as owner:
                    final, requests = self.finish(owner, decision, wrong_id=True)
                self.assertEqual(final["exit_code"], 0, final)
                self.assertEqual(len(requests), count, requests)
                self.assertEqual((self.workspace / "effect.txt").read_text(), "second")

    def test_denial_has_no_effect(self):
        self.launch()
        with self.connect() as owner:
            final, requests = self.finish(owner, "deny")
        self.assertEqual(len(requests), 1)
        self.assertEqual(final["stop"], "denied", final)
        self.assertFalse((self.workspace / "effect.txt").exists())

    def test_human_wait_does_not_consume_five_second_code_budget(self):
        self.launch()
        with self.connect() as owner:
            final, requests = self.finish(owner, "allow", delay=6)
        self.assertEqual(final["exit_code"], 0, final)
        self.assertEqual(len(requests), 1)
        self.assertEqual((self.workspace / "effect.txt").read_text(), "first")

    def test_disconnect_preserves_pending_background_permission_for_reattach(self):
        self.launch()
        with self.connect() as owner, owner.makefile("rb") as stream:
            for line in stream:
                if json.loads(line).get("ev") == "permission":
                    break
            else:
                self.fail("permission was not emitted")
        time.sleep(0.1)
        self.assertFalse((self.workspace / "effect.txt").exists())
        with self.connect() as owner:
            final, requests = self.finish(owner, "deny")
        self.assertEqual(len(requests), 1)
        self.assertEqual(final["stop"], "denied", final)
        self.assertFalse((self.workspace / "effect.txt").exists())

    def test_tool_role_image_controls_cannot_read_files_while_code_is_active(self):
        self.launch()
        with self.connect() as owner, owner.makefile("rb") as stream:
            for line in stream:
                event = json.loads(line)
                if event.get("ev") == "permission":
                    with socket.socket(socket.AF_UNIX) as tool:
                        tool.settimeout(5)
                        tool.connect(str(self.socket_path))
                        self.send(tool, {"op": "hello", "role": "tool"})
                        with tool.makefile("rb") as replies:
                            self.assertEqual(
                                json.loads(replies.readline())["ev"], "hello"
                            )
                            for operation in ("image_attach", "image_preview"):
                                self.send(
                                    tool,
                                    {
                                        "op": operation,
                                        "id": operation,
                                        "path": str(self.workspace / "not-a-file.png"),
                                    },
                                )
                                response = json.loads(replies.readline())
                                self.assertEqual(
                                    response["ev"], "control_result", response
                                )
                                self.assertEqual(response["id"], operation, response)
                                self.assertFalse(response["ok"], response)
                                self.assertIn("use nested", json.dumps(response))
                    self.send(
                        owner, {"op": "perm", "id": event["id"], "decision": "deny"}
                    )
                if event.get("ev") == "turn_end":
                    self.assertEqual(event["stop"], "denied", event)
                    break
            else:
                self.fail("runner disconnected before permission denial settled")
        self.assertFalse((self.workspace / "effect.txt").exists())

    def test_vacant_background_owner_role_cannot_read_images_during_code(self):
        # A detached background turn has no owner. The role in hello is only
        # a self-declared label, so owner admission must not bypass this guard.
        image = self.workspace / "manual.png"
        image.write_bytes(PNG_A)
        self.launch()
        with self.connect() as owner, owner.makefile("rb") as stream:
            for line in stream:
                event = json.loads(line)
                if event.get("ev") == "permission":
                    permission = event
                    break
            else:
                self.fail("background owner did not receive pending permission")
            for operation in ("image_attach", "image_preview"):
                self.send(owner, {"op": operation, "id": operation, "path": str(image)})
                for line in stream:
                    response = json.loads(line)
                    if (
                        response.get("ev") == "control_result"
                        and response.get("id") == operation
                    ):
                        break
                else:
                    self.fail(
                        "owner image control did not receive a correlated refusal"
                    )
                self.assertFalse(response["ok"], response)
                # Owner image controls are rejected by the role allowlist
                # before the active-cell guard; the label grants no image I/O.
                self.assertEqual(
                    response["error"],
                    "operation is not allowed for this client role",
                    response,
                )
            self.send(owner, {"op": "perm", "id": permission["id"], "decision": "deny"})
            for line in stream:
                event = json.loads(line)
                if event.get("ev") == "turn_end":
                    self.assertEqual(event["stop"], "denied", event)
                    break
            else:
                self.fail("runner disconnected before owner denial settled")
        self.assertFalse((self.workspace / "effect.txt").exists())
        self.assertEqual(image.read_bytes(), PNG_A)

    def test_owner_cancel_during_permission_has_no_effect(self):
        self.launch()
        with self.connect() as owner, owner.makefile("rb") as stream:
            for line in stream:
                event = json.loads(line)
                if event.get("ev") == "permission":
                    self.send(owner, {"op": "cancel", "hard": True})
                if event.get("ev") == "turn_end":
                    self.assertEqual(event["stop"], "interrupted", event)
                    break
            else:
                self.fail("runner disconnected without interrupted settlement")
        self.assertFalse((self.workspace / "effect.txt").exists())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
