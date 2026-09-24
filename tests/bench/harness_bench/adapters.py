"""Isolated CLI adapters for the cross-harness recording benchmark."""

import base64
import json
import os
import secrets
import shutil
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

ADAPTERS = ("tny", "codex", "pi", "omp", "hermes", "opencode", "fx", "unreal-agent")
SESSION_ADAPTERS = frozenset(("tny", "codex"))
UNREAL_BIN = Path("/home/tomas/.cache/tny-opt/bin/unreal-agent-runner")


def _binary(name):
    binary = shutil.which(name)
    if binary and "/mise/shims/" in binary:
        resolved = subprocess.run(
            ["mise", "which", name],
            capture_output=True,
            text=True,
            check=True,
            timeout=10,
        ).stdout.strip()
        return resolved
    return binary


def _jwt():
    def part(value):
        return base64.urlsafe_b64encode(json.dumps(value).encode()).decode().rstrip("=")

    return ".".join(
        (
            part({"alg": "none"}),
            part(
                {
                    "exp": int(time.time()) + 86400,
                    "nonce": secrets.token_hex(16),
                    "https://api.openai.com/auth": {
                        "chatgpt_account_id": "fixture-account",
                        "chatgpt_plan_type": "pro",
                    },
                }
            ),
            "fixture",
        )
    )


@dataclass
class Invocation:
    command: list[str]
    env: dict[str, str]


def isolated_env(run_dir, proxy_url):
    """Keep executable lookup and locale, but discard user agent and provider state."""
    home = Path(run_dir) / "home"
    codex_home = home / ".codex"
    codex_home.mkdir(parents=True, exist_ok=True)
    token = _jwt()
    (codex_home / "auth.json").write_text(
        json.dumps(
            {
                "auth_mode": "chatgpt",
                "tokens": {
                    "access_token": token,
                    "id_token": token,
                    "refresh_token": "fixture",
                    "account_id": "fixture-account",
                },
                "last_refresh": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            }
        )
    )
    (codex_home / "auth.json").chmod(0o600)
    env = {
        key: value
        for key, value in os.environ.items()
        if key
        in {
            "PATH",
            "LANG",
            "LC_ALL",
            "TERM",
            "TMPDIR",
            "SSL_CERT_FILE",
            "NIX_SSL_CERT_FILE",
        }
    }
    env.update(
        HOME=str(home),
        CODEX_HOME=str(codex_home),
        XDG_CONFIG_HOME=str(home / ".config"),
        XDG_DATA_HOME=str(home / ".local" / "share"),
        XDG_CACHE_HOME=str(home / ".cache"),
        TNY_CODEX_BASE_URL=proxy_url + "/v1",
        TNY_PROVIDER_RETRIES="0",
        OPENAI_API_KEY=token,
    )
    node = _binary("node")
    if node:
        env["PATH"] = str(Path(node).parent) + os.pathsep + env.get("PATH", "")
    return env


