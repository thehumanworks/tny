#!/usr/bin/env python3
"""Native profiles use HTTP without discovering or launching vendor agents."""

import json
import os
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs

ROOT = Path(__file__).resolve().parents[2]
TNY = Path(
    os.environ.get("TNY", sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/tny")
).resolve()


class Issuer(BaseHTTPRequestHandler):
    grants = []

    def log_message(self, *_):
        pass

    def do_POST(self):
        form = parse_qs(self.rfile.read(int(self.headers["Content-Length"])).decode())
        if self.path == "/oauth2/device/code":
            body = {
                "device_code": "fixture-device",
                "user_code": "fixture-code",
                "verification_uri": "https://example.invalid/login",
                "expires_in": 600,
                "interval": 1,
            }
        else:
            assert self.path == "/oauth2/token"
            self.grants.append(form["grant_type"][0])
            body = {
                "access_token": "fixture-session",
                "refresh_token": "fixture-refresh",
                "expires_in": 3600,
            }
        data = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    with tempfile.TemporaryDirectory(prefix="tny-native-profiles-") as temp:
        root = Path(temp)
        home, ws, bins = (root / name for name in ("home", "ws", "bin"))
        for path in (home, ws, bins):
            path.mkdir()
        marker = root / "vendor-invoked"
        for name in (
            "codex",
            "claude",
            "grok",
            "cursor-sdk-bridge",
            "agent",
            "gemini",
            "opencode",
            "claude-agent-acp",
        ):
            path = bins / name
            path.write_text(f"#!/bin/sh\nprintf invoked > '{marker}'\nexit 97\n")
            path.chmod(0o700)
        env = {
            "HOME": str(home),
            "PATH": str(bins) + os.pathsep + os.defpath,
            "TMPDIR": str(root),
            "TNY_EXTENSIONS": "0",
            "TNY_ISOLATE": "0",
            "TNY_PROVIDER_RETRIES": "0",
        }
        settings = home / ".tny/settings.json"
        settings.parent.mkdir()

        def run(*args, extra=None, ok=True):
            result = subprocess.run(
                [str(TNY), "--cwd", str(ws), *args],
                env=dict(env, **(extra or {})),
                capture_output=True,
                text=True,
                timeout=30,
            )
            assert (result.returncode == 0) == ok, (
                args,
                result.returncode,
                result.stderr,
            )
            assert not marker.exists(), "vendor executable was invoked"
            return result

        (home / ".claude").mkdir()
        (home / ".claude/.credentials.json").write_text(
            json.dumps({"claudeAiOauth": {"accessToken": "fixture-ignored"}})
        )
        ignored = {
            "CLAUDE_CODE_OAUTH_TOKEN": "fixture-ignored",
            "CURSOR_API_KEY": "fixture-ignored",
            "ANTHROPIC_API_KEY": "fixture-ignored",
        }
        result = run("status", "--json", extra=ignored)
        assert json.loads(result.stdout)["backend"] == "openai"
        for provider in ("cursor", "claude"):
            result = run("--provider", provider, "ask", "hello", ok=False)
            assert "removed" in result.stderr
        for flag in ("--bridge-bin",):
            assert (
                "unknown flag" in run(flag, "fixture", "ask", "hello", ok=False).stderr
            )
        # Optional ACP requires explicit command/profile; no ambient discovery.
        for provider in ("acp", "acp@fixture", "acp:fixture"):
            result = run("--provider", provider, "ask", "hello", ok=False)
            assert "removed" not in result.stderr
            if provider == "acp":
                assert "no ACP agent configured" in result.stderr, result.stderr
            else:
                assert f"ACP provider '{provider}' is not defined" in result.stderr, (
                    result.stderr
                )
                assert "settings.json acp.fixture" in result.stderr, result.stderr
        for command in ("acp", "cursor"):
            assert "removed" in run(command, ok=False).stderr
        for config in (
            {"cursor": {}},
            {"last_provider": "cursor"},
            {"last_provider": "claude"},
            {"openai": {"api_key": "fixture-stored"}},
        ):
            settings.write_text(json.dumps(config))
            result = run("status", extra={"OPENAI_API_KEY": "fixture-env"}, ok=False)
            assert "removed" in result.stderr
            assert "fixture-stored" not in result.stderr
        settings.write_text(json.dumps({"last_provider": "missinggateway"}))
        assert (
            "gateway"
            in run("status", extra={"OPENAI_API_KEY": "fixture-env"}, ok=False).stderr
        )
        run("--provider", "openai", "status", extra={"OPENAI_API_KEY": "fixture-env"})
        settings.write_text("{}")
        result = run(
            "provider",
            "setup",
            "fixture",
            "--base-url",
            "http://127.0.0.1:1/v1",
            "--api-key-env",
            "lowercase_key",
        )
        assert "not set" in result.stderr
        assert (
            json.loads(settings.read_text())["fixture"]["api_key_env"]
            == "lowercase_key"
        )
        settings.write_text("{}")
        for url in (
            "https://example.com/v1",
            "http://127.0.0.1.evil/v1",
            "http://127.0.0.1@evil/v1",
            "ftp://127.0.0.1/v1",
            "http://127.0.0.1:12junk/v1",
            "http://127.0.0.1:65536/v1",
        ):
            result = run(
                "--provider",
                "grok",
                "status",
                extra={"TNY_GROK_BASE_URL": url},
                ok=False,
            )
            assert "loopback" in result.stderr
        assert (
            "removed"
            in run(
                "provider", "setup", "aiproxy", "--api-key", "fixture-stored", ok=False
            ).stderr
        )

        def wire(provider, shape, subscription=False, configured=False):
            headers = (
                "Authorization=Bearer fixture-session"
                if subscription
                else "Authorization=Bearer fixture-env"
            )
            if subscription:
                headers += (
                    ";X-XAI-Token-Auth=xai-grok-cli;x-grok-model-override=mock-model"
                )
            mock_env = {
                "PATH": os.defpath,
                "HOME": str(home),
                "MOCK_EXPECT_WIRE": shape,
                "MOCK_EXPECT_HEADERS": headers,
            }
            mock_env["MOCK_REJECT_HEADERS"] = (
                "chatgpt-account-id;OpenAI-Beta"
                if subscription
                else "X-XAI-Token-Auth;x-grok-model-override;x-grok-client-version;chatgpt-account-id;OpenAI-Beta"
            )
            mock = subprocess.Popen(
                [sys.executable, str(ROOT / "tests/integration/mock_openai.py"), "0"],
                env=mock_env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            try:
                ready = mock.stdout.readline().strip()
                assert ready.startswith("ready"), ready
                port = ready.split()[-1]
                url = f"http://127.0.0.1:{port}/v1"
                extra = {}
                if provider == "grok":
                    extra = {"TNY_GROK_BASE_URL": url, "XAI_API_KEY": "fixture-env"}
                elif configured:
                    settings.write_text(
                        json.dumps(
                            {
                                provider: {
                                    "base_url": url,
                                    "api_key_env": "GATEWAY_KEY",
                                    "wire_api": shape,
                                }
                            }
                        )
                    )
                    extra = {"GATEWAY_KEY": "fixture-env"}
                else:
                    prefix = provider.upper()
                    extra = {
                        prefix + "_BASE_URL": url,
                        prefix + "_API_KEY": "fixture-env",
                        prefix + "_WIRE_API": shape,
                    }
                result = run(
                    "--backend",
                    provider,
                    "--model",
                    "mock-model",
                    "--ephemeral",
                    "ask",
                    "--json",
                    "list files",
                    extra=extra,
                )
                assert "MOCK-OK" in result.stdout
                assert "fixture-env" not in result.stdout + result.stderr
                assert "fixture-session" not in result.stdout + result.stderr
            finally:
                mock.terminate()
                mock.wait(timeout=5)
                mock.stdout.close()
                mock.stderr.close()
                settings.write_text("{}")

        for provider in ("openrouter", "aiproxy"):
            for shape in ("responses", "chat"):
                wire(provider, shape)
                wire(provider, shape, configured=True)
        wire("claude", "chat", configured=True)
        wire("grok", "responses")
        server = ThreadingHTTPServer(("127.0.0.1", 0), Issuer)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        issuer = f"http://127.0.0.1:{server.server_port}"
        env["GROK_OAUTH2_ISSUER"] = issuer
        try:
            run("--provider", "grok", "login")
            auth = home / ".grok/auth.json"
            data = json.loads(auth.read_text())
            entry = next(iter(data.values()))
            assert entry["key"] == "fixture-session"
            entry["expires_at"] = "2020-01-01T00:00:00Z"
            auth.write_text(json.dumps(data))
            codex = home / ".codex"
            codex.mkdir()
            (codex / "auth.json").write_text(
                json.dumps({"OPENAI_API_KEY": "fixture-retired"})
            )
            result = run("status", "--json")
            assert json.loads(result.stdout)["backend"] == "grok"
            assert "stored OPENAI_API_KEY" not in result.stderr
            assert (
                "stored OPENAI_API_KEY"
                in run("--provider", "codex", "status", ok=False).stderr
            )
            wire("grok", "chat", subscription=True)
            assert "refresh_token" in Issuer.grants
            assert "urn:ietf:params:oauth:grant-type:device_code" in Issuer.grants
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
        assert not marker.exists()
    print(
        "test_native_profiles: HTTP profiles, migration errors, Grok login/refresh and no vendor binaries passed"
    )


if __name__ == "__main__":
    main()
