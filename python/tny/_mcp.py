"""Minimal MCP client for ``tny.proxy``: stdio and Streamable HTTP.

Only what a proxy needs: initialize, tools/list (paginated) and tools/call.
Server-initiated requests get an empty result or "method not found", so a
server that pings or asks for roots does not stall.
"""

from __future__ import annotations

import json
import os
import select
import subprocess
import sys
import time
import urllib.error
import urllib.request
from typing import Any

PROTOCOL_VERSION = "2025-06-18"
CLIENT_INFO = {"name": "tny-tool-proxy", "version": "1"}


class MCPError(Exception):
    pass


class _Base:
    timeout: float
    next_id: int

    def request(self, method: str, params: dict[str, Any] | None = None) -> Any:
        raise NotImplementedError

    def notify(self, method: str) -> None:
        raise NotImplementedError

    def initialize(self) -> None:
        self.request(
            "initialize",
            {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": CLIENT_INFO,
            },
        )
        self.notify("notifications/initialized")

    def list_tools(self) -> list[dict[str, Any]]:
        tools: list[dict[str, Any]] = []
        cursor = None
        for _ in range(64):
            result = self.request("tools/list", {"cursor": cursor} if cursor else {})
            tools.extend(
                t for t in (result or {}).get("tools", []) if isinstance(t, dict)
            )
            cursor = (result or {}).get("nextCursor")
            if not cursor:
                break
        return tools

    def call_tool(self, name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        result = self.request("tools/call", {"name": name, "arguments": arguments})
        if not isinstance(result, dict):
            raise MCPError("tools/call returned no result object")
        return result

    def close(self) -> None:
        pass


def _reply_to_server(msg: dict[str, Any]) -> dict[str, Any] | None:
    """Response for a server-to-client request, or None for a notification."""
    if "id" not in msg or "method" not in msg:
        return None
    if msg["method"] == "ping":
        return {"jsonrpc": "2.0", "id": msg["id"], "result": {}}
    if msg["method"] == "roots/list":
        return {"jsonrpc": "2.0", "id": msg["id"], "result": {"roots": []}}
    return {
        "jsonrpc": "2.0",
        "id": msg["id"],
        "error": {
            "code": -32601,
            "message": f"{msg['method']} is not supported by tny proxies",
        },
    }


class StdioClient(_Base):
    def __init__(
        self,
        command: list[str],
        env: dict[str, str] | None,
        cwd: str | None,
        timeout: float,
    ) -> None:
        self.timeout = timeout
        self.next_id = 0
        child_env = dict(os.environ)
        child_env.update(env or {})
        try:
            self.proc = subprocess.Popen(
                command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=sys.stderr.fileno() if hasattr(sys.stderr, "fileno") else None,
                env=child_env,
                cwd=cwd,
            )
        except OSError as exc:
            raise MCPError(
                f"cannot start {command[0]}: {exc.strerror or exc}"
            ) from None
        self.buffer = b""

    def _send(self, msg: dict[str, Any]) -> None:
        assert self.proc.stdin is not None
        try:
            self.proc.stdin.write(
                json.dumps(msg, separators=(",", ":")).encode() + b"\n"
            )
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            raise MCPError("the MCP server exited") from None

    def _read_line(self, deadline: float) -> bytes:
        assert self.proc.stdout is not None
        fd = self.proc.stdout.fileno()
        while b"\n" not in self.buffer:
            left = deadline - time.monotonic()
            if left <= 0:
                raise MCPError(
                    f"the MCP server did not answer within {self.timeout:g}s"
                )
            ready, _, _ = select.select([fd], [], [], left)
            if not ready:
                continue
            chunk = os.read(fd, 65536)
            if not chunk:
                raise MCPError("the MCP server closed its output")
            self.buffer += chunk
        line, _, self.buffer = self.buffer.partition(b"\n")
        return line

    def request(self, method: str, params: dict[str, Any] | None = None) -> Any:
        self.next_id += 1
        rid = self.next_id
        self._send(
            {"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}}
        )
        deadline = time.monotonic() + self.timeout
        while True:
            line = self._read_line(deadline).strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            if not isinstance(msg, dict):
                continue
            if msg.get("id") == rid and "method" not in msg:
                if "error" in msg:
                    err = msg["error"] or {}
                    raise MCPError(str(err.get("message", "error")))
                return msg.get("result")
            reply = _reply_to_server(msg)
            if reply:
                self._send(reply)

    def notify(self, method: str) -> None:
        self._send({"jsonrpc": "2.0", "method": method})

    def close(self) -> None:
        try:
            if self.proc.stdin:
                self.proc.stdin.close()
            self.proc.wait(timeout=2)
            if self.proc.stdout:
                self.proc.stdout.close()
        except Exception:
            self.proc.kill()


class HttpClient(_Base):
    def __init__(
        self, url: str, headers: dict[str, str] | None, timeout: float
    ) -> None:
        self.url = url
        self.headers = dict(headers or {})
        self.timeout = timeout
        self.next_id = 0
        self.session: str | None = None

    def _post(self, msg: dict[str, Any]) -> Any:
        body = json.dumps(msg).encode()
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
            "MCP-Protocol-Version": PROTOCOL_VERSION,
            **self.headers,
        }
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        req = urllib.request.Request(
            self.url, data=body, headers=headers, method="POST"
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                session = resp.headers.get("Mcp-Session-Id")
                if session:
                    self.session = session
                kind = resp.headers.get("Content-Type", "")
                raw = resp.read()
        except urllib.error.HTTPError as exc:
            raise MCPError(f"HTTP {exc.code} from the MCP server") from None
        except (urllib.error.URLError, OSError) as exc:
            raise MCPError(
                f"cannot reach the MCP server: {getattr(exc, 'reason', exc)}"
            ) from None
        if "id" not in msg:
            return None
        messages: list[Any] = []
        if kind.startswith("text/event-stream"):
            data: list[str] = []
            for line in raw.decode("utf-8", "replace").splitlines() + [""]:
                if line.startswith("data:"):
                    data.append(line[5:].lstrip())
                elif not line and data:
                    try:
                        messages.append(json.loads("\n".join(data)))
                    except ValueError:
                        pass
                    data = []
        elif raw.strip():
            try:
                parsed = json.loads(raw)
            except ValueError:
                raise MCPError("the MCP server sent invalid JSON") from None
            messages = parsed if isinstance(parsed, list) else [parsed]
        for m in messages:
            if isinstance(m, dict) and m.get("id") == msg["id"] and "method" not in m:
                if "error" in m:
                    raise MCPError(str((m["error"] or {}).get("message", "error")))
                return m.get("result")
        raise MCPError(f"no response to {msg.get('method')}")

    def request(self, method: str, params: dict[str, Any] | None = None) -> Any:
        self.next_id += 1
        return self._post(
            {
                "jsonrpc": "2.0",
                "id": self.next_id,
                "method": method,
                "params": params or {},
            }
        )

    def notify(self, method: str) -> None:
        try:
            self._post({"jsonrpc": "2.0", "method": method})
        except MCPError:
            pass


def connect(proxy: Any) -> _Base:
    if proxy.command:
        client: _Base = StdioClient(proxy.command, proxy.env, proxy.cwd, proxy.timeout)
    else:
        client = HttpClient(proxy.url, proxy.headers, proxy.timeout)
    try:
        client.initialize()
    except Exception:
        client.close()
        raise
    return client


def result_text(result: dict[str, Any]) -> str:
    """Flatten MCP content into the text a tny tool result carries."""
    parts = []
    for item in result.get("content") or []:
        if not isinstance(item, dict):
            continue
        if item.get("type") == "text":
            parts.append(str(item.get("text", "")))
        elif item.get("type") == "resource":
            res = item.get("resource") or {}
            parts.append(str(res.get("text") or res.get("uri") or ""))
        elif item.get("type") in ("image", "audio"):
            parts.append(f"[{item['type']} {item.get('mimeType', '')} omitted]")
        elif item.get("type") == "resource_link":
            parts.append(str(item.get("uri", "")))
    if not parts and result.get("structuredContent") is not None:
        parts.append(json.dumps(result["structuredContent"], ensure_ascii=False))
    return "\n".join(parts)
