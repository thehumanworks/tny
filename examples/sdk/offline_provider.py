#!/usr/bin/env python3
"""Scripted OpenAI-compatible provider for running the SDK examples offline.

This is a stand-in, not a model: it recognises each example role from the
request and streams a canned reply over the Chat Completions wire, so the
real native runtime, tools, permissions and workflow scheduler all run
without a key or network. Generators and the fixer answer with a genuine
`write_file` tool call first; the first generated `greet.py` carries a bug so
the verification/repair loop has something to repair, and the first
decomposer reply is malformed so the JSON re-ask path runs too. Set
OFFLINE_REQUEST_LOG to a file to record each request's role, model and effort.

Usage: offline_provider.py [port]        (default 8787; 0 picks a free port)
Then:  OPENAI_BASE_URL=http://127.0.0.1:<port>/v1 OPENAI_API_KEY=offline \
       OPENAI_WIRE_API=chat <example> ...
"""

from __future__ import annotations

import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def fenced(value: object) -> str:
    return "```json\n" + json.dumps(value, indent=2) + "\n```"


# A specification containing "[stubborn]" makes the fixer fail on every model
# but the plan's critical one, which exercises the examples' escalation.
CRITICAL_MODEL = json.loads(
    (Path(__file__).resolve().parent / "models.json").read_text(encoding="utf-8")
)["tiers"]["critical"]["model"]

GREET_BUGGY = 'def greet(name):\n    return "Hello " + name\n'
GREET_FIXED = 'def greet(name):\n    return f"Hello, {name}!"\n'
MAIN = (
    "import sys\n\nfrom greet import greet\n\n"
    'if __name__ == "__main__":\n'
    '    print(greet(sys.argv[1] if len(sys.argv) > 1 else "world"))\n'
)

PLAN = {
    "questions": [
        {
            "id": "layout",
            "question": "How is the workspace organised?",
            "approach": "List the top-level files and read the README.",
        },
        {
            "id": "entry",
            "question": "What is the main entry point?",
            "approach": "Search for a main function or script.",
        },
    ]
}
UNITS = {
    "units": [
        {
            "id": "core",
            "goal": "greet(name) returning 'Hello, <name>!'",
            "files": ["greet.py"],
            "depends_on": [],
            "acceptance": "greet('Ada') == 'Hello, Ada!'",
        },
        {
            "id": "cli",
            "goal": "Command line entry point printing the greeting",
            "files": ["main.py"],
            "depends_on": ["core"],
            "acceptance": "python3 main.py Ada prints Hello, Ada!",
        },
    ]
}
FINDING = (
    "## Answer\nOffline stand-in answer.\n\n## Evidence\n- README.md:1\n\n"
    "## Confidence\nlow - scripted provider\n\n## Open questions\nNone."
)
REPORT = (
    "## Summary\nOffline stand-in report.\n\n## Findings\n- See README.md:1\n\n"
    "## Contradictions and caveats\nNone.\n\n## What remains unknown\nEverything real."
)
GENERATED = (
    "## Files written\n- {path}\n\n## Public interface\nSee the file.\n\n"
    "## Checks run\nnone (offline)\n\n## Deviations from the contract\nNone."
)


ROLES = (
    ("You are the research planner", "research-planner"),
    ("You are a research investigator", "research-investigator"),
    ("You are the research synthesiser", "research-synthesiser"),
    ("You are the research critic", "research-critic"),
    ("in an automated research loop.\nGiven how a run went", "research-retro"),
    ("You are the decomposer", "codegen-decomposer"),
    ("You are the architect", "codegen-architect"),
    ("You are the fixer", "codegen-fixer"),
    ("You are a generator", "codegen-generator"),
    ("You are the retrospective analyst", "codegen-retro"),
)


def role_of(text: str, last_user: str) -> str:
    if "Review the generated" in last_user:  # built-in preset: match the task
        return "review"
    return next((role for needle, role in ROLES if needle in text), "unknown")


