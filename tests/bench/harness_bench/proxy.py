"""Per-run recording proxy for the ChatGPT Responses endpoint.

Only loopback clients can connect. OAuth material is read for each request and
never written to the recording directory. Request bodies are synthetic task
data and are retained locally for later token accounting.
"""

import gzip
import json
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

UPSTREAM = "https://chatgpt.com/backend-api/codex/responses"
MODELS_UPSTREAM = "https://chatgpt.com/backend-api/codex/models"
PATHS = {
    "/v1/responses",
    "/responses",
    "/codex/responses",
    "/backend-api/codex/responses",
}
ROUTING_HEADERS = {"session-id", "thread-id", "originator", "version"}
TERMINAL_EVENTS = {"response.completed", "response.incomplete", "response.failed"}
TOOL_TYPES = {
    "function_call",
    "custom_tool_call",
    "local_shell_call",
    "web_search_call",
}


def _chars(value):
    if isinstance(value, str):
        return len(value)
    return len(json.dumps(value, ensure_ascii=False, separators=(",", ":")))


def static_parts(body):
    """Return instruction fragments and tool definitions across wire layouts."""
    instructions = [body["instructions"]] if body.get("instructions") else []
    tools = list(body.get("tools") or [])
    items = body.get("input") or []
    if not isinstance(items, list):
        items = [items]
    for item in items:
        if not isinstance(item, dict) or item.get("role") not in {
            "developer",
            "system",
        }:
            continue
        if item.get("type") == "additional_tools":
            for group in item.get("tools") or []:
                tools.extend(group.get("tools") or [group])
        elif item.get("type", "message") == "message":
            instructions.append(item.get("content", ""))
    return instructions, tools


def sections(body):
    """Report JSON character counts with tool outputs separated from history."""
    instructions, tools = static_parts(body)
    items = body.get("input") or []
    if not isinstance(items, list):
        items = [items]
    by_type = {}
    outputs = []
    history = 0
    for item in items:
        item_type = item.get("type", "message") if isinstance(item, dict) else "text"
        by_type[item_type] = by_type.get(item_type, 0) + 1
        if isinstance(item, dict) and item.get("role") in {"developer", "system"}:
            continue
        if item_type in {"function_call_output", "custom_tool_call_output"}:
            outputs.append(_chars(item.get("output", "")))
        else:
            history += _chars(item)
    per_tool = [
        {"name": tool.get("name", tool.get("type", "unknown")), "chars": _chars(tool)}
        for tool in tools
    ]
    return {
        "instructions_chars": sum(_chars(value) for value in instructions),
        "tools_count": len(tools),
        "tools_chars": sum(tool["chars"] for tool in per_tool),
        "per_tool": per_tool,
        "input_items_count": len(items),
        "input_items_by_type": by_type,
        "tool_output_chars": outputs,
        "history_chars": history,
    }


class _Server(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, _request, _client_address):
        # Some model-metadata probes disconnect while receiving a 404. Their
        # socket tracebacks contain no useful benchmark information.
        pass

    def __init__(self, output_dir, auth_file):
        super().__init__(("127.0.0.1", 0), _Handler)
        self.output_dir = Path(output_dir)
        self.auth_file = Path(auth_file)
        self.rows = []
        self.lock = threading.Lock()

    def save(self, raw, row):
        with self.lock:
            index = len(self.rows) + 1
            name = f"request-{index:04d}.json.gz"
            (self.output_dir / name).write_bytes(gzip.compress(raw))
            row["body_file"] = name
            self.rows.append(row)
            with (self.output_dir / "requests.jsonl").open("a") as stream:
                stream.write(json.dumps(row, ensure_ascii=False) + "\n")


