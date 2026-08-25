#!/usr/bin/env python3
"""Deterministic JSONL host fixture for tests/test_extensions.c."""

import json
import os
import sys
import time


def extension_name(path: str) -> str:
    if os.path.basename(path) == "index.py":
        return os.path.basename(os.path.dirname(path))
    return os.path.splitext(os.path.basename(path))[0]


for raw in sys.stdin:
    request = json.loads(raw)
    request_id = request["id"]
    if request.get("op") == "initialize":
        names = [extension_name(path) for path in request.get("entries", [])]
        subscriptions = []
        if names:
            subscriptions.extend(
                [
                    {
                        "event": "tool_end",
                        "handler_id": "context",
                        "extension": names[0],
                    },
                    {
                        "event": "status",
                        "handler_id": "failure",
                        "extension": names[0],
                    },
                    {
                        "event": "thinking",
                        "handler_id": "hang",
                        "extension": names[0],
                    },
                    {
                        "event": "turn_end",
                        "handler_id": "stop",
                        "extension": names[0],
                    },
                    {
                        "event": "plan",
                        "handler_id": "invalid",
                        "extension": names[0],
                    },
                ]
            )
        if len(names) > 1:
            subscriptions.append(
                {
                    "event": "tool_end",
                    "handler_id": "continue",
                    "extension": names[1],
                }
            )
        response = {
            "id": request_id,
            "ok": True,
            "protocol": 1,
            "subscriptions": subscriptions,
        }
    elif request.get("op") == "invoke":
        handler = request.get("handler_id")
        if handler == "context":
            response = {
                "id": request_id,
                "ok": True,
                "action": {
                    "type": "context",
                    "content": "visible context",
                    "custom_type": "fixture.context",
                    "display": True,
                },
            }
        elif handler == "continue":
            response = {
                "id": request_id,
                "ok": True,
                "actions": [
                    {
                        "type": "continue",
                        "content": "please iterate",
                        "message_kind": "custom",
                        "custom_type": "fixture.continue",
                        "display": False,
                    }
                ],
            }
        elif handler == "stop":
            response = {
                "id": request_id,
                "ok": True,
                "action": {"type": "stop", "reason": "fixture stop"},
            }
        elif handler == "failure":
            response = {
                "id": request_id,
                "ok": False,
                "failure": {"code": "handler_exception", "message": "fixture failure"},
            }
        elif handler == "hang":
            time.sleep(1.0)
            response = {"id": request_id, "ok": True, "action": {"type": "none"}}
        elif handler == "invalid":
            response = {
                "id": request_id,
                "ok": True,
                "action": {"kind": "teleport", "content": "somewhere"},
            }
        else:
            response = {
                "id": request_id,
                "ok": False,
                "failure": {"code": "unknown_handler", "message": "unknown handler"},
            }
    else:
        response = {"id": request_id, "ok": False, "error": "unknown operation"}
    sys.stdout.write(json.dumps(response, separators=(",", ":")) + "\n")
    sys.stdout.flush()
