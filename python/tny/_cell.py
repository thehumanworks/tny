"""One host-authorized Python code cell on the user's own python3 (ADR 0222).

The execution server starts ``python3 -u -X utf8 -c LOADER`` with this source
as the first frame on the private protocol socket (fd 3). The cell runs like
the user's own ``python3 -c``: their environment, PYTHON* settings,
site-packages and virtual environment, with no restriction layer of tny's own.
Only the nested-tool bridge is bounded, because the parent re-checks every
frame anyway (code_runtime.c, code_policy.c).

Frames are a 4-byte big-endian length and that many bytes, led by a type byte:
S (start: catalog, working directory, code), C (call), R (result), F (terminal
failure), D (done). See ADR 0179 for the private cell protocol.

This file must stay self-contained and run on Python 3.8+: it is not imported
from the package, and it must not import anything a workspace could shadow
before sys.path is reset below.
"""

import sys

# The workspace directory joins the path only for the cell's own code.
sys.path[:] = [p for p in sys.path if p not in ("", ".")]

import atexit  # noqa: E402
import json  # noqa: E402
import linecache  # noqa: E402
import os  # noqa: E402
import socket  # noqa: E402
import threading  # noqa: E402
import traceback  # noqa: E402
import types  # noqa: E402

FD = 3
FRAME_MAX = 8 * 1024 * 1024
SOURCE_BYTES = 256 * 1024
ARGUMENT_BYTES = 256 * 1024
NAME_BYTES = 256
TOOL_CALLS = 64
TOOL_RESULT_BYTES = 8 * 1024 * 1024 - 16
OUTPUT_BYTES = 64 * 1024
METADATA_CALL = "@tny:call"
RECURSIVE = ("run_code", METADATA_CALL, "__parallel")
# The parent owns time; this only bounds the wait for a vanished parent.
PARENT_WAIT_S = (600_000 + 5 * 60 * 1000 + 1000) / 1000


class CellLimitError(BaseException):
    """A code-cell budget was exhausted; the cell is terminal."""


CellLimitError.__module__ = "tny"


