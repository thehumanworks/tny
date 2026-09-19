"""CLI entry point; rpc stdout is exclusively one JSON envelope."""

import argparse
import json
import os
import sys

from .core import MAX_FILE, BoardError, handle, parse_json
from .http import make_server
from .terminal import render, safe, tui


def main(argv=None):
    parser = argparse.ArgumentParser(description="Standalone local cooperative ticket board")
    parser.add_argument(
        "--root", default=".", help="Existing project directory (no symlink components)"
    )
    parser.add_argument("--board", default="default")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("rpc", help="One JSON request on stdin, one JSON envelope on stdout")
    sub.add_parser("show", help="Print safe terminal columns")
    ui = sub.add_parser(
        "tui", help="Line-oriented inspect/move/comment/refresh; non-TTY prints once"
    )
    ui.add_argument("--actor", default="user")
    serve = sub.add_parser(
        "serve", help="Loopback HTTP; requires TNYBOARD_TOKEN environment variable"
    )
    serve.add_argument("--port", type=int, default=8765)
    for op in ("init", "create", "move", "comment", "claim", "release", "assign", "configure"):
        cmd = sub.add_parser(op)
        cmd.add_argument("--actor", default="user")
        if op not in ("init", "create"):
            cmd.add_argument("--expected-revision", type=int, required=True)
        if op in ("move", "comment", "claim", "release", "assign"):
            cmd.add_argument("id")
            cmd.add_argument("--token")
        if op in ("init", "configure"):
            cmd.add_argument("--columns", help="JSON array of {id,title,owner}")
            cmd.add_argument("--lead", help="Owner label; 'null' disables lead")
        if op == "create":
            cmd.add_argument("title")
            cmd.add_argument("--id")
            cmd.add_argument("--status")
            cmd.add_argument("--body")
            cmd.add_argument("--owner")
        if op == "move":
            cmd.add_argument("status")
            cmd.add_argument("reason")
        if op == "comment":
            cmd.add_argument("body")
        if op == "assign":
            cmd.add_argument("owner", help="Owner label; 'null' clears override")
    args = parser.parse_args(argv)
    try:
        if args.command == "serve":
            server = make_server(args.root, args.board, os.environ.get("TNYBOARD_TOKEN"), args.port)
            print(
                f"tnyboard listening on 127.0.0.1:{server.server_port}", file=sys.stderr, flush=True
            )
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                pass
            finally:
                server.server_close()
            return 0
        if args.command == "tui":
            return tui(args.root, args.board, sys.stdin, sys.stdout, args.actor)
        if args.command == "rpc":
            data = sys.stdin.buffer.read(MAX_FILE + 1)
            if len(data) > MAX_FILE:
                raise BoardError("too_large", "Request body too large")
            request = parse_json(data.decode("utf-8"))
        elif args.command == "show":
            request = {"op": "get"}
        else:
            request = {
                k: v
                for k, v in vars(args).items()
                if k not in ("root", "board", "command") and v is not None
            }
            request["op"] = args.command
            if "columns" in request:
                request["columns"] = parse_json(request["columns"])
            for key in ("lead", "owner"):
                if request.get(key) == "null":
                    request[key] = None
        result = handle(args.root, args.board, request)
    except (BoardError, UnicodeError, ValueError, OSError) as exc:
        result = {
            "ok": False,
            "error": {"code": getattr(exc, "code", "invalid_request"), "message": str(exc)},
        }
    if args.command == "show" and result["ok"]:
        print(render(result["result"]))
    elif args.command == "rpc" or args.command != "show":
        print(json.dumps(result, ensure_ascii=True))
    else:
        print(safe(json.dumps(result)), file=sys.stderr)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
