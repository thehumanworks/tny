"""The tool host: load extension modules and serve them over MCP stdio.

``tny --tool-host`` runs this inside tny's embedded CPython. The C side
passes the extension directories in precedence order (project before
global); a project module shadows a global module of the same name.

Modes:
  catalog   print one JSON catalog object and exit
  serve     JSON-RPC 2.0 over stdin/stdout, one message per line: MCP
            initialize, tools/list and tools/call, plus tny/catalog
"""

from __future__ import annotations

import asyncio
import contextlib
import dataclasses
import enum
import importlib.machinery
import importlib.util
import io
import json
import os
import sys
import traceback
import types
from typing import Any

from tny import _mcp, _registry, _schema
from tny._registry import HOST_PACKAGE, NAME_RE, Group, Proxy, Tool, ToolError

PROTOCOL_VERSION = "2025-06-18"


@dataclasses.dataclass
class Module:
    name: str
    path: str
    scope: str
    entry: str
    shadows: str | None = None


@dataclasses.dataclass
class Entry:
    name: str
    module: Module
    tool: Tool | None
    proxy: Proxy | None
    remote: str | None
    description: str
    schema: dict[str, Any]


def discover(dirs: list[tuple[str, str]]) -> tuple[list[Module], list[dict[str, Any]]]:
    """Modules by name; the first directory that defines a name wins."""
    found: dict[str, Module] = {}
    problems: list[dict[str, Any]] = []
    for scope, directory in dirs:
        try:
            names = sorted(os.listdir(directory))
        except FileNotFoundError:
            continue
        except OSError as exc:
            problems.append(
                {
                    "path": directory,
                    "scope": scope,
                    "error": f"cannot list: {exc.strerror}",
                }
            )
            continue
        for item in names:
            if item.startswith((".", "_")):
                continue
            path = os.path.join(directory, item)
            if item.endswith(".py") and os.path.isfile(path):
                name, entry = item[:-3], path
            elif os.path.isdir(path):
                entry = os.path.join(path, "__main__.py")
                if not os.path.isfile(entry):
                    continue
                name = item
            else:
                continue
            if not NAME_RE.match(name):
                problems.append(
                    {
                        "module": name,
                        "path": path,
                        "scope": scope,
                        "error": "invalid module name",
                    }
                )
                continue
            if name in found:
                if found[name].shadows is None:
                    found[name].shadows = path
                continue
            found[name] = Module(name=name, path=path, scope=scope, entry=entry)
    return list(found.values()), problems


def _ensure_namespace() -> None:
    if HOST_PACKAGE not in sys.modules:
        pkg = types.ModuleType(HOST_PACKAGE)
        pkg.__path__ = []  # type: ignore[attr-defined]
        sys.modules[HOST_PACKAGE] = pkg


def load(module: Module) -> None:
    """Import one extension under ``_tny_tools.NAME``. Raises on failure."""
    _ensure_namespace()
    qualified = f"{HOST_PACKAGE}.{module.name}"
    is_package = module.entry.endswith("__main__.py")
    spec = importlib.util.spec_from_file_location(
        qualified,
        module.entry,
        submodule_search_locations=[os.path.dirname(module.entry)]
        if is_package
        else None,
    )
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load {module.entry}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[qualified] = mod
    try:
        with contextlib.redirect_stdout(sys.stderr):
            spec.loader.exec_module(mod)
    except BaseException:
        sys.modules.pop(qualified, None)
        raise


def _first_line(text: str | None) -> str:
    return (text or "").strip()


def _doc_summary(module_name: str) -> str:
    """The first paragraph of a loaded module's docstring, on one line."""
    module = sys.modules.get(module_name)
    doc = getattr(module, "__doc__", None) if module is not None else None
    if not isinstance(doc, str):
        return ""
    return " ".join(doc.strip().split("\n\n", 1)[0].split())


