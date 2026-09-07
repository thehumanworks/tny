#!/usr/bin/env python3
"""Opt-in ChatGPT cache benchmark for tny and Codex CLI (ADR 0077).

Only a generated fictional catalog reaches the provider. Real credentials
stay in the forwarding process; child homes contain fake credentials.
The output contains usage/timing/size metadata, never prompts, responses,
headers, affinity tokens, account IDs, or encrypted reasoning.
"""

import argparse
import base64
import gzip
import hashlib
import json
import os
import secrets
import shutil
import statistics
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

UPSTREAM = "https://chatgpt.com/backend-api/codex/responses"
ROUTING_HEADERS = {
    "session-id",
    "thread-id",
    "originator",
    "version",
    "x-openai-internal-codex-responses-lite",
}


def summarize(rows):
    """Token-weighted rates; unknown usage is never treated as zero."""
    if not rows or any(
        row.get("input_tokens") is None or row.get("output_tokens") is None
        for row in rows
    ):
        return None
    totals = {
        key: sum(row[key] for row in rows)
        for key in ["input_tokens", "output_tokens", "request_bytes"]
    }
    cached = [row.get("cached_input_tokens") for row in rows]
    totals["cached_input_tokens"] = (
        sum(cached) if all(v is not None for v in cached) else None
    )
    totals["requests_with_cache_hits"] = (
        sum(v > 0 for v in cached) if all(v is not None for v in cached) else None
    )
    totals["uncached_input_tokens"] = (
        None
        if totals["cached_input_tokens"] is None
        else totals["input_tokens"] - totals["cached_input_tokens"]
    )
    totals["cache_hit_rate"] = (
        None
        if not totals["input_tokens"] or totals["cached_input_tokens"] is None
        else totals["cached_input_tokens"] / totals["input_tokens"]
    )
    first = [
        row["first_event_ms"] for row in rows if row.get("first_event_ms") is not None
    ]
    totals["median_first_event_ms"] = statistics.median(first) if first else None
    totals["requests"] = len(rows)
    return totals


