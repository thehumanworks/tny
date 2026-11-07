#!/usr/bin/env python3
"""Minimal stdio MCP server for tny.proxy tests: two tools, one paginated
tools/list, and a server-initiated ping before answering tools/call."""

import json
import sys

TOOLS = [
    {
        "name": "echo",
        "description": "Echo text back.",
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
        },
    },
    {
        "name": "fail",
        "description": "Always fails.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "secret",
        "description": "Excluded by the proxy filter.",
        "inputSchema": {"type": "object", "properties": {}},
    },
]


def send(msg):
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


for line in sys.stdin:
    msg = json.loads(line)
    method, rid, params = msg.get("method"), msg.get("id"), msg.get("params") or {}
    if rid is None or method is None:
        continue
    if method == "initialize":
        send(
            {
                "jsonrpc": "2.0",
                "id": rid,
                "result": {
                    "protocolVersion": "2025-06-18",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "fake", "version": "1"},
                },
            }
        )
    elif method == "tools/list":
        page = 1 if params.get("cursor") == "p2" else 0
        result = {"tools": TOOLS[:1] if page == 0 else TOOLS[1:]}
        if page == 0:
            result["nextCursor"] = "p2"
        send({"jsonrpc": "2.0", "id": rid, "result": result})
    elif method == "tools/call":
        send({"jsonrpc": "2.0", "id": "srv-1", "method": "ping"})
        reply = json.loads(sys.stdin.readline())
        assert reply.get("id") == "srv-1" and "result" in reply, reply
        name, args = params.get("name"), params.get("arguments") or {}
        if name == "echo":
            result = {
                "content": [{"type": "text", "text": "echo:" + args.get("text", "")}]
            }
        else:
            result = {
                "content": [{"type": "text", "text": "it failed"}],
                "isError": True,
            }
        send({"jsonrpc": "2.0", "id": rid, "result": result})
    else:
        send(
            {"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": "nope"}}
        )
