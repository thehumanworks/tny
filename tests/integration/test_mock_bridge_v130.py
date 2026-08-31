#!/usr/bin/env python3
"""Standalone exhaustive contract test for the sdk.v1.0.30 mock bridge."""

import base64
import http.client
import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/integration"))
from mock_bridge_v130 import ROUTES  # noqa: E402


class Callback(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    seen = []

    def log_message(self, *args):
        pass

    def do_POST(self):
        assert self.path == "/sdk.v1.SdkCustomToolCallbackService/CallCustomTool"
        assert self.headers["Authorization"] == "Bearer callback-secret"
        assert self.headers["Connect-Protocol-Version"] == "1"
        raw = self.rfile.read(int(self.headers["Content-Length"]))
        req = json.loads(raw)
        assert req["toolName"] == "host_echo" and req["agentId"]
        self.seen.append(req)
        body = json.dumps({"result": {"echo": req["args"]["text"]}}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class MockBridgeContractTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="tny-mock-sdk-v1-")
        self.base = Path(self.temp.name)
        self.workspace = self.base / "ws"
        self.state = self.base / "state"
        self.workspace.mkdir()
        self.state.mkdir()
        (self.workspace / "README.md").write_text("hello\n")
        Callback.seen = []
        self.callback = ThreadingHTTPServer(("127.0.0.1", 0), Callback)
        threading.Thread(target=self.callback.serve_forever, daemon=True).start()
        self.api_key = "key-full-contract-test"
        env = os.environ | {
            "TNY_MOCK_DIR": str(self.state),
            "TNY_MOCK_CWD": str(self.workspace),
            "CURSOR_API_KEY": self.api_key,
            "TNY_MOCK_INVOKE_CUSTOM_TOOL": "host_echo",
            "CURSOR_SDK_STORE_CALLBACK_AUTH_TOKEN": "store-secret",
        }
        self.proc = subprocess.Popen(
            [
                sys.executable,
                "-u",
                str(ROOT / "tests/integration/mock_bridge.py"),
                "--store-callback-url",
                "http://127.0.0.1:9",
            ],
            cwd=self.workspace,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        ready_line = self.proc.stderr.readline()
        self.assertTrue(ready_line.startswith("cursor-sdk-bridge ready "))
        ready = json.loads(ready_line.split(" ready ", 1)[1])
        self.target = ready["host"], ready["port"]
        token = Path(ready["authTokenFile"]).read_text().strip()
        self.headers = {
            "Authorization": "Bearer " + token,
            "Connect-Protocol-Version": "1",
        }

    def tearDown(self):
        self.callback.shutdown()
        self.callback.server_close()
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait(timeout=5)
        self.proc.stdout.close()
        self.proc.stderr.close()
        self.temp.cleanup()

    def rpc(self, service, method, req, *, stream=False, expected=200):
        conn = http.client.HTTPConnection(*self.target, timeout=5)
        raw = json.dumps(req, separators=(",", ":")).encode()
        if stream:
            raw = b"\0" + len(raw).to_bytes(4, "big") + raw
        headers = self.headers | {
            "Content-Type": (
                "application/connect+json" if stream else "application/json"
            )
        }
        conn.request("POST", f"/sdk.v1.{service}/{method}", raw, headers)
        response = conn.getresponse()
        body = response.read()
        conn.close()
        self.assertEqual(expected, response.status, (method, response.status, body))
        if not stream:
            return json.loads(body or b"{}")
        frames = []
        offset = 0
        end_count = 0
        while offset + 5 <= len(body):
            flags = body[offset]
            size = int.from_bytes(body[offset + 1 : offset + 5], "big")
            self.assertLessEqual(offset + 5 + size, len(body))
            payload = body[offset + 5 : offset + 5 + size]
            offset += 5 + size
            self.assertIn(flags, (0, 2))
            if flags == 2:
                end_count += 1
                self.assertIsInstance(json.loads(payload), dict)
            elif payload:
                self.assertEqual(0, end_count, "data arrived after EndStream")
                frames.append(json.loads(payload))
        self.assertEqual(len(body), offset, "truncated Connect envelope")
        self.assertEqual(1, end_count, "stream must contain exactly one EndStream")
        return frames

    def test_all_27_routes_state_streams_callbacks_and_errors(self):
        key = {"apiKey": self.api_key}
        self.assertEqual(
            "pong", self.rpc("SdkBridgeControlService", "Ping", {})["message"]
        )
        version = self.rpc("SdkBridgeControlService", "GetVersion", {})
        self.assertEqual("sdk.v1", version["protocolVersion"])
        self.rpc(
            "SdkBridgeControlService",
            "SetToolCallback",
            {
                "url": f"http://127.0.0.1:{self.callback.server_port}",
                "authToken": "callback-secret",
            },
        )
        for method in ("Me", "ListModels", "ListRepositories"):
            self.rpc("SdkCursorService", method, {"options": key})

        local_options = {
            "model": {"id": "mock-cursor-model"},
            "apiKey": self.api_key,
            "name": "local",
            "local": {
                "cwd": [str(self.workspace)],
                "dirs": [str(self.workspace)],
                "store": {"type": "custom"},
                "customTools": {
                    "host_echo": {
                        "description": "echo",
                        "inputSchema": {"type": "object"},
                    }
                },
            },
        }
        local = self.rpc(
            "SdkAgentService",
            "CreateAgent",
            {"options": local_options, "idempotencyKey": "local-1"},
        )["agentId"]
        cloud_options = {
            "model": {"id": "mock-cursor-model"},
            "apiKey": self.api_key,
            "name": "cloud",
            "cloud": {
                "env": {"type": "CLOUD_ENVIRONMENT_TYPE_CLOUD", "name": "mock"},
                "repos": [{"url": "https://github.com/example/mock-one"}],
                "metadata": {"kind": "test"},
            },
        }
        cloud = self.rpc(
            "SdkAgentService",
            "CreateAgent",
            {"options": cloud_options, "idempotencyKey": "cloud-1"},
        )["agentId"]
        self.rpc(
            "SdkAgentService",
            "ResumeAgent",
            {"agentId": local, "options": local_options},
        )
        self.rpc("SdkAgentService", "ReloadAgent", {"agentId": local})
        self.rpc("SdkAgentService", "CloseAgent", {"agentId": local})
        self.rpc("SdkAgentService", "ReloadAgent", {"agentId": local})

        sent = self.rpc(
            "SdkAgentService",
            "Send",
            {
                "agentId": local,
                "message": {"text": "hello"},
                "options": {"enableDeltas": True, "enableSteps": True},
                "idempotencyKey": "send-1",
            },
            stream=True,
        )
        run_id = next(item["result"]["runId"] for item in sent if "result" in item)
        self.assertTrue(Callback.seen)
        tool_result = json.loads((self.state / "custom_tool_result.json").read_text())
        self.assertEqual("host_echo", tool_result["toolName"])

        operation = {"apiKey": self.api_key, "cwd": str(self.workspace)}
        self.rpc("SdkAgentService", "WaitLiveRun", {"runId": run_id})
        self.rpc("SdkAgentService", "GetRun", {"runId": run_id, "options": operation})
        self.rpc(
            "SdkAgentService",
            "ListRuns",
            {"agentId": local, "options": operation | {"limit": 1}},
        )
        conversation = self.rpc(
            "SdkAgentService", "GetRunConversation", {"runId": run_id}
        )
        self.assertEqual(run_id, json.loads(conversation["conversationJson"])["runId"])
        observed = self.rpc(
            "SdkAgentService",
            "ObserveRun",
            {"runId": run_id, "afterOffset": "offset:1"},
            stream=True,
        )
        self.assertTrue(any("offset" in item for item in observed))
        self.rpc(
            "SdkAgentService",
            "CancelRun",
            {"runId": run_id, "agentId": local},
        )
        self.rpc(
            "SdkAgentService", "GetAgent", {"agentId": local, "options": operation}
        )
        self.rpc("SdkAgentService", "ListAgents", {"options": operation | {"limit": 1}})
        self.rpc(
            "SdkAgentService",
            "ArchiveAgent",
            {"agentId": local, "options": operation},
        )
        self.rpc(
            "SdkAgentService",
            "ListAgents",
            {"options": operation | {"includeArchived": True}},
        )
        self.rpc(
            "SdkAgentService",
            "UnarchiveAgent",
            {"agentId": local, "options": operation},
        )
        messages = self.rpc(
            "SdkAgentService",
            "ListAgentMessages",
            {"agentId": local, "options": operation | {"limit": 1, "offset": 1}},
        )
        self.assertEqual(1, len(messages["messages"]))

        cloud_sent = self.rpc(
            "SdkAgentService",
            "Send",
            {
                "agentId": cloud,
                "message": {"text": "cloud"},
                "options": {"enableDeltas": True, "cloud": {"envVars": {"A": "B"}}},
            },
            stream=True,
        )
        cloud_run = next(
            item["result"]["runId"] for item in cloud_sent if "result" in item
        )
        artifacts = self.rpc("SdkAgentService", "ListArtifacts", {"agentId": cloud})[
            "artifacts"
        ]
        chunks = self.rpc(
            "SdkAgentService",
            "DownloadArtifact",
            {"agentId": cloud, "path": artifacts[0]["path"]},
            stream=True,
        )
        artifact = b"".join(base64.b64decode(item["data"]) for item in chunks)
        self.assertIn(b"artifact", artifact)
        usage = self.rpc(
            "SdkAgentService", "GetUsage", {"agentId": cloud, "runId": cloud_run}
        )
        self.assertTrue(usage["usage"]["runs"])
        error = self.rpc(
            "SdkAgentService",
            "GetAgent",
            {"agentId": "missing", "options": operation},
            expected=404,
        )
        self.assertTrue(error["details"][0]["type"].endswith("SdkErrorDetails"))
        self.rpc(
            "SdkAgentService",
            "DeleteAgent",
            {"agentId": cloud, "options": key},
        )
        self.rpc("SdkBridgeControlService", "Shutdown", {"graceSeconds": 0})
        self.proc.wait(timeout=3)
        remaining_stderr = self.proc.stderr.read()
        self.assertNotIn("store-secret", remaining_stderr)

        expected = {
            f"/sdk.v1.{service}/{method}"
            for service, methods in ROUTES.items()
            for method in methods
        }
        seen = set((self.state / "routes.log").read_text().splitlines())
        self.assertEqual(set(), expected - seen)
        persisted = (self.state / "state.json").read_text()
        self.assertNotIn("callback-secret", persisted)
        self.assertNotIn("store-secret", persisted)
        self.assertNotIn(self.api_key, persisted)
        launch = json.loads((self.state / "launch.json").read_text())
        self.assertTrue(launch["storeCallback"]["configured"])
        self.assertTrue(launch["storeCallback"]["authTokenPresent"])
        self.assertFalse(launch["storeCallback"]["authTokenArgPresent"])
        self.assertFalse(launch["storeCallback"]["tokenInArgv"])
        failures = self.state / "failures.log"
        self.assertFalse(failures.exists() and failures.read_text())

    def test_wrong_connect_protocol_is_a_structured_400(self):
        headers = dict(self.headers)
        headers["Connect-Protocol-Version"] = "2"
        conn = http.client.HTTPConnection(*self.target, timeout=5)
        conn.request(
            "POST",
            "/sdk.v1.SdkBridgeControlService/Ping",
            b"{}",
            headers | {"Content-Type": "application/json"},
        )
        response = conn.getresponse()
        body = json.loads(response.read())
        conn.close()
        self.assertEqual(400, response.status)
        self.assertEqual("invalid_argument", body["code"])
        self.assertTrue(body["details"][0]["type"].endswith("SdkErrorDetails"))


if __name__ == "__main__":
    # tests/integration/run.sh supplies the built tny path to every Python
    # integration test. This fixture exercises the bridge contract directly.
    sys.argv[1:] = []
    unittest.main()