def invocation(name, run_dir, proxy_url, prompt, model, effort, tny_bin):
    env = isolated_env(run_dir, proxy_url)
    binary = (
        str(Path(tny_bin).resolve())
        if name == "tny"
        else str(UNREAL_BIN)
        if name == "unreal-agent"
        else _binary(name)
    )
    if not binary or not Path(binary).exists():
        raise RuntimeError(f"{name} executable not found")
    if name == "tny":
        return Invocation(
            [
                binary,
                "--provider",
                "codex",
                "--model",
                model,
                "--effort",
                effort,
                "ask",
                "--json",
                prompt,
            ],
            env,
        )
    if name == "codex":
        # The provider's base_url includes /v1, matching the documented override.
        url = proxy_url + "/v1"
        return Invocation(
            [
                binary,
                "exec",
                "--ignore-user-config",
                "--ignore-rules",
                "-m",
                model,
                "-c",
                f'model_reasoning_effort="{effort}"',
                "-c",
                'model_provider="harness_bench"',
                "-c",
                f'model_providers.harness_bench={{name="harness bench",base_url="{url}",wire_api="responses",requires_openai_auth=true,supports_websockets=false}}',
                "-c",
                'web_search="disabled"',
                "-c",
                "features.shell_snapshot=false",
                "--dangerously-bypass-approvals-and-sandbox",
                "--skip-git-repo-check",
                "--json",
                prompt,
            ],
            env,
        )
    if name == "pi":
        agent_dir = Path(env["HOME"]) / ".pi" / "agent"
        agent_dir.mkdir(parents=True)
        env["PI_CODING_AGENT_DIR"] = str(agent_dir)
        env.update(PI_OFFLINE="1", PI_SKIP_VERSION_CHECK="1", PI_TELEMETRY="0")
        (agent_dir / "models.json").write_text(
            json.dumps({"providers": {"openai-codex": {"baseUrl": proxy_url}}})
        )
        (agent_dir / "settings.json").write_text(json.dumps({"transport": "sse"}))
        (agent_dir / "auth.json").write_text(
            json.dumps(
                {
                    "openai-codex": {
                        "type": "oauth",
                        "access": env["OPENAI_API_KEY"],
                        "refresh": "fixture",
                        "expires": (int(time.time()) + 86400) * 1000,
                    }
                }
            )
        )
        return Invocation(
            [
                binary,
                "--provider",
                "openai-codex",
                "--model",
                model,
                "--thinking",
                effort,
                "--mode",
                "json",
                "--print",
                prompt,
            ],
            env,
        )
    if name == "omp":
        agent_dir = Path(env["HOME"]) / ".omp" / "agent"
        agent_dir.mkdir(parents=True)
        (agent_dir / "models.yml").write_text(
            f"providers:\n  openai-codex:\n    baseUrl: {proxy_url}\n"
        )
        env.update(
            OPENAI_CODEX_OAUTH_TOKEN=env["OPENAI_API_KEY"],
            PI_CODEX_WEBSOCKET="0",
            OMP_SKIP_SETUP="1",
        )
        return Invocation(
            [
                binary,
                "--model",
                f"openai-codex/{model}",
                "--thinking",
                effort,
                "--mode",
                "json",
                "--print",
                "--auto-approve",
                "--no-title",
                "--no-skills",
                "--no-rules",
                prompt,
            ],
            env,
        )
    if name == "hermes":
        hermes_home = Path(env["HOME"]) / ".hermes"
        hermes_home.mkdir(parents=True)
        (hermes_home / "config.yaml").write_text(
            "auxiliary:\n  title_generation:\n    enabled: false\n"
            "memory:\n  nudge_interval: 0\n"
            "skills:\n  creation_nudge_interval: 0\n"
        )
        env["HERMES_HOME"] = str(hermes_home)
        env["OPENAI_BASE_URL"] = proxy_url + "/v1"
        return Invocation(
            [
                binary,
                "chat",
                "--oneshot",
                "--format",
                "stream-json",
                "--provider",
                "openai-api",
                "--model",
                model,
                "--reasoning",
                effort,
                "--yolo",
                "--ignore-rules",
                "--query",
                prompt,
            ],
            env,
        )
    if name == "fx":
        fx_dir = Path(env["HOME"]) / ".fx"
        fx_dir.mkdir(mode=0o700)
        (fx_dir / "settings.json").write_text(
            json.dumps(
                {"provider": "codex", "models": {"codex": model}, "effort": effort}
            )
        )
        auth_file = fx_dir / "chatgpt-auth.json"
        auth_file.write_text(
            json.dumps(
                {
                    "version": 1,
                    "access_token": env["OPENAI_API_KEY"],
                    "refresh_token": "fixture",
                    "expires_at_ms": (int(time.time()) + 86400) * 1000,
                    "account_id": "fixture-account",
                }
            )
        )
        auth_file.chmod(0o600)
        env.update(
            FX_PROVIDER="codex",
            FX_MODEL=model,
            FX_E2E_OPENAI_CODEX_RESPONSES_URL=proxy_url
            + "/backend-api/codex/responses",
            FX_E2E_OPENAI_CODEX_MODELS_URL=proxy_url + "/backend-api/codex/models",
        )
        return Invocation(
            [binary, "ask", "--full-access", "--json", "--no-save", "--", prompt],
            env,
        )
    if name == "unreal-agent":
        env.update(
            UNREAL_HARNESS_LLM_PROVIDER="openai-codex",
            UNREAL_HARNESS_LLM_BASE_URL=proxy_url + "/backend-api/codex",
            UNREAL_HARNESS_LLM_MODEL=model,
            UNREAL_HARNESS_LLM_MAX_ATTEMPTS="1",
        )
        request = json.dumps(
            {
                "prompt": prompt,
                "model": model,
                "thinking_level": effort,
                "max_attempts": 1,
            }
        )
        return Invocation([binary, request], env)
    if name == "opencode":
        config_dir = Path(env["XDG_CONFIG_HOME"]) / "opencode"
        config_dir.mkdir(parents=True)
        (config_dir / "opencode.json").write_text(
            json.dumps(
                {
                    "$schema": "https://opencode.ai/config.json",
                    "provider": {
                        "bench": {
                            "npm": "@ai-sdk/openai",
                            "name": "Benchmark proxy",
                            "options": {
                                "baseURL": proxy_url + "/v1",
                                "apiKey": env["OPENAI_API_KEY"],
                            },
                            "models": {
                                model: {
                                    "name": model,
                                    "limit": {"context": 200000, "output": 32000},
                                    "variants": {effort: {"reasoningEffort": effort}},
                                }
                            },
                        }
                    },
                    "tools": {"websearch": False},
                }
            )
        )
        env["OPENCODE_DISABLE_MODELS_FETCH"] = "1"
        return Invocation(
            [
                binary,
                "run",
                "--pure",
                "--auto",
                "--format",
                "json",
                "--model",
                f"bench/{model}",
                "--variant",
                effort,
                prompt,
            ],
            env,
        )
    raise RuntimeError(f"{name}: ChatGPT Responses proxy configuration unverified")