class _Cell:
    def __init__(self):
        self.sock = None
        self.pid = os.getpid()
        self.lock = threading.Lock()
        self.catalog = "[]"
        self.calls = 0
        self.active = False
        self.finished = False

    # transport -------------------------------------------------------------

    def _read(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise EOFError
            data += chunk
        return data

    def receive(self):
        length = int.from_bytes(self._read(4), "big")
        if not length or length > FRAME_MAX:
            raise EOFError
        data = self._read(length)
        if b"\0" in data:
            raise EOFError
        return data

    def send(self, frame):
        if not frame or len(frame) > FRAME_MAX or b"\0" in frame:
            raise ValueError("invalid frame")
        self.sock.sendall(len(frame).to_bytes(4, "big") + frame)

    # terminal results ---------------------------------------------------------

    def finish(self, text):
        """Send the final frame and exit at once: nothing runs afterwards."""
        for stream in (sys.stdout, sys.stderr):
            try:
                stream.flush()
            except Exception:  # noqa: BLE001 - best effort, like interpreter exit
                pass
        status = 1
        try:
            if self.sock is not None:
                self.send(
                    b"D"
                    + text.encode("utf-8", "replace").replace(b"\0", b"\xef\xbf\xbd")
                )
                status = 0
        except Exception:  # noqa: BLE001 - the parent reports the lost result
            pass
        os._exit(status)

    def terminal(self, reason):
        """A spent budget or failed callback ends the cell, uncatchably."""
        self.finish("error: code: " + reason)

    def forked(self):
        # A fork() child keeps running Python but is not the cell: it must
        # neither hold the parent's EOF open nor write a frame.
        if self.sock is not None:
            try:
                self.sock.detach()
                os.close(FD)
            except OSError:
                pass
            self.sock = None

    def refuse(self):
        if self.finished or not self.active:
            raise RuntimeError("the code cell has finished; tools are closed")
        if os.getpid() != self.pid or self.sock is None:
            raise RuntimeError("tools are unavailable in a forked child process")

    # nested calls -------------------------------------------------------------

    def round_trip(self, name, arguments):
        frame = b"C" + b"%03d" % len(name) + name + arguments
        with self.lock:
            try:
                self.send(frame)
                reply = self.receive()
            except (OSError, EOFError, ValueError):
                self.terminal("cell transport failed")
        kind, body = reply[:1], reply[1:]
        if kind == b"R":
            if len(body) > TOOL_RESULT_BYTES:
                self.terminal("tool result limit exceeded")
            return body.decode("utf-8", "replace")
        if kind == b"F":
            self.terminal(body.decode("utf-8", "replace") or "tool callback failed")
        self.terminal("cell protocol violation")
        return ""  # unreachable


CELL = _Cell()


def _is_object_text(text):
    try:
        return isinstance(json.loads(text), dict)
    except ValueError:
        return False


def call(name, arguments_json, reason=None):
    """call(name, arguments_json) -> str: run a permitted nested tool"""
    if (
        not isinstance(name, str)
        or not isinstance(arguments_json, str)
        or (reason is not None and not isinstance(reason, str))
    ):
        raise TypeError(
            "tools.call(name, arguments_json[, reason]) takes a tool name, optional reason and "
            "JSON object text such as json.dumps({...})"
        )
    CELL.refuse()
    raw_name = name.encode("utf-8", "surrogatepass")
    raw_args = arguments_json.encode("utf-8", "surrogatepass")
    if b"\0" in raw_name or b"\0" in raw_args:
        raise ValueError("tool name and arguments cannot contain NUL")
    if not raw_name or len(raw_name) > NAME_BYTES:
        raise ValueError("invalid tool name")
    if name in RECURSIVE:
        raise ValueError("recursive run_code is forbidden")
    if len(raw_args) > ARGUMENT_BYTES:
        raise ValueError("tool arguments limit exceeded")
    if not _is_object_text(arguments_json):
        raise ValueError("tool arguments must be JSON object text")
    if CELL.calls >= TOOL_CALLS:
        CELL.terminal("tool call limit exceeded")
    envelope = None
    if reason is not None:
        raw_reason = reason.encode("utf-8", "surrogatepass")
        if len(raw_reason) > 256 or b"\0" in raw_reason:
            raise ValueError(
                "reason must be short text (at most 256 bytes), without NUL"
            )
        # Display metadata stays outside business arguments.
        envelope = json.dumps(
            {"name": name, "arguments_json": arguments_json, "reason": reason},
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8", "surrogatepass")
    CELL.calls += 1
    if envelope is not None:
        return CELL.round_trip(METADATA_CALL.encode(), envelope)
    return CELL.round_trip(raw_name, raw_args)


def list():  # noqa: A001 - the documented tools.list() name
    """list() -> str: JSON catalog of permitted tools"""
    CELL.refuse()
    return CELL.catalog


_ASCII_LOWER = {c: c + 32 for c in range(ord("A"), ord("Z") + 1)}


def _entries():
    try:
        entries = json.loads(CELL.catalog)
    except ValueError:
        return []
    return entries if isinstance(entries, type([])) else []


def _function(entry):
    if not isinstance(entry, dict):
        return {}
    fn = entry.get("function")
    return fn if isinstance(fn, dict) else entry


def search(query="", limit=10):
    """search(query='', limit=10) -> str: bounded name/description matches, total, truncated"""
    CELL.refuse()
    if (
        not isinstance(query, str)
        or not isinstance(limit, int)
        or isinstance(limit, bool)
    ):
        raise TypeError(
            "tools.search(query='', limit=10) takes a string and an integer"
        )
    raw = query.encode("utf-8", "surrogatepass")
    if len(raw) > 256 or b"\0" in raw or limit < 1 or limit > 100:
        raise ValueError(
            "search requires a query of at most 256 UTF-8 bytes without NUL and "
            "a limit between 1 and 100"
        )
    # ASCII folding is locale-independent; non-ASCII matches exactly.
    needle = query.translate(_ASCII_LOWER)
    matches, total = [], 0
    for entry in _entries():
        fn = _function(entry)
        name, description = fn.get("name"), fn.get("description")
        if not isinstance(name, str):
            continue
        if not isinstance(description, str):
            description = None
        if needle not in name.translate(_ASCII_LOWER) and (
            description is None or needle not in description.translate(_ASCII_LOWER)
        ):
            continue
        total += 1
        if total > limit:
            continue
        summary = (description or "").encode("utf-8", "surrogatepass")[:240]
        matches.append({"name": name, "description": summary.decode("utf-8", "ignore")})
    return json.dumps(
        {"tools": matches, "total": total, "truncated": total > limit},
        ensure_ascii=False,
        separators=(",", ":"),
    )


def describe(name):
    """describe(name) -> str | None: one tool's JSON schema"""
    if not isinstance(name, str):
        raise TypeError("tools.describe(name) takes a tool name string")
    CELL.refuse()
    if "\0" in name:
        raise ValueError("tool name cannot contain NUL")
    for entry in _entries():
        if _function(entry).get("name") == name:
            return json.dumps(entry, ensure_ascii=False, separators=(",", ":"))
    return None


def parallel(calls_json):
    """parallel(calls_json) -> str: ordered JSON results; independent reads overlap"""
    if not isinstance(calls_json, str):
        raise TypeError("tools.parallel(calls_json) takes JSON array text")
    CELL.refuse()
    raw = calls_json.encode("utf-8", "surrogatepass")
    if len(raw) > ARGUMENT_BYTES or b"\0" in raw:
        raise ValueError("parallel arguments limit exceeded or contain NUL")
    try:
        calls = json.loads(calls_json)
    except ValueError:
        calls = None
    if not isinstance(calls, type([])) or not calls or len(calls) > TOOL_CALLS:
        raise ValueError("parallel calls must be a nonempty JSON array")
    if len(calls) > TOOL_CALLS - CELL.calls:
        CELL.terminal("tool call limit exceeded")
    CELL.calls += len(calls)
    return CELL.round_trip(b"__parallel", b'{"calls":' + raw + b"}")


class _ToolPath:
    """tools.jira.projects.list(status="open") is tools.call("jira.projects.list", ...)."""

    __slots__ = ("_name",)

    def __init__(self, name):
        self._name = name

    def __getattr__(self, part):
        if part.startswith("__"):
            raise AttributeError(part)
        return _ToolPath(self._name + "." + part)

    def __call__(self, **arguments):
        return call(self._name, json.dumps(arguments))

    def __repr__(self):
        return "<tool " + self._name + ">"


def _tools_module():
    tools = types.ModuleType("tools", "Nested tny tools for this code cell.")
    for fn in (call, list, search, describe, parallel):
        setattr(tools, fn.__name__, fn)
    tools.CellLimitError = CellLimitError

    def __getattr__(name):
        if name.startswith("__"):
            raise AttributeError(name)
        return _ToolPath(name)

    tools.__getattr__ = __getattr__
    return tools


# running the cell ----------------------------------------------------------


def _parse_start(frame):
    """b"S" catalog_len ":" cwd_len ":" catalog cwd code (byte lengths)."""
    if frame[:1] != b"S":
        return None
    try:
        first, rest = frame[1:].split(b":", 1)
        second, body = rest.split(b":", 1)
        a, b = int(first), int(second)
    except ValueError:
        return None
    if (
        not first.isdigit()
        or not second.isdigit()
        or a > len(body)
        or b > len(body) - a
    ):
        return None
    return body[:a], body[a : a + b], body[a + b :]


def _summary(error):
    """'error: code: Type: message (line N)' for the cell's last frame."""
    try:
        message = str(error)
    except Exception:  # noqa: BLE001 - a broken __str__ keeps the type name
        message = ""
    line = -1
    if isinstance(error, SyntaxError) and isinstance(error.lineno, int):
        line = error.lineno
    tb = error.__traceback__
    while tb is not None:
        if tb.tb_frame.f_code.co_filename == "<code>":
            line = tb.tb_lineno
        tb = tb.tb_next
    text = "error: code: " + type(error).__name__
    if message:
        text += ": " + message[: OUTPUT_BYTES // 2]
    if line > 0:
        text += " (line %d)" % line
    return text


def _print_traceback(error, code):
    # Like `python -c`, with the cell's own source lines.
    try:
        linecache.cache["<code>"] = (len(code), None, code.splitlines(True), "<code>")
        traceback.print_exception(type(error), error, error.__traceback__)
    except Exception:  # noqa: BLE001 - the summary line still reports the error
        pass


def _system_exit(error):
    code = error.code
    if code is None or (
        isinstance(code, int) and not isinstance(code, bool) and code == 0
    ):
        return ""
    try:
        value = str(code)
    except Exception:  # noqa: BLE001
        value = "?"
    return "error: code: SystemExit: " + value[: OUTPUT_BYTES // 2]


def _settle():
    # As at interpreter exit, while the cell is still live: non-daemon threads
    # finish, atexit handlers (tempfile cleanups) run, stdio flushes.
    for step in (
        getattr(threading, "_shutdown", None),
        getattr(atexit, "_run_exitfuncs", None),
    ):
        try:
            if step is not None:
                step()
        except BaseException:  # noqa: BLE001 - exit-time failures do not change the result
            pass


def main():
    os.set_blocking(FD, True)
    os.set_inheritable(FD, False)  # exec'd programs never hold the socket
    CELL.sock = socket.socket(fileno=FD)
    CELL.sock.settimeout(PARENT_WAIT_S)
    os.register_at_fork(after_in_child=CELL.forked)
    try:
        start = _parse_start(CELL.receive())
    except (OSError, EOFError):
        start = None
    if start is None:
        CELL.finish("error: code: cell protocol violation")
    catalog, cwd, code_bytes = start
    if len(code_bytes) > SOURCE_BYTES:
        CELL.finish("error: code: source limit exceeded")
    CELL.catalog = catalog.decode("utf-8", "replace")
    if cwd:
        try:
            os.chdir(cwd)
        except OSError as error:
            CELL.finish(
                "error: code: could not enter the working directory: "
                + str(error.strerror or error)
            )
        # Shells and tools trust PWD when it names the working directory.
        os.environ["PWD"] = os.fsdecode(cwd)
    code = code_bytes.decode("utf-8", "replace")

    tools = _tools_module()
    sys.modules["tools"] = tools
    main_module = types.ModuleType("__main__")
    main_module.__dict__.update(
        {"__builtins__": __builtins__, "tools": tools, "json": json}
    )
    sys.modules["__main__"] = main_module
    # As for `python -c`: the working directory's modules import first.
    sys.path.insert(0, "")

    result = ""
    CELL.active = True
    try:
        exec(compile(code, "<code>", "exec"), main_module.__dict__)
    except SystemExit as error:
        result = _system_exit(error)
    except BaseException as error:  # noqa: BLE001 - every failure becomes the cell's result
        result = _summary(error)
        _print_traceback(error, code)
    _settle()
    CELL.active = False
    CELL.finished = True
    CELL.finish(result)


if __name__ == "__main__":
    main()