def _resolve_proxy(proxy: Proxy) -> None:
    if proxy.loaded:
        return
    proxy.loaded = True
    try:
        proxy.client = _mcp.connect(proxy)
        remote = proxy.client.list_tools()
    except Exception as exc:
        proxy.load_error = str(exc) or type(exc).__name__
        return
    for t in remote:
        rname = t.get("name")
        if not isinstance(rname, str):
            continue
        if proxy.include is not None and rname not in proxy.include:
            continue
        if rname in proxy.exclude:
            continue
        local = proxy.rename.get(rname, rname)
        if not NAME_RE.match(local):
            local = "".join(c if c.isalnum() or c in "_-" else "_" for c in local)[:64]
            if not local or not NAME_RE.match(local):
                continue
        if local in proxy.tools or local in proxy.remote_names:
            continue
        proxy.remote_names[local] = rname
        schema = (
            t.get("inputSchema")
            if isinstance(t.get("inputSchema"), dict)
            else {"type": "object"}
        )
        proxy.tools[local] = Tool(
            fn=_proxy_call(proxy, rname),
            name=local,
            description=t.get("description")
            if isinstance(t.get("description"), str)
            else "",
            input_schema=schema,
        )


def _proxy_call(proxy: Proxy, remote: str) -> Any:
    def call(**arguments: Any) -> Any:
        if proxy.client is None:
            raise ToolError(
                proxy.load_error or "the proxied MCP server is not connected"
            )
        try:
            result = proxy.client.call_tool(remote, arguments)
        except _mcp.MCPError as exc:
            raise ToolError(str(exc)) from None
        text = _mcp.result_text(result)
        if result.get("isError"):
            raise ToolError(text or "the proxied tool failed")
        return text

    call.__tny_proxy__ = True  # type: ignore[attr-defined]
    return call


def _walk(
    group: Group, prefix: tuple[str, ...]
) -> list[tuple[tuple[str, ...], Tool, Proxy | None]]:
    out: list[tuple[tuple[str, ...], Tool, Proxy | None]] = []
    unnamed = getattr(group, "_proxy", None)
    if isinstance(unnamed, Proxy):
        _resolve_proxy(unnamed)
        for n, t in unnamed.tools.items():
            if n not in group.tools and n not in group.groups:
                out.append((prefix + (n,), t, unnamed))
    for n, t in group.tools.items():
        out.append((prefix + (n,), t, group if isinstance(group, Proxy) else None))
    for n, g in group.groups.items():
        if isinstance(g, Proxy):
            _resolve_proxy(g)
        out.extend(_walk(g, prefix + (n,)))
    return out