def session_invocation(
    name, run_dir, proxy_url, prompt, model, effort, tny_bin, resume_id
):
    """Use the same isolated HOME and resume the existing noninteractive session."""
    if name not in SESSION_ADAPTERS:
        raise ValueError(f"{name}: noninteractive session resume is unverified")
    call = invocation(name, run_dir, proxy_url, prompt, model, effort, tny_bin)
    if resume_id:
        verb = "--resume" if name == "tny" else "resume"
        call.command[-1:-1] = [verb, resume_id]
    return call


def resume_id(name, stdout):
    """Read the native session or thread identifier from CLI JSON output."""
    if name == "tny":
        try:
            return json.loads(stdout).get("session_id")
        except ValueError:
            return None
    if name == "codex":
        for line in stdout.splitlines():
            try:
                event = json.loads(line)
            except ValueError:
                continue
            if event.get("type") == "thread.started":
                return event.get("thread_id")
    return None


def final_message(name, stdout):
    if name == "tny":
        try:
            return json.loads(stdout).get("output", "")
        except ValueError:
            return ""
    if name == "fx":
        try:
            result = json.loads(stdout)
            return result.get("final_output") or result.get("output") or ""
        except ValueError:
            return ""
    text = ""
    for line in stdout.splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if name == "codex" and event.get("type") == "item.completed":
            item = event.get("item") or {}
            if item.get("type") == "agent_message":
                text = item.get("text", "")
        elif name in {"pi", "omp"} and event.get("type") == "message_end":
            message = event.get("message") or {}
            if message.get("role") == "assistant":
                text = "".join(
                    part.get("text", "")
                    for part in message.get("content", [])
                    if part.get("type") == "text"
                )
        elif name == "hermes" and event.get("type") in {
            "assistant",
            "final",
            "message",
            "result",
        }:
            value = event.get("content") or event.get("text") or ""
            if isinstance(value, str):
                text = value
        elif name == "opencode" and event.get("type") == "text":
            text += (event.get("part") or {}).get("text", "")
        elif name == "unreal-agent" and event.get("Kind") == "model_response":
            response = (event.get("Data") or {}).get("Response") or {}
            for item in response.get("Output") or []:
                data = item.get("Data") or {}
                if item.get("Type") == "message" and data.get("Role") == "assistant":
                    text = data.get("Text", "")
    return text
