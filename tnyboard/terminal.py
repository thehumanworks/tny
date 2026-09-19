"""Control-safe text rendering and a small line-oriented terminal interface."""

import json
import shlex
import shutil
import unicodedata

from .core import handle


def safe(value):
    """Escape controls, bidi/format controls and non-ASCII to fixed-width text."""
    result = []
    for char in str(value):
        code = ord(char)
        if 32 <= code < 127:
            result.append(char)
        elif char == "\n":
            result.append(r"\n")
        elif char == "\t":
            result.append(r"\t")
        elif code < 256:
            result.append(f"\\x{code:02x}")
        elif unicodedata.category(char).startswith("C") or code >= 256:
            result.append(f"\\u{code:04x}" if code <= 65535 else f"\\U{code:08x}")
    return "".join(result)


def clip(value, width):
    value = safe(value)
    return value if len(value) <= width else value[: max(0, width - 1)] + "~"


def render(snapshot, width=100):
    """Render an API snapshot as printable ASCII columns; no terminal escapes."""
    if snapshot.get("ok") is True:
        snapshot = snapshot["result"]
    width = max(1, min(int(width), 1000))
    board = snapshot["board"]
    cols = board["columns"]
    lines = [clip(f"{board['name']} (board r{board['revision']})", width)]
    cell = (width - 3 * (len(cols) - 1)) // len(cols)
    if cell < 16:
        for col in cols:
            lines.append(clip(f"[{col['title']}] owner={col['owner'] or '-'}", width))
            for ticket in snapshot["tickets"]:
                if ticket["status"] == col["id"]:
                    lines.append(
                        clip(f"{ticket['id']} r{ticket['revision']} {ticket['title']}", width)
                    )
    else:
        rows = []
        for col in cols:
            items = [clip(col["title"], cell), clip("owner=" + (col["owner"] or "-"), cell)]
            items += [
                clip(f"{t['id']} r{t['revision']} {t['title']}", cell)
                for t in snapshot["tickets"]
                if t["status"] == col["id"]
            ]
            rows.append(items)
        for i in range(max(map(len, rows))):
            lines.append(
                " | ".join((r[i] if i < len(r) else "").ljust(cell) for r in rows).rstrip()
            )
    return "\n".join(lines)


def tui(root, board, input_stream, output_stream, actor="user"):
    """Interactive commands use fresh read revisions; conflicts are never retried."""
    width = shutil.get_terminal_size((100, 24)).columns

    def refresh():
        state = handle(root, board, {"op": "get"})
        output_stream.write(
            (render(state["result"], width) if state["ok"] else safe(json.dumps(state))) + "\n"
        )
        output_stream.flush()
        return state["ok"]

    if not refresh():
        return 1
    if not input_stream.isatty() or not output_stream.isatty():
        return 0
    output_stream.write(
        "Commands: inspect ID; move ID STATUS REASON; comment ID BODY; refresh; quit\n"
    )
    while True:
        output_stream.write("board> ")
        output_stream.flush()
        line = input_stream.readline()
        if not line:
            return 0
        try:
            args = shlex.split(line)
            if not args:
                continue
            if args == ["quit"] or args == ["exit"]:
                return 0
            if args == ["refresh"]:
                refresh()
                continue
            if args[0] not in ("inspect", "move", "comment") or len(args) < 2:
                raise ValueError("Unknown command")
            state = handle(root, board, {"op": "get", "id": args[1]})
            if state["ok"] and args[0] != "inspect":
                req = {
                    "op": args[0],
                    "id": args[1],
                    "actor": actor,
                    "expected_revision": state["result"]["revision"],
                }
                if args[0] == "move" and len(args) >= 4:
                    req.update(status=args[2], reason=" ".join(args[3:]))
                elif args[0] == "comment" and len(args) >= 3:
                    req["body"] = " ".join(args[2:])
                else:
                    raise ValueError("Missing status, reason or body")
                state = handle(root, board, req)
            output_stream.write(safe(json.dumps(state, ensure_ascii=True)) + "\n")
        except ValueError as exc:
            output_stream.write("Error: " + safe(exc) + "\n")
