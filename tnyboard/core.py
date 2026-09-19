"""Validated JSON transactions. Public identities are cooperative labels."""

import copy
import fcntl
import json
import os
import re
import secrets
import stat
import threading
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path

MAX_FILE = 1024 * 1024
MAX_BODY = 65536
MAX_TICKETS = 10000
ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}\Z")
ACTOR = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.@-]{0,127}\Z")
_THREAD_LOCK = threading.RLock()
DEFAULT_COLUMNS = [
    {"id": "todo", "title": "To do", "owner": "worker"},
    {"id": "doing", "title": "Doing", "owner": "worker"},
    {"id": "done", "title": "Done", "owner": "worker"},
]


class BoardError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def fail(code, message):
    raise BoardError(code, message)


def require(condition, message, code="invalid_request"):
    if not condition:
        fail(code, message)


def ident(value, label="id"):
    require(isinstance(value, str) and ID.fullmatch(value), f"Invalid {label}")
    return value


def actor(value, nullable=False):
    require(
        (nullable and value is None) or (isinstance(value, str) and ACTOR.fullmatch(value)),
        "Invalid actor/owner label",
    )
    return value


def text(value, label, limit=MAX_BODY, nonempty=False):
    require(isinstance(value, str), f"{label} must be a string")
    try:
        size = len(value.encode("utf-8"))
    except UnicodeError:
        fail("invalid_request", f"Invalid Unicode in {label}")
    require(size <= limit and (not nonempty or value.strip()), f"Invalid {label} length")
    return value


def integer(value):
    return type(value) is int and value >= 1


def unique_object(pairs):
    obj = {}
    for key, value in pairs:
        require(key not in obj, f"Duplicate JSON key: {key}")
        obj[key] = value
    return obj


def parse_json(data):
    try:
        return json.loads(
            data,
            object_pairs_hook=unique_object,
            parse_constant=lambda _: fail("invalid_request", "Non-finite JSON number"),
        )
    except (ValueError, UnicodeError, RecursionError) as exc:
        fail("invalid_request", f"Invalid JSON: {type(exc).__name__}")