class _Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def _error(self, status):
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _reject(self, status):
        with self.server.lock:
            with (self.server.output_dir / "rejected.jsonl").open("a") as stream:
                stream.write(
                    json.dumps(
                        {
                            "method": self.command,
                            "path": urllib.parse.urlsplit(self.path).path,
                            "status": status,
                        }
                    )
                    + "\n"
                )
        self._error(status)

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        if parsed.path != "/backend-api/codex/models":
            self._reject(404)
            return
        try:
            auth = json.loads(self.server.auth_file.read_text())["tokens"]
            url = MODELS_UPSTREAM
            if parsed.query:
                url += "?" + parsed.query
            request = urllib.request.Request(
                url,
                headers={
                    "Authorization": "Bearer " + auth["access_token"],
                    "chatgpt-account-id": auth["account_id"],
                    "Accept": "application/json",
                },
            )
            with urllib.request.urlopen(request, timeout=30) as response:
                body = response.read(4 * 1024 * 1024 + 1)
                if len(body) > 4 * 1024 * 1024:
                    self._error(502)
                    return
                self.send_response(response.status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        except urllib.error.HTTPError as error:
            self._error(error.code)
            error.close()
        except (OSError, KeyError, ValueError):
            self._error(502)

    def do_POST(self):
        if self.path not in PATHS:
            self._reject(404)
            return
        try:
            length = int(self.headers.get("Content-Length", ""))
            if not 0 < length <= 64 * 1024 * 1024:
                raise ValueError("invalid length")
            raw = self.rfile.read(length)
            if len(raw) != length:
                raise ValueError("short body")
            encoding = self.headers.get("Content-Encoding", "identity").lower()
            if encoding == "gzip":
                raw = gzip.decompress(raw)
            elif encoding == "zstd":
                try:
                    import zstandard  # noqa: PLC0415
                except ImportError:
                    print(
                        "proxy: zstd request requires the optional zstandard module",
                        flush=True,
                    )
                    self._error(415)
                    return
                raw = zstandard.ZstdDecompressor().decompress(raw)
            elif encoding != "identity":
                self._error(415)
                return
            body = json.loads(raw)
        except (ValueError, OSError, json.JSONDecodeError):
            self._error(400)
            return

        row = {
            "request_bytes": len(raw),
            "http_status": None,
            "first_event_ms": None,
            "input_tokens": None,
            "cached_input_tokens": None,
            "cache_write_tokens": None,
            "output_tokens": None,
            "reasoning_tokens": None,
            "output_items": [],
            "sections": sections(body),
        }
        started = time.monotonic()
        try:
            auth = json.loads(self.server.auth_file.read_text())["tokens"]
            headers = {
                "Authorization": "Bearer " + auth["access_token"],
                "chatgpt-account-id": auth["account_id"],
                "Content-Type": "application/json",
                "Accept": "text/event-stream",
                "OpenAI-Beta": "responses=v1",
            }
            for key, value in self.headers.items():
                if key.lower() in ROUTING_HEADERS or key.lower().startswith("x-codex-"):
                    headers[key] = value
            request = urllib.request.Request(UPSTREAM, data=raw, headers=headers)
            try:
                response = urllib.request.urlopen(request, timeout=180)
            except urllib.error.HTTPError as error:
                row["http_status"] = error.code
                self._error(error.code)
                error.close()
                return
            with response:
                row["http_status"] = response.status
                self.send_response(response.status)
                self.send_header(
                    "Content-Type",
                    response.headers.get("Content-Type", "text/event-stream"),
                )
                self.send_header("Connection", "close")
                self.close_connection = True
                affinity = response.headers.get("x-codex-turn-state")
                if affinity:
                    self.send_header("x-codex-turn-state", affinity)
                self.end_headers()
                event_lines = []
                output_items = {}
                for line in response:
                    # Preserve the upstream stream byte for byte, including comments and blank lines.
                    try:
                        self.wfile.write(line)
                        self.wfile.flush()
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                    if line.startswith(b"data:"):
                        event_lines.append(line[5:].strip())
                    elif not line.strip() and event_lines:
                        payload = b"\n".join(event_lines)
                        event_lines.clear()
                        if payload == b"[DONE]":
                            continue
                        try:
                            event = json.loads(payload)
                        except ValueError:
                            continue
                        if row["first_event_ms"] is None:
                            row["first_event_ms"] = round(
                                (time.monotonic() - started) * 1000, 3
                            )
                        if event.get("type") in {
                            "response.output_item.added",
                            "response.output_item.done",
                        }:
                            item = event.get("item") or {}
                            if item.get("type") in TOOL_TYPES:
                                key = (
                                    item.get("id")
                                    or item.get("call_id")
                                    or event.get("output_index")
                                )
                                output_items[key] = {
                                    "type": item["type"],
                                    "name": item.get("name"),
                                }
                        if event.get("type") in TERMINAL_EVENTS:
                            row["completion"] = event["type"]
                            result = event.get("response") or {}
                            usage = result.get("usage") or {}
                            details = usage.get("input_tokens_details") or {}
                            output_details = usage.get("output_tokens_details") or {}
                            row.update(
                                input_tokens=usage.get("input_tokens"),
                                cached_input_tokens=details.get("cached_tokens"),
                                cache_write_tokens=details.get("cache_write_tokens", 0),
                                cache_write_reported="cache_write_tokens" in details,
                                output_tokens=usage.get("output_tokens"),
                                reasoning_tokens=output_details.get("reasoning_tokens"),
                            )
                            completed_items = [
                                {"type": item.get("type"), "name": item.get("name")}
                                for item in result.get("output", [])
                                if item.get("type") in TOOL_TYPES
                            ]
                            row["output_items"] = completed_items or list(
                                output_items.values()
                            )
        except (OSError, KeyError, ValueError) as error:
            row["error"] = type(error).__name__
            if row["http_status"] is None:
                self._error(502)
                row["http_status"] = 502
        finally:
            row["elapsed_ms"] = round((time.monotonic() - started) * 1000, 3)
            self.server.save(raw, row)


class RecordingProxy:
    """A fresh proxy and recorder for one benchmark run."""

    def __init__(self, output_dir, auth_file):
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.server = _Server(self.output_dir, auth_file)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def base_url(self):
        return f"http://127.0.0.1:{self.server.server_port}"

    @property
    def rows(self):
        with self.server.lock:
            return list(self.server.rows)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_args):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