class Host:
    def __init__(self, dirs: list[tuple[str, str]]) -> None:
        self.entries: dict[str, Entry] = {}
        self.groups: list[dict[str, Any]] = []
        self.errors: list[dict[str, Any]] = []
        self.warnings: list[dict[str, Any]] = []
        self.modules: list[Module] = []
        _registry.reset()
        modules, problems = discover(dirs)
        self.errors.extend(problems)
        for m in modules:
            if m.shadows:
                self.warnings.append(
                    {
                        "module": m.name,
                        "path": m.path,
                        "scope": m.scope,
                        "warning": f"shadows {m.shadows}",
                    }
                )
            try:
                load(m)
            except (
                BaseException
            ) as exc:  # SystemExit/KeyboardInterrupt in a module must not kill the host
                self.errors.append(
                    {
                        "module": m.name,
                        "path": m.path,
                        "scope": m.scope,
                        "error": "".join(
                            traceback.format_exception_only(type(exc), exc)
                        ).strip(),
                        "traceback": "".join(traceback.format_exception(exc))[-8000:],
                    }
                )
                continue
            self.modules.append(m)
            self._collect(m)

    def _collect(self, m: Module) -> None:
        root = _registry.roots().get(f"{HOST_PACKAGE}.{m.name}")
        if root is None:
            self.warnings.append(
                {
                    "module": m.name,
                    "path": m.path,
                    "scope": m.scope,
                    "warning": "declares no tools",
                }
            )
            return
        listed = _walk(root, ())
        for proxy in _proxies(root):
            if proxy.load_error:
                self.errors.append(
                    {
                        "module": m.name,
                        "path": m.path,
                        "scope": m.scope,
                        "error": f"proxy: {proxy.load_error}",
                    }
                )
        if not listed:
            if not any(p.load_error for p in _proxies(root)):
                self.warnings.append(
                    {
                        "module": m.name,
                        "path": m.path,
                        "scope": m.scope,
                        "warning": "declares no tools",
                    }
                )
            return
        collapse = len(listed) == 1 and len(listed[0][0]) == 1
        if not collapse:
            self._collect_groups(m, root, listed)
        for path, t, proxy in listed:
            name = m.name if collapse else ".".join((m.name,) + path)
            try:
                if t._compiled is None:
                    t._compiled = _schema.Compiled(t.fn, t.description, t.input_schema)
            except Exception as exc:
                self.errors.append(
                    {
                        "module": m.name,
                        "path": m.path,
                        "scope": m.scope,
                        "tool": name,
                        "error": f"schema: {type(exc).__name__}: {exc}",
                    }
                )
                continue
            self.entries[name] = Entry(
                name=name,
                module=m,
                tool=t,
                proxy=proxy,
                remote=proxy.remote_names.get(path[-1]) if proxy else None,
                description=_first_line(t._compiled.description),
                schema=t._compiled.schema,
            )

    def _collect_groups(
        self,
        m: Module,
        root: Group,
        listed: list[tuple[tuple[str, ...], Tool, Proxy | None]],
    ) -> None:
        """Help entries for the module and every group that holds a tool.

        A description comes from ``tny.group(name, description)``, else from
        the docstring of the module that defines the group (``__main__.py``
        or a single-file module for the root, ``projects.py`` for
        ``jira.projects``), else from a proxy's ``description=``.
        """
        prefixes = sorted(
            {path[:i] for path, _, _ in listed for i in range(1, len(path))}
        )
        base = f"{HOST_PACKAGE}.{m.name}"
        unnamed = getattr(root, "_proxy", None)
        for path in [(), *prefixes]:
            group: Group | None = root
            for part in path:
                group = group.groups.get(part) if group is not None else None
            description = (
                group.description if group is not None else None
            ) or _doc_summary(".".join((base, *path)))
            if not description and not path and isinstance(unnamed, Proxy):
                description = unnamed.description or ""
            self.groups.append(
                {
                    "name": ".".join((m.name, *path)),
                    "description": description or "",
                    "module": m.name,
                    "scope": m.scope,
                }
            )

    def catalog(self) -> dict[str, Any]:
        tools = []
        for e in self.entries.values():
            tools.append(
                {
                    "name": e.name,
                    "description": e.description,
                    "inputSchema": e.schema,
                    "module": e.module.name,
                    "scope": e.module.scope,
                    "source": e.module.path,
                    **({"proxy": True} if e.proxy else {}),
                    **(
                        {"timeout": e.tool.timeout}
                        if e.tool is not None and e.tool.timeout
                        else {}
                    ),
                }
            )
        return {
            "tools": tools,
            "groups": self.groups,
            "errors": self.errors,
            "warnings": self.warnings,
        }

    def call(self, name: str, arguments: Any) -> tuple[str, bool, Any]:
        """(text, is_error, structured) for one call."""
        entry = self.entries.get(name)
        if entry is None or entry.tool is None:
            return f"unknown tool {name}", True, None
        compiled = entry.tool._compiled
        captured = io.StringIO()
        try:
            if entry.proxy is not None:
                kwargs = arguments if isinstance(arguments, dict) else {}
            else:
                kwargs = compiled.bind(arguments if arguments is not None else {})
            with contextlib.redirect_stdout(captured):
                value = entry.tool.fn(**kwargs)
                if asyncio.iscoroutine(value):
                    value = asyncio.run(value)
        except ToolError as exc:
            return str(exc) or "tool error", True, None
        except BaseException as exc:
            if isinstance(exc, KeyboardInterrupt):
                raise
            tb = traceback.extract_tb(exc.__traceback__)
            where = ""
            for frame in reversed(tb):
                if (
                    frame.filename.startswith(os.path.dirname(entry.module.entry))
                    or frame.filename == entry.module.entry
                ):
                    where = f" ({os.path.basename(frame.filename)}:{frame.lineno})"
                    break
            traceback.print_exception(exc, file=sys.stderr)
            return f"{type(exc).__name__}: {exc}{where}", True, None
        printed = captured.getvalue()
        if value is None:
            return printed, False, None
        if printed:
            sys.stderr.write(printed)
        return render(value)


def _proxies(group: Group) -> list[Proxy]:
    out = []
    unnamed = getattr(group, "_proxy", None)
    if isinstance(unnamed, Proxy):
        out.append(unnamed)
    if isinstance(group, Proxy):
        out.append(group)
    for g in group.groups.values():
        out.extend(_proxies(g))
    return out


def _jsonable(value: Any) -> Any:
    if hasattr(value, "model_dump") and callable(value.model_dump):
        return value.model_dump(mode="json")
    if dataclasses.is_dataclass(value) and not isinstance(value, type):
        return dataclasses.asdict(value)
    if isinstance(value, enum.Enum):
        return value.value
    if isinstance(value, (set, frozenset, tuple)):
        return list(value)
    if isinstance(value, bytes):
        import base64

        return base64.b64encode(value).decode()
    if hasattr(value, "isoformat"):
        return value.isoformat()
    return str(value)