def encode(value):
    try:
        data = (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode(
            "utf-8"
        )
    except (ValueError, TypeError, UnicodeError, RecursionError):
        fail("invalid_request", "Not a JSON value")
    require(len(data) <= MAX_FILE, "Document too large", "too_large")
    return data


def fields(obj, required, optional=()):
    require(type(obj) is dict, "Expected an object")
    require(
        set(required) <= obj.keys(),
        "Missing required fields: " + ", ".join(sorted(set(required) - obj.keys())),
    )
    require(obj.keys() <= set(required) | set(optional), "Unknown fields")


def columns(value):
    require(type(value) is list and 1 <= len(value) <= 32, "Expected 1..32 columns")
    seen = set()
    for col in value:
        fields(col, ("id", "title", "owner"))
        ident(col["id"], "column id")
        text(col["title"], "column title", 256, True)
        actor(col["owner"], True)
        require(col["id"] not in seen, "Duplicate column id")
        seen.add(col["id"])
    return value


def validate_board(value, name):
    fields(value, ("schema_version", "name", "revision", "columns", "lead"))
    require(
        type(value["schema_version"]) is int and value["schema_version"] == 1,
        "Unknown schema version",
    )
    require(value["name"] == name and integer(value["revision"]), "Invalid board identity/revision")
    columns(value["columns"])
    actor(value["lead"], True)
    return value


def validate_dispatch(value):
    if value is None:
        return
    fields(
        value,
        ("intent_id", "state", "actor", "owner", "created_at", "updated_at", "job_id", "detail"),
    )
    ident(value["intent_id"], "intent_id")
    require(
        value["state"] in ("pending", "confirmed", "failed", "uncertain"), "Invalid dispatch state"
    )
    actor(value["actor"])
    actor(value["owner"], True)
    for key in ("created_at", "updated_at"):
        text(value[key], key, 64, True)
    text(value["detail"], "detail", 8192)
    if value["job_id"] is not None:
        text(value["job_id"], "job_id", 256, True)
    require(
        value["state"] != "confirmed" or value["job_id"] is not None,
        "Confirmed dispatch requires job_id",
    )


def validate_ticket(value, ticket_id, board):
    fields(
        value,
        (
            "schema_version",
            "id",
            "revision",
            "status",
            "title",
            "body",
            "owner",
            "comments",
            "history",
            "claim",
            "dispatch",
        ),
    )
    require(
        type(value["schema_version"]) is int and value["schema_version"] == 1,
        "Unknown schema version",
    )
    ident(value["id"])
    require(
        value["id"] == ticket_id and integer(value["revision"]), "Invalid ticket identity/revision"
    )
    require(value["status"] in [c["id"] for c in board["columns"]], "Unknown stored status")
    text(value["title"], "title", 1024, True)
    text(value["body"], "body")
    actor(value["owner"], True)
    require(type(value["comments"]) is list and type(value["history"]) is list, "Invalid events")
    for comment in value["comments"]:
        fields(comment, ("actor", "body", "at", "revision"))
        actor(comment["actor"])
        text(comment["body"], "comment", MAX_BODY, True)
        text(comment["at"], "at", 64, True)
        require(
            integer(comment["revision"]) and comment["revision"] <= value["revision"],
            "Invalid comment revision",
        )
    require(len(value["history"]) == value["revision"], "Invalid history length")
    intents = set()
    for rev, event in enumerate(value["history"], 1):
        fields(event, ("op", "actor", "at", "revision", "detail"))
        actor(event["actor"])
        require(
            event["revision"] == rev and type(event["revision"]) is int, "Invalid history revision"
        )
        require(event["op"] in TICKET_OPS | {"create"}, "Invalid history operation")
        text(event["at"], "at", 64, True)
        require(type(event["detail"]) is dict, "Invalid history detail")
        require((rev == 1) == (event["op"] == "create"), "Invalid create history")
        detail = event["detail"]
        if event["op"] == "move":
            fields(detail, ("from", "to", "reason"))
            ident(detail["from"], "historical status")
            ident(detail["to"], "historical status")
            text(detail["reason"], "reason", 8192, True)
        elif event["op"] == "assign":
            fields(detail, ("from", "to"))
            actor(detail["from"], True)
            actor(detail["to"], True)
        elif event["op"] == "dispatch-result":
            validate_dispatch(detail)
            require(
                detail["state"] != "pending" and detail["intent_id"] in intents,
                "Invalid result history",
            )
        elif event["op"] != "dispatch-intent":
            fields(detail, ())
        if event["op"] == "dispatch-intent":
            fields(detail, ("intent_id",))
            intent = ident(event["detail"].get("intent_id"), "intent_id")
            require(intent not in intents, "Duplicate dispatch intent")
            intents.add(intent)
    comment_events = [e for e in value["history"] if e["op"] == "comment"]
    require(len(comment_events) == len(value["comments"]), "Comment/history mismatch")
    for comment, entry in zip(value["comments"], comment_events):
        require(
            comment["revision"] == entry["revision"] and comment["actor"] == entry["actor"],
            "Comment/history mismatch",
        )
    claim = value["claim"]
    if claim is not None:
        fields(claim, ("actor", "token"))
        actor(claim["actor"])
        text(claim["token"], "token", 256, True)
    validate_dispatch(value["dispatch"])
    require(bool(intents) == (value["dispatch"] is not None), "Dispatch/history mismatch")
    if value["dispatch"] is not None:
        latest = next(e for e in reversed(value["history"]) if e["op"].startswith("dispatch-"))
        require(
            value["dispatch"]["intent_id"] == latest["detail"]["intent_id"],
            "Dispatch/history mismatch",
        )
        if latest["op"] == "dispatch-result":
            require(value["dispatch"] == latest["detail"], "Dispatch result/history mismatch")
        else:
            require(value["dispatch"]["state"] == "pending", "Dispatch intent is not pending")
    return value


def directory(path, create=False):
    if create:
        try:
            os.mkdir(path, 0o700)
            parent_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(parent_fd)
            finally:
                os.close(parent_fd)
        except FileExistsError:
            pass
    try:
        mode = os.lstat(path).st_mode
    except FileNotFoundError:
        fail("not_found", "Board directory does not exist")
    require(stat.S_ISDIR(mode), "Symlink or non-directory in board path", "unsafe_path")


def safe_root(root):
    path = Path(os.path.abspath(os.fspath(root)))
    # Reject symlinks in every component; do not silently resolve the requested root.
    for part in reversed((path, *path.parents)):
        directory(part)
    return path


@contextmanager
def transaction(root, name, create=False):
    ident(name, "board name")
    with _THREAD_LOCK:
        path = safe_root(root)
        for component in (".tnyboard", "boards", name):
            path /= component
            directory(path, create)
        lock = path / ".lock"
        try:
            fd = os.open(lock, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
        except OSError:
            fail("unsafe_path", "Cannot open board lock")
        try:
            require(
                stat.S_ISREG(os.fstat(fd).st_mode) and os.fstat(fd).st_nlink == 1,
                "Unsafe board lock",
                "unsafe_path",
            )
            fcntl.flock(fd, fcntl.LOCK_EX)
            directory(path / "tickets", create)
            yield path
        finally:
            os.close(fd)


def read(path):
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except FileNotFoundError:
        fail("not_found", f"Missing {path.name}")
    except OSError:
        fail("unsafe_path", f"Unsafe file: {path.name}")
    try:
        info = os.fstat(fd)
        require(
            stat.S_ISREG(info.st_mode) and info.st_nlink == 1, "Unsafe stored file", "unsafe_path"
        )
        require(info.st_size <= MAX_FILE, "Stored file too large", "corrupt_state")
        with os.fdopen(fd, "rb", closefd=False) as stream:
            data = stream.read(MAX_FILE + 1)
        require(len(data) <= MAX_FILE, "Stored file too large", "corrupt_state")
        try:
            return parse_json(data.decode("utf-8"))
        except UnicodeError:
            fail("corrupt_state", "Stored JSON must be UTF-8")
    finally:
        os.close(fd)


def write(path, value):
    data = encode(value)
    if os.path.lexists(path):
        info = os.lstat(path)
        require(
            stat.S_ISREG(info.st_mode) and info.st_nlink == 1, "Unsafe destination", "unsafe_path"
        )
    temp = path.with_name(".tmp-" + secrets.token_hex(16))
    fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, path)
        dirfd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(dirfd)
        finally:
            os.close(dirfd)
    finally:
        if temp.exists():
            temp.unlink()


def snapshot(path, name):
    try:
        board = validate_board(read(path / "board.json"), name)
        tickets = []
        for entry in sorted((path / "tickets").iterdir()):
            if entry.name.startswith(".tmp-"):
                continue
            require(entry.suffix == ".json", "Unexpected file in tickets", "corrupt_state")
            ident(entry.stem)
            require(len(tickets) < MAX_TICKETS, "Too many tickets", "corrupt_state")
            tickets.append(validate_ticket(read(entry), entry.stem, board))
        intents = [
            e["detail"]["intent_id"]
            for t in tickets
            for e in t["history"]
            if e["op"] == "dispatch-intent"
        ]
        require(len(intents) == len(set(intents)), "Duplicate board intent ID")
        return {"board": board, "tickets": tickets}
    except BoardError as exc:
        if exc.code == "invalid_request":
            fail("corrupt_state", str(exc))
        raise


TICKET_OPS = {"move", "comment", "claim", "release", "assign", "dispatch-intent", "dispatch-result"}
OPTIONAL = {
    "init": {"columns", "lead"},
    "get": {"id"},
    "list": set(),
    "create": {"id", "status", "body", "owner"},
    "configure": {"columns", "lead"},
    "move": {"token"},
    "comment": {"token"},
    "claim": {"token"},
    "release": {"token"},
    "assign": {"token"},
    "dispatch-intent": {"token"},
    "dispatch-result": {"token", "job_id", "detail"},
}
REQUIRED = {
    "create": {"title"},
    "move": {"status", "reason"},
    "comment": {"body"},
    "assign": {"owner"},
    "dispatch-intent": {"intent_id"},
    "dispatch-result": {"intent_id", "outcome"},
}


def revision(obj, req):
    require(integer(req["expected_revision"]), "expected_revision must be a positive integer")
    require(req["expected_revision"] == obj["revision"], "Revision conflict", "revision_conflict")


def privileged(board, who):
    return who == "user" or who == board["lead"]


def effective_owner(board, ticket):
    if ticket["owner"] is not None:
        return ticket["owner"]
    return next(c["owner"] for c in board["columns"] if c["id"] == ticket["status"])


def authorize(board, ticket, req, ownership=True):
    who = req["actor"]
    if privileged(board, who):
        return
    if ownership:
        require(who == effective_owner(board, ticket), "Actor is not owner or lead", "forbidden")
    claim = ticket["claim"]
    if claim is not None:
        require(
            who == claim["actor"] and req.get("token") == claim["token"],
            "Current claim token required",
            "claim_conflict",
        )


def now():
    return datetime.now(timezone.utc).isoformat()


def event(ticket, op, who, detail):
    ticket["history"].append(
        {"op": op, "actor": who, "at": now(), "revision": ticket["revision"], "detail": detail}
    )


def execute(root, name, req):
    require(type(req) is dict, "Request must be an object")
    encode(req)
    op = req.get("op")
    require(isinstance(op, str) and op in OPTIONAL, "Unknown operation")
    required = {"op"} | REQUIRED.get(op, set())
    if op not in ("get", "list"):
        required.add("actor")
    if op in TICKET_OPS | {"configure"}:
        required.add("expected_revision")
    if op in TICKET_OPS:
        required.add("id")
    fields(req, required, OPTIONAL[op])
    if "actor" in req:
        actor(req["actor"])
    if "id" in req:
        ident(req["id"])
    if "token" in req:
        text(req["token"], "token", 256, True)
    with transaction(root, name, op == "init") as path:
        if op == "init":
            require(
                not os.path.lexists(path / "board.json"), "Board already exists", "already_exists"
            )
            require(
                not any((path / "tickets").iterdir()), "Orphaned tickets exist", "corrupt_state"
            )
            board = {
                "schema_version": 1,
                "name": name,
                "revision": 1,
                "columns": copy.deepcopy(req.get("columns", DEFAULT_COLUMNS)),
                "lead": req.get("lead", "board-lead"),
            }
            validate_board(board, name)
            require(
                privileged(board, req["actor"]), "Only lead or user may initialize", "forbidden"
            )
            # Runtime artifacts are not Git content. Never overwrite an existing ignore file.
            ignore = path / ".gitignore"
            if not os.path.lexists(ignore):
                fd = os.open(ignore, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
                with os.fdopen(fd, "w", encoding="utf-8") as stream:
                    stream.write(".lock\n.tmp-*\ntickets/.tmp-*\n")
                    stream.flush()
                    os.fsync(stream.fileno())
            write(path / "board.json", board)
            return {"board": board, "tickets": []}
        state = snapshot(path, name)
        board, tickets = state["board"], state["tickets"]
        if op == "list" or (op == "get" and "id" not in req):
            return state
        if op == "configure":
            revision(board, req)
            require(privileged(board, req["actor"]), "Only lead or user may configure", "forbidden")
            require("columns" in req or "lead" in req, "No configuration changes")
            for key in ("columns", "lead"):
                if key in req:
                    board[key] = copy.deepcopy(req[key])
            board["revision"] += 1
            validate_board(board, name)
            require(
                all(t["status"] in [c["id"] for c in board["columns"]] for t in tickets),
                "Cannot remove an occupied column",
            )
            require(
                not any(
                    t["claim"] is not None
                    or (t["dispatch"] and t["dispatch"]["state"] in ("pending", "uncertain"))
                    for t in tickets
                ),
                "Release claims and reconcile dispatches before configuring",
                "claim_conflict",
            )
            write(path / "board.json", board)
            return board
        if op == "create":
            require(len(tickets) < MAX_TICKETS, "Ticket limit reached", "too_large")
            ticket_id = req.get("id", "t-" + secrets.token_hex(8))
            require(
                not os.path.lexists(path / "tickets" / (ticket_id + ".json")),
                "Ticket already exists",
                "already_exists",
            )
            ticket = {
                "schema_version": 1,
                "id": ticket_id,
                "revision": 1,
                "status": req.get("status", board["columns"][0]["id"]),
                "title": req["title"],
                "body": req.get("body", ""),
                "owner": req.get("owner"),
                "comments": [],
                "history": [],
                "claim": None,
                "dispatch": None,
            }
            event(ticket, op, req["actor"], {})
        else:
            ticket = next((t for t in tickets if t["id"] == req["id"]), None)
            require(ticket is not None, "Ticket not found", "not_found")
            if op == "get":
                return ticket
            revision(ticket, req)
            # Comments remain open to all valid actors, including while claimed.
            if op != "comment":
                authorize(board, ticket, req)
            detail = {}
            if op == "move":
                require(req["status"] in [c["id"] for c in board["columns"]], "Unknown status")
                text(req["reason"], "reason", 8192, True)
                detail = {"from": ticket["status"], "to": req["status"], "reason": req["reason"]}
                ticket["status"] = req["status"]
                ticket["claim"] = None
            elif op == "comment":
                text(req["body"], "comment", MAX_BODY, True)
                ticket["comments"].append(
                    {
                        "actor": req["actor"],
                        "body": req["body"],
                        "at": now(),
                        "revision": ticket["revision"] + 1,
                    }
                )
            elif op == "claim":
                # A successful explicit takeover replaces the previous credential.
                ticket["claim"] = {
                    "actor": req["actor"],
                    "token": req.get("token", secrets.token_urlsafe(32)),
                }
            elif op == "release":
                require(ticket["claim"] is not None, "Ticket is not claimed", "claim_conflict")
                ticket["claim"] = None
            elif op == "assign":
                actor(req["owner"], True)
                detail = {"from": ticket["owner"], "to": req["owner"]}
                ticket["owner"] = req["owner"]
                ticket["claim"] = None
            elif op == "dispatch-intent":
                ident(req["intent_id"], "intent_id")
                previous = ticket["dispatch"]
                require(
                    previous is None or previous["state"] in ("confirmed", "failed"),
                    "Unresolved dispatch requires reconciliation",
                    "dispatch_conflict",
                )
                require(
                    not any(
                        e["op"] == op and e["detail"].get("intent_id") == req["intent_id"]
                        for t in tickets
                        for e in t["history"]
                    ),
                    "Intent ID already used",
                    "dispatch_conflict",
                )
                detail = {"intent_id": req["intent_id"]}
                ticket["dispatch"] = {
                    "intent_id": req["intent_id"],
                    "state": "pending",
                    "actor": req["actor"],
                    "owner": effective_owner(board, ticket),
                    "created_at": now(),
                    "updated_at": now(),
                    "job_id": None,
                    "detail": "",
                }
            elif op == "dispatch-result":
                ident(req["intent_id"], "intent_id")
                dispatch = ticket["dispatch"]
                require(
                    dispatch is not None and dispatch["intent_id"] == req["intent_id"],
                    "No matching dispatch intent",
                    "dispatch_conflict",
                )
                require(
                    dispatch["state"] in ("pending", "uncertain"),
                    "Dispatch already resolved",
                    "dispatch_conflict",
                )
                require(req["outcome"] in ("confirmed", "failed", "uncertain"), "Invalid outcome")
                if req["outcome"] == "confirmed":
                    text(req.get("job_id"), "job_id", 256, True)
                elif "job_id" in req:
                    text(req["job_id"], "job_id", 256, True)
                text(req.get("detail", ""), "detail", 8192)
                dispatch.update(
                    state=req["outcome"],
                    updated_at=now(),
                    job_id=req.get("job_id"),
                    detail=req.get("detail", ""),
                )
                detail = copy.deepcopy(dispatch)
            if (
                op in ("dispatch-intent", "dispatch-result")
                and privileged(board, req["actor"])
                and ticket["claim"] is not None
                and ticket["claim"]["actor"] != req["actor"]
            ):
                ticket["claim"] = None
            ticket["revision"] += 1
            event(ticket, op, req["actor"], detail)
        validate_ticket(ticket, ticket["id"], board)
        write(path / "tickets" / (ticket["id"] + ".json"), ticket)
        return ticket


def handle(root, board, request):
    """Execute one request; return a JSON-compatible success or error envelope."""
    try:
        return {"ok": True, "result": execute(root, board, copy.deepcopy(request))}
    except BoardError as exc:
        return {"ok": False, "error": {"code": exc.code, "message": str(exc)}}
    except (OSError, ValueError, TypeError, RecursionError):
        return {
            "ok": False,
            "error": {
                "code": "storage_error",
                "message": "Cannot access board state or invalid value",
            },
        }
