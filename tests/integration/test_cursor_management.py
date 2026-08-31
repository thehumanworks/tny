#!/usr/bin/env python3
"""End-to-end `tny cursor` aliases and safe raw sdk.v1 RPCs."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TNY = Path(
    os.environ.get("TNY") or (sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/tny")
)
MOCK = ROOT / "tests/integration/mock_bridge.py"
BRIDGE_READY_TIMEOUT_MS = 60_000 if sys.platform == "darwin" else None
SUBPROCESS_TIMEOUT = (
    BRIDGE_READY_TIMEOUT_MS // 1000 + 10 if BRIDGE_READY_TIMEOUT_MS else 20
)


def main():
    temp = Path(tempfile.mkdtemp(prefix="tny-cursor-management-"))
    try:
        home, workspace, state = temp / "home", temp / "ws", temp / "bridge"
        home.mkdir()
        workspace.mkdir()
        state.mkdir()
        settings_dir = home / ".tny"
        settings_dir.mkdir()
        (settings_dir / "settings.json").write_text(
            json.dumps(
                {
                    "cursor": {
                        "runtime": "local",
                        "agent_options": {
                            "mode": "AGENT_MODE_OPTION_PLAN",
                            "tools": {"names": []},
                            "disallowedTools": ["shell"],
                            "local": {"store": {"type": "custom"}},
                        },
                        "send_options": {
                            "enableSteps": True,
                            "mode": "AGENT_MODE_OPTION_PLAN",
                        },
                    }
                }
            )
        )
        (workspace / "README.md").write_text("hello\n")
        wrapper = temp / "mock-bridge"
        wrapper.write_text(f'#!/bin/sh\nexec "{sys.executable}" -u "{MOCK}" "$@"\n')
        wrapper.chmod(0o755)
        edge_source = temp / "edge_bridge.py"
        edge_source.write_text(
            "import base64, json, os, sys\n"
            f"sys.path.insert(0, {str(MOCK.parent)!r})\n"
            "import mock_bridge as mb\n"
            "class EdgeHandler(mb.Handler):\n"
            "  def get_version(self):\n"
            "    mode = os.environ.get('TNY_EDGE_MODE')\n"
            "    if mode == 'version-malformed':\n"
            "      self._unary_in(); self._json(200, {}); return\n"
            "    if mode == 'version-protocol':\n"
            '      self._unary_in(); self._json(200, {"bridgeVersion":"1.0.30","protocolVersion":"sdk.v2","capabilities":[]}); return\n'
            "    return super().get_version()\n"
            "  def _raw_frames(self, frames):\n"
            "    self.send_response(200); self.send_header('Content-Type', 'application/connect+json'); self.send_header('Transfer-Encoding', 'chunked'); self.end_headers()\n"
            "    [self._chunk(frame) for frame in frames]; self._chunk(b'')\n"
            "  def _raw_stream(self, payload):\n"
            "    self._raw_frames([mb.envelope(0, payload), mb.envelope(2, b'{}')])\n"
            "  def download_artifact(self):\n"
            "    self._stream_in(); mode = os.environ.get('TNY_EDGE_MODE')\n"
            "    if mode == 'artifact-exact':\n"
            '      data = base64.b64encode(b\'x\' * (4 * 1024 * 1024)).decode(); self._stream_out([{"data":data},{"data":data}]); return\n'
            '    if mode == \'artifact-empty-frame\': self._stream_out([None,{"data":"Zg=="}]); return\n'
            "    if mode == 'artifact-missing-data': self._stream_out([{}]); return\n"
            "    if mode == 'artifact-bad-json': self._raw_stream(b'{'); return\n"
            '    chunk = mb.frame({"data":"Zg=="})\n'
            "    if mode == 'artifact-missing-end': self._raw_frames([chunk]); return\n"
            "    if mode == 'artifact-duplicate-end': self._raw_frames([chunk,mb.envelope(2,b'{}'),mb.envelope(2,b'{}')]); return\n"
            "    if mode == 'artifact-data-after-end': self._raw_frames([mb.envelope(2,b'{}'),chunk]); return\n"
            "    if mode == 'artifact-compressed': self._raw_frames([mb.envelope(1,b'{}'),mb.envelope(2,b'{}')]); return\n"
            "    if mode == 'artifact-unknown-flags': self._raw_frames([mb.envelope(4,b'{}'),mb.envelope(2,b'{}')]); return\n"
            "    if mode == 'artifact-malformed-end': self._raw_frames([chunk,mb.envelope(2,b'[]')]); return\n"
            "    return super().download_artifact()\n"
            "mb.Handler = EdgeHandler\n"
            "mb.main()\n"
        )
        edge_wrapper = temp / "edge-bridge"
        edge_wrapper.write_text(
            f'#!/bin/sh\nexec "{sys.executable}" -u "{edge_source}" "$@"\n'
        )
        edge_wrapper.chmod(0o755)
        key = "key_cursor_management_fake"
        ambient_store_token = "ambient_store_token_must_not_forward"
        env = os.environ | {
            "HOME": str(home),
            "CURSOR_API_KEY": key,
            "CURSOR_SDK_STORE_CALLBACK_AUTH_TOKEN": ambient_store_token,
            "TNY_MOCK_DIR": str(state),
            "TNY_MOCK_CWD": str(workspace),
            "TNY_MOCK_CALL_STORE_ON_CREATE": "1",
            "TNY_MOCK_CALL_STORE_ON_SEND": "1",
        }
        if BRIDGE_READY_TIMEOUT_MS:
            env["TNY_CURSOR_BRIDGE_READY_TIMEOUT_MS"] = str(BRIDGE_READY_TIMEOUT_MS)

        def run(*args, stdin=None, ok=True, extra_env=None, bridge=wrapper):
            result = subprocess.run(
                [
                    str(TNY),
                    "--bridge-bin",
                    str(bridge),
                    "--cwd",
                    str(workspace),
                    "cursor",
                    *args,
                ],
                input=stdin,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=env | (extra_env or {}),
                timeout=SUBPROCESS_TIMEOUT,
            )
            if ok:
                assert result.returncode == 0, (args, result.stderr.decode())
            else:
                assert result.returncode != 0, args
            for output in (result.stdout, result.stderr):
                assert key.encode() not in output
                assert ambient_store_token.encode() not in output
                assert b"cursor-sdk-bridge ready" not in output
                token_file = state / "auth.token"
                if token_file.exists():
                    token = token_file.read_bytes().strip()
                    assert not token or token not in output
            return result

        invalid_timeout = run(
            "models",
            ok=False,
            extra_env={"TNY_CURSOR_BRIDGE_READY_TIMEOUT_MS": "60s"},
        )
        assert b"TNY_CURSOR_BRIDGE_READY_TIMEOUT_MS" in invalid_timeout.stderr
        models = json.loads(run("models").stdout)
        assert models["items"][0]["id"] == "mock-cursor-model"
        version = json.loads(
            run("rpc", "SdkBridgeControlService", "GetVersion", "{}").stdout
        )
        assert version["protocolVersion"] == "sdk.v1"
        ping_failure = run(
            "models", ok=False, extra_env={"TNY_MOCK_ERROR_ROUTE": "Ping"}
        )
        assert b"Ping failed" in ping_failure.stderr
        version_failure = run(
            "models", ok=False, extra_env={"TNY_MOCK_ERROR_ROUTE": "GetVersion"}
        )
        assert b"GetVersion failed" in version_failure.stderr
        malformed_version = run(
            "version",
            ok=False,
            bridge=edge_wrapper,
            extra_env={"TNY_EDGE_MODE": "version-malformed"},
        )
        assert b"malformed GetVersion response" in malformed_version.stderr
        wrong_protocol = run(
            "version",
            ok=False,
            bridge=edge_wrapper,
            extra_env={"TNY_EDGE_MODE": "version-protocol"},
        )
        assert b"expected sdk.v1" in wrong_protocol.stderr

        local = json.loads(run("create", "management-local").stdout)["agentId"]
        launch = json.loads((state / "launch.json").read_text())["storeCallback"]
        assert launch["authTokenPresent"] is True
        assert launch["authTokenArgPresent"] is False
        assert launch["tokenInArgv"] is False
        persisted_local = json.loads((state / "state.json").read_text())["agents"][
            local
        ]
        agent_options = persisted_local["options"]
        assert agent_options["mode"] == "AGENT_MODE_OPTION_PLAN"
        assert agent_options["tools"] == {"names": []}
        assert agent_options["disallowedTools"] == ["shell"]
        store_callback = json.loads((state / "store_callback_result.json").read_text())
        assert store_callback["output"]["agentId"] == local
        stored_agents = list((home / ".tny/cursor-sdk-store/agents").glob("*.json"))
        assert len(stored_agents) == 1
        send_lines = [
            json.loads(line) for line in run("send", local, "hello").stdout.splitlines()
        ]
        assert any("step" in item for item in send_lines)
        stream_store = json.loads(
            (state / "store_callback_stream_result.json").read_text()
        )
        assert stream_store["output"]["agentId"] == local
        run_id = next(
            item["result"]["runId"] for item in send_lines if "result" in item
        )
        assert json.loads(run("runs", local).stdout)["items"][0]["runId"] == run_id
        conversation = json.loads(run("conversation", run_id).stdout)
        assert json.loads(conversation["conversationJson"])["runId"] == run_id
        assert json.loads(run("messages", local).stdout)["messages"]
        observed = [
            json.loads(line)
            for line in run("observe", run_id, "offset:1").stdout.splitlines()
        ]
        assert any(item.get("offset") for item in observed)

        get_agent = {
            "agentId": local,
            "options": {"cwd": str(workspace), "apiKey": key},
        }
        raw = run(
            "rpc",
            "SdkAgentService",
            "GetAgent",
            "-",
            stdin=json.dumps(get_agent).encode(),
        )
        assert json.loads(raw.stdout)["agent"]["agentId"] == local

        before = (state / "routes.log").read_text().count("/DeleteAgent\n")
        refused = run("delete", local, ok=False)
        assert b"--yes" in refused.stderr
        after = (state / "routes.log").read_text().count("/DeleteAgent\n")
        assert before == after
        invalid = run("rpc", "SdkBridgeControlService", "Ping", "[]", ok=False)
        assert b"JSON object" in invalid.stderr

        (settings_dir / "settings.json").write_text(
            json.dumps(
                {
                    "cursor": {
                        "runtime": "cloud",
                        "agent_options": {
                            "mode": "AGENT_MODE_OPTION_PLAN",
                            "cloud": {
                                "repos": [
                                    {"url": "https://github.com/example/mock-one"}
                                ],
                                "metadata": {"lane": "management"},
                            },
                        },
                        "send_options": {
                            "enableSteps": True,
                            "cloud": {"envVars": {"RUN_KIND": "management"}},
                        },
                    }
                }
            )
        )
        cloud = json.loads(run("create", "management-cloud").stdout)["agentId"]
        cloud_launch = json.loads((state / "launch.json").read_text())["storeCallback"]
        assert cloud_launch["authTokenPresent"] is False
        assert cloud_launch["authTokenArgPresent"] is False
        persisted_cloud = json.loads((state / "state.json").read_text())["agents"][
            cloud
        ]
        assert persisted_cloud["runtime"] == "cloud"
        assert persisted_cloud["options"]["cloud"]["metadata"] == {"lane": "management"}
        cloud_frames = [
            json.loads(line) for line in run("send", cloud, "cloud").stdout.splitlines()
        ]
        assert any("step" in item for item in cloud_frames)
        cloud_run = next(
            item["result"]["runId"] for item in cloud_frames if "result" in item
        )
        raw_observe = {
            "runId": cloud_run,
            "afterOffset": "offset:1",
        }
        raw_frames = [
            json.loads(line)
            for line in run(
                "rpc",
                "SdkAgentService",
                "ObserveRun",
                "-",
                stdin=json.dumps(raw_observe).encode(),
            ).stdout.splitlines()
        ]
        assert any(item.get("offset") for item in raw_frames)
        artifacts = json.loads(run("artifacts", cloud).stdout)["artifacts"]
        downloaded = run("download", cloud, artifacts[0]["path"]).stdout
        assert downloaded == f"artifact from {cloud}\n".encode()
        exact = run(
            "download",
            cloud,
            artifacts[0]["path"],
            bridge=edge_wrapper,
            extra_env={"TNY_EDGE_MODE": "artifact-exact"},
        )
        assert len(exact.stdout) == 8 * 1024 * 1024
        empty_frame = run(
            "download",
            cloud,
            artifacts[0]["path"],
            bridge=edge_wrapper,
            extra_env={"TNY_EDGE_MODE": "artifact-empty-frame"},
        )
        assert empty_frame.stdout == b"f"
        for mode in ("artifact-missing-data", "artifact-bad-json"):
            malformed_frame = run(
                "download",
                cloud,
                artifacts[0]["path"],
                ok=False,
                bridge=edge_wrapper,
                extra_env={"TNY_EDGE_MODE": mode},
            )
            assert malformed_frame.stdout == b""
            assert b"malformed artifact chunk" in malformed_frame.stderr
        stream_protocol_errors = {
            "artifact-missing-end": b"without an EndStream",
            "artifact-duplicate-end": b"duplicate EndStream",
            "artifact-data-after-end": b"data after EndStream",
            "artifact-compressed": b"compressed Connect",
            "artifact-unknown-flags": b"unknown Connect",
            "artifact-malformed-end": b"malformed EndStream",
        }
        for mode, expected_error in stream_protocol_errors.items():
            invalid_stream = run(
                "download",
                cloud,
                artifacts[0]["path"],
                ok=False,
                bridge=edge_wrapper,
                extra_env={"TNY_EDGE_MODE": mode},
            )
            if mode in {
                "artifact-missing-end",
                "artifact-duplicate-end",
                "artifact-malformed-end",
            }:
                assert invalid_stream.stdout == b"f"
            else:
                assert invalid_stream.stdout == b""
            assert expected_error in invalid_stream.stderr
        malformed = run(
            "download",
            cloud,
            artifacts[0]["path"],
            ok=False,
            extra_env={"TNY_MOCK_ARTIFACT_BASE64": "Zh=="},
        )
        assert malformed.stdout == b""
        assert b"not valid base64" in malformed.stderr
        oversized = run(
            "download",
            cloud,
            artifacts[0]["path"],
            ok=False,
            extra_env={"TNY_MOCK_ARTIFACT_OVERSIZE": "1"},
        )
        assert len(oversized.stdout) == 4_300_000
        assert b"artifact exceeds" in oversized.stderr
        usage = json.loads(run("usage", cloud, cloud_run).stdout)
        assert usage["usage"]["runs"][0]["runId"] == cloud_run

        delete_request = {
            "agentId": cloud,
            "options": {"apiKey": key},
        }
        run(
            "rpc",
            "SdkAgentService",
            "DeleteAgent",
            "-",
            "--yes",
            stdin=json.dumps(delete_request).encode(),
        )
        failures = state / "failures.log"
        assert not failures.exists() or not failures.read_text(), failures.read_text()
        pid = int((state / "pid.txt").read_text())
        alive = True
        for _ in range(20):
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                alive = False
                break
            time.sleep(0.025)
        assert not alive, "mock bridge survived management command cleanup"
        print("ok  cursor management aliases, raw RPC, streams, artifact and safety")
    finally:
        shutil.rmtree(temp)


if __name__ == "__main__":
    main()