def render(value: Any) -> tuple[str, bool, Any]:
    if isinstance(value, str):
        return value, False, None
    try:
        text = json.dumps(value, ensure_ascii=False, default=_jsonable)
    except (TypeError, ValueError):
        return str(value), False, None
    structured = json.loads(text)
    return text, False, structured if isinstance(structured, dict) else None


# ---- MCP stdio server -------------------------------------------------------


class Server:
    def __init__(self, host: Host, out: Any) -> None:
        self.host = host
        self.out = out

    def send(self, msg: dict[str, Any]) -> None:
        self.out.write(
            json.dumps(msg, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"
        )
        self.out.flush()

    def reply(self, rid: Any, result: Any) -> None:
        self.send({"jsonrpc": "2.0", "id": rid, "result": result})

    def error(self, rid: Any, code: int, message: str) -> None:
        self.send(
            {"jsonrpc": "2.0", "id": rid, "error": {"code": code, "message": message}}
        )

    def handle(self, msg: Any) -> None:
        if not isinstance(msg, dict):
            self.error(None, -32600, "invalid request")
            return
        method = msg.get("method")
        rid = msg.get("id")
        if not isinstance(method, str):
            return
        if rid is None:
            return  # notifications need no answer
        params = msg.get("params") if isinstance(msg.get("params"), dict) else {}
        if method == "initialize":
            self.reply(
                rid,
                {
                    "protocolVersion": params.get("protocolVersion")
                    or PROTOCOL_VERSION,
                    "capabilities": {"tools": {"listChanged": False}},
                    "serverInfo": {"name": "tny-tool-host", "version": "1"},
                },
            )
        elif method == "ping":
            self.reply(rid, {})
        elif method == "tools/list":
            tools = [
                {
                    "name": t["name"],
                    "description": t["description"],
                    "inputSchema": t["inputSchema"],
                }
                for t in self.host.catalog()["tools"]
            ]
            self.reply(rid, {"tools": tools})
        elif method == "tny/catalog":
            self.reply(rid, self.host.catalog())
        elif method == "tools/call":
            name = params.get("name")
            if not isinstance(name, str):
                self.error(rid, -32602, "tools/call needs a name")
                return
            text, is_error, structured = self.host.call(name, params.get("arguments"))
            result: dict[str, Any] = {
                "content": [{"type": "text", "text": text}],
                "isError": is_error,
            }
            if structured is not None:
                result["structuredContent"] = structured
            self.reply(rid, result)
        else:
            self.error(rid, -32601, f"method not found: {method}")

    def serve(self, stream: Any) -> None:
        for raw in stream:
            line = raw.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except ValueError:
                self.error(None, -32700, "parse error")
                continue
            self.handle(msg)


def _parse_dirs(args: list[str]) -> tuple[str, list[tuple[str, str]]]:
    mode = args[0] if args else "serve"
    dirs: list[tuple[str, str]] = []
    rest = args[1:]
    while rest:
        flag = rest.pop(0)
        if flag in ("--project", "--global") and rest:
            dirs.append((flag[2:], rest.pop(0)))
        else:
            raise SystemExit(f"tool host: unknown argument {flag}")
    return mode, dirs


def main(args: list[str]) -> int:
    # The protocol owns the original stdout; everything a tool prints, or a
    # subprocess writes to fd 1, goes to stderr instead.
    sys.dont_write_bytecode = True
    protocol = os.fdopen(os.dup(1), "wb", buffering=0)
    os.dup2(2, 1)
    sys.stdout = sys.stderr
    # Workspace files must not shadow modules extensions import.
    sys.path[:] = [p for p in sys.path if p not in ("", ".")]
    mode, dirs = _parse_dirs(args)
    host = Host(dirs)
    if mode == "catalog":
        protocol.write(json.dumps(host.catalog(), ensure_ascii=False).encode() + b"\n")
        return 0
    if mode != "serve":
        sys.stderr.write(f"tool host: unknown mode {mode}\n")
        return 2
    Server(host, protocol).serve(
        io.TextIOWrapper(sys.stdin.buffer, encoding="utf-8", errors="replace")
    )
    return 0