class Forwarder(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        # Only this endpoint is authorized. In particular, this is not an
        # open proxy for model-invoked tools or arbitrary destinations.
        if self.path != "/v1/responses":
            self.send_error(404)
            return
        state = self.server.state
        if not secrets.compare_digest(
            self.headers.get("Authorization", ""), state["expected_auth"]
        ):
            self.send_error(401)
            return
        raw = self.rfile.read(int(self.headers["Content-Length"]))
        encoding = self.headers.get("Content-Encoding")
        if encoding == "gzip":
            raw = gzip.decompress(raw)
        elif encoding:
            self.send_error(415)
            return
        body = json.loads(raw)
        state["done"].clear()
        row = {
            "round": state["round"],
            "turn": state["turn"],
            "request_bytes": len(raw),
            "cache_key_present": bool(body.get("prompt_cache_key")),
            "affinity_sent": "x-codex-turn-state" in self.headers,
            "input_tokens": None,
            "output_tokens": None,
        }
        headers = {
            "Authorization": "Bearer " + state["auth"]["access_token"],
            "chatgpt-account-id": state["auth"]["account_id"],
            "Content-Type": "application/json",
            "Accept": "text/event-stream",
            "OpenAI-Beta": "responses=v1",
        }
        for key, value in self.headers.items():
            if key.lower().startswith("x-codex-") or key.lower() in ROUTING_HEADERS:
                headers[key] = value
        request = urllib.request.Request(UPSTREAM, data=raw, headers=headers)
        start = time.monotonic()
        row["first_event_ms"] = None
        try:
            with urllib.request.urlopen(request, timeout=90) as response:
                row["http_status"] = response.status
                self.send_response(response.status)
                self.send_header("Content-Type", "text/event-stream")
                affinity = response.headers.get("x-codex-turn-state")
                if affinity:
                    self.send_header("x-codex-turn-state", affinity)
                self.end_headers()
                for line in response:
                    if line.startswith(b"data: "):
                        try:
                            event = json.loads(line[6:])
                        except ValueError:
                            event = {}
                        if (
                            event.get("type")
                            in [
                                "response.output_text.delta",
                                "response.function_call_arguments.delta",
                                "response.output_item.added",
                            ]
                            and row["first_event_ms"] is None
                        ):
                            row["first_event_ms"] = round(
                                (time.monotonic() - start) * 1000, 3
                            )
                        if event.get("type") in [
                            "response.completed",
                            "response.incomplete",
                            "response.failed",
                        ]:
                            usage = event.get("response", {}).get("usage", {})
                            row.update(
                                input_tokens=usage.get("input_tokens"),
                                output_tokens=usage.get("output_tokens"),
                                cached_input_tokens=usage.get(
                                    "input_tokens_details", {}
                                ).get("cached_tokens"),
                                cache_write_tokens=usage.get(
                                    "input_tokens_details", {}
                                ).get("cache_write_tokens"),
                                completion=event["type"],
                            )
                    self.wfile.write(line)
                    self.wfile.flush()
        except urllib.error.HTTPError as error:
            # Error bodies can echo input or credentials. Retain only status.
            row["http_status"] = error.code
            self.send_error(error.code)
            error.close()
        except (OSError, ValueError) as error:
            row["error"] = type(error).__name__
        finally:
            row["elapsed_ms"] = round((time.monotonic() - start) * 1000, 3)
            state["rows"].append(row)
            state["done"].set()


def fixture_env(root, url):
    home = root / "home"
    codex_dir = home / ".codex"
    codex_dir.mkdir(parents=True)

    def b64(obj):
        return base64.urlsafe_b64encode(json.dumps(obj).encode()).decode().rstrip("=")

    token = (
        b64({"alg": "none"})
        + "."
        + b64(
            {
                "exp": int(time.time()) + 86400,
                "nonce": secrets.token_hex(32),
                "https://api.openai.com/auth": {
                    "chatgpt_account_id": "fixture-account",
                    "chatgpt_plan_type": "pro",
                },
            }
        )
        + ".fixture"
    )
    (codex_dir / "auth.json").write_text(
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
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(
            ("TNY_", "CODEX_", "CHATGPT_", "OPENAI_", "CLAUDE_", "XAI_")
        )
        and not key.endswith(("_API_KEY", "_BASE_URL"))
    }
    env.update(
        HOME=str(home),
        CODEX_HOME=str(codex_dir),
        TNY_CODEX_BASE_URL=url,
        TNY_ISOLATE="0",
        TNY_TOOLS="terminal",
        TNY_PROVIDER_RETRIES="0",
    )
    return env, "Bearer " + token


def client_command(args, client, binary, url, session_id):
    if client != "codex":
        command = [
            binary,
            "--provider",
            "codex",
            "--model",
            args.model,
            "--effort",
            "low",
            "ask",
            "--json",
        ]
        return command + (["--resume", session_id] if session_id else [])
    command = [
        binary,
        "exec",
        "--ignore-user-config",
        "--ignore-rules",
        "-m",
        args.model,
        "-c",
        'model_reasoning_effort="low"',
        "-c",
        'model_provider="cache_bench"',
        "-c",
        f'model_providers.cache_bench={{name="cache bench",base_url="{url}",wire_api="responses",requires_openai_auth=true,supports_websockets=false}}',
        "-c",
        'web_search="disabled"',
        "-c",
        "features.shell_snapshot=false",
        "--skip-git-repo-check",
        "--json",
    ]
    return command + (["resume", session_id] if session_id else [])


def run_round(args, client, binary, auth, round_index):
    with tempfile.TemporaryDirectory(prefix="tny-cache-bench-") as directory:
        root = Path(directory)
        ws = root / "workspace"
        ws.mkdir()
        colors = ["blue", "green", "red"]
        catalog = "\n".join(
            f"Item {i:03d}: code C{i:03d}, color {colors[i % 3]}, shelf {i % 11}, quantity {i * 7 + 3}."
            for i in range(args.records)
        )
        (ws / "AGENTS.md").write_text(
            "Synthetic cache benchmark. Answer catalog questions with only the color. Do not use tools unless asked.\n"
            + catalog
            + "\n"
        )
        (ws / "facts.txt").write_text("cedar\n")
        state = {
            "auth": auth,
            "round": round_index,
            "turn": 0,
            "rows": [],
            "done": threading.Event(),
        }
        state["done"].set()
        server = ThreadingHTTPServer(("127.0.0.1", 0), Forwarder)
        server.state = state
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        url = f"http://127.0.0.1:{server.server_port}/v1"
        env, state["expected_auth"] = fixture_env(root, url)
        if client != "codex" and getattr(args, "cache_scope", None):
            env["TNY_OPENAI_CACHE_SCOPE"] = args.cache_scope
        thread.start()
        session_id = None
        turns = []
        try:
            for turn in range(args.turns):
                state["turn"] = turn
                if args.scenario == "fresh":
                    session_id = None
                expected = colors[turn % 3]
                prompt = f"What is the color of catalog item {turn:03d}? Answer only the color without tools."
                if args.scenario == "tools":
                    expected = "cedar 6"
                    prompt = "Use your shell tool to run cat facts.txt. After you see the result, use a second shell call to run wc -c facts.txt. Execute the two commands separately, in order. Then reply exactly: cedar 6"
                start = time.monotonic()
                result = subprocess.run(
                    client_command(args, client, binary, url, session_id) + [prompt],
                    cwd=ws,
                    env=env,
                    capture_output=True,
                    timeout=120,
                )
                output = ""
                reported_usage = None
                if result.returncode == 0:
                    if client != "codex":
                        obj = json.loads(result.stdout)
                        session_id = obj["session_id"]
                        output = obj["output"]
                        reported_usage = obj.get("usage")
                    else:
                        for line in result.stdout.splitlines():
                            obj = json.loads(line)
                            if obj.get("type") == "thread.started":
                                session_id = obj["thread_id"]
                            if (
                                obj.get("type") == "item.completed"
                                and obj["item"].get("type") == "agent_message"
                            ):
                                output = obj["item"].get("text", "")
                # Completion can reach the child before the proxy's finally
                # block, so wait for that request's accounting before scoring.
                state["done"].wait(timeout=5)
                rows = [row for row in state["rows"] if row["turn"] == turn]
                summary = summarize(rows)
                correct = output.strip().lower().rstrip(".") == expected
                tool_rounds = args.scenario != "tools" or len(rows) >= 3
                valid = (
                    result.returncode == 0
                    and correct
                    and tool_rounds
                    and summary is not None
                    and all(
                        row.get("completion") == "response.completed" for row in rows
                    )
                )
                if reported_usage and summary:
                    valid = valid and all(
                        reported_usage[key] == summary[key]
                        for key in [
                            "input_tokens",
                            "output_tokens",
                            "cached_input_tokens",
                        ]
                    )
                turns.append(
                    {
                        "turn": turn,
                        "exit_code": result.returncode,
                        "correct": correct,
                        "valid": valid,
                        "elapsed_ms": round((time.monotonic() - start) * 1000, 3),
                        "usage": summary,
                    }
                )
                if not valid:
                    break
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
        return {
            "client": client,
            "round": round_index,
            "turns": turns,
            "requests": state["rows"],
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--live", action="store_true", help="explicitly authorize subscription calls"
    )
    parser.add_argument("--tny", default="build/tny")
    parser.add_argument(
        "--baseline", help="pre-change release binary; built in a separate worktree"
    )
    parser.add_argument("--codex", default="codex")
    parser.add_argument("--client", choices=["all", "tny", "codex"], default="all")
    parser.add_argument(
        "--cache-scope",
        choices=["workspace", "session"],
        help="override tny cache routing scope; Codex is unchanged",
    )
    parser.add_argument(
        "--model",
        default="gpt-5.6-luna",
        choices=["gpt-5.6-luna", "gpt-5.6-sol", "gpt-5.6-terra", "gpt-6-astra"],
    )
    parser.add_argument(
        "--auth-file",
        type=Path,
        default=Path(os.environ.get("CODEX_HOME", str(Path.home() / ".codex")))
        / "auth.json",
    )
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--turns", type=int, default=5)
    parser.add_argument("--records", type=int, default=256)
    parser.add_argument(
        "--scenario", choices=["conversation", "tools", "fresh"], default="conversation"
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.live:
        parser.error(
            "live calls require --live; this benchmark is never run by make test"
        )
    if not (
        1 <= args.rounds <= 10
        and 1 <= args.turns <= 20
        and args.turns <= args.records <= 2048
    ):
        parser.error("rounds 1..10, turns 1..20, records turns..2048")
    auth = json.loads(args.auth_file.read_text()).get("tokens", {})
    if not auth.get("access_token") or not auth.get("account_id"):
        parser.error(
            "a current ChatGPT OAuth login is required; API-key mode is not used"
        )
    clients = []
    if args.client in ["all", "tny"]:
        if args.baseline:
            clients.append(("baseline", str(Path(args.baseline).resolve())))
        clients.append(("tny", str(Path(args.tny).resolve())))
    if args.client in ["all", "codex"]:
        binary = shutil.which(args.codex)
        if not binary:
            parser.error("Codex CLI not found")
        clients.append(("codex", binary))
    report = {
        "schema_version": 1,
        "started_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "model": args.model,
        "effort": "low",
        "scenario": args.scenario,
        "records": args.records,
        "turns_per_round": args.turns,
        "transport": "HTTP/SSE via loopback forwarding",
        "tool_profile": "terminal",
        "tny_cache_scope": args.cache_scope or "binary default",
        "runs": [],
    }
    report["binaries"] = {
        client: {
            "sha256": hashlib.sha256(Path(binary).read_bytes()).hexdigest(),
            "version": subprocess.run(
                [binary, "--version"],
                capture_output=True,
                text=True,
                check=True,
                timeout=10,
            ).stdout.strip(),
        }
        for client, binary in clients
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for round_index in range(args.rounds):
        # Rotate order to avoid always placing one client at the same point
        # in time. Each run has a fresh synthetic workspace and conversation.
        offset = round_index % len(clients)
        for client, binary in clients[offset:] + clients[:offset]:
            run = run_round(args, client, binary, auth, round_index)
            report["runs"].append(run)
            args.output.write_text(json.dumps(report, indent=2) + "\n")
            print(
                json.dumps(
                    {
                        "client": client,
                        "round": round_index,
                        "usage": summarize(run["requests"]),
                        "valid": len(run["turns"]) == args.turns
                        and all(turn["valid"] for turn in run["turns"]),
                    }
                ),
                flush=True,
            )
    for client, _binary in clients:
        rows = [
            row
            for run in report["runs"]
            if run["client"] == client
            for row in run["requests"]
        ]
        print(
            json.dumps(
                {
                    "client": client,
                    "all_requests": summarize(rows),
                    "after_first_turn": summarize(
                        [row for row in rows if row["turn"] > 0]
                    ),
                }
            ),
            flush=True,
        )
    return (
        0
        if all(
            len(run["turns"]) == args.turns
            and all(turn["valid"] for turn in run["turns"])
            for run in report["runs"]
        )
        else 1
    )


if __name__ == "__main__":
    raise SystemExit(main())