def reply_for(
    text: str, last_user: str, tool_done: bool, model: str
) -> str | tuple[str, str]:
    """Return reply text, or (path, content) for a write_file tool call."""
    if "Review the generated" in last_user:  # built-in preset: match the task
        return "No blocking findings (offline stand-in)."
    if "You are the research planner" in text:
        return "Two small questions.\n\n" + fenced(PLAN)
    if "You are a research investigator" in text:
        return FINDING
    if "You are the research synthesiser" in text:
        return REPORT
    if "You are the research critic" in text:
        if "Round 1 " in last_user:
            low = {
                "score": 0.55,
                "strengths": ["structured"],
                "gaps": ["Which file documents the build?"],
            }
            return "Thin evidence.\n\n" + fenced(low)
        return "Adequate.\n\n" + fenced(
            {"score": 0.9, "strengths": ["cited"], "gaps": []}
        )
    if "You are the retrospective analyst" in text:
        lesson = (
            "Pin exact return values in the architecture contract."
            if "code generation pipeline" in text
            else "Ask for file-level evidence before broad questions."
        )
        return fenced({"lessons": [lesson]})
    if "You are the decomposer" in text:
        if "could not be used" in last_user:
            return "Two units.\n\n" + fenced(UNITS)
        return "Two units: core, then cli."  # deliberately missing its JSON block
    if "You are the architect" in text:
        return (
            "## Layout\ngreet.py, main.py\n\n## Interfaces\ngreet(name: str) -> str\n\n"
            "## Conventions\nStandard library only.\n\n## Test strategy\nRun main.py."
        )
    if "You are the fixer" in text:
        if not tool_done:
            stubborn = "[stubborn]" in text and model != CRITICAL_MODEL
            return ("greet.py", GREET_BUGGY if stubborn else GREET_FIXED)
        return (
            "## Root cause\nMissing punctuation.\n\n## Change made\ngreet.py\n\n"
            "## Checks run\nnone (offline)"
        )
    if "You are a generator" in text:
        path, content = (
            ("main.py", MAIN)
            if "Unit `cli`" in last_user
            else ("greet.py", GREET_BUGGY)
        )
        return GENERATED.format(path=path) if tool_done else (path, content)
    return "Offline provider: unrecognised role."


def content_text(content: object) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "".join(
            part.get("text", "") for part in content if isinstance(part, dict)
        )
    return ""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args: object) -> None:
        pass

    def _send(self, status: int, content_type: str, body: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        self._send(200, "application/json", b'{"data":[{"id":"offline"}]}')

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length) or b"{}")
        if not self.path.endswith("/chat/completions"):
            error = {"error": {"message": "offline provider serves wire_api=chat only"}}
            self._send(400, "application/json", json.dumps(error).encode())
            return
        messages = request.get("messages", [])
        text = "\n".join(content_text(m.get("content")) for m in messages)
        users = [m for m in messages if m.get("role") == "user"]
        last_user = content_text(users[-1].get("content")) if users else ""
        tool_done = any(m.get("role") == "tool" for m in messages)
        reply = reply_for(text, last_user, tool_done, str(request.get("model")))
        # One line per request, so a caller can check which model and effort
        # each role actually put on the wire.
        if request_log := os.environ.get("OFFLINE_REQUEST_LOG"):
            entry = {
                "role": role_of(text, last_user),
                "model": request.get("model"),
                "effort": request.get("reasoning_effort"),
            }
            with open(request_log, "a", encoding="utf-8") as handle:
                handle.write(json.dumps(entry) + "\n")

        if isinstance(reply, tuple):
            arguments = json.dumps({"path": reply[0], "content": reply[1]})
            call = {
                "index": 0,
                "id": "call_offline_1",
                "type": "function",
                "function": {"name": "write_file", "arguments": arguments},
            }
            delta, finish = {"role": "assistant", "tool_calls": [call]}, "tool_calls"
        else:
            delta, finish = {"role": "assistant", "content": reply}, "stop"
        usage = {"prompt_tokens": len(text) // 4, "completion_tokens": 32}
        frames = [
            {"choices": [{"index": 0, "delta": delta, "finish_reason": None}]},
            {
                "choices": [{"index": 0, "delta": {}, "finish_reason": finish}],
                "usage": usage,
            },
        ]
        body = b"".join(f"data: {json.dumps(f)}\n\n".encode() for f in frames)
        self._send(200, "text/event-stream", body + b"data: [DONE]\n\n")


def main() -> None:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8787
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"http://127.0.0.1:{server.server_address[1]}/v1", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
