"""Tool, group and proxy declarations.

Declarations register into a per-module tree keyed by the defining module's
``__name__``. The tool host loads each extension under the private package
``_tny_tools`` (``_tny_tools.jira``, ``_tny_tools.jira.projects``), so a
submodule's tools nest under the submodule name. Outside the host the tree is
still built, which keeps extensions importable and testable as plain Python.
"""

from __future__ import annotations

import inspect
import re
import sys
from dataclasses import dataclass, field
from typing import Any, Callable, TypeVar, overload

# A decorated function keeps its own type, so editors still see its signature.
_F = TypeVar("_F", bound=Callable[..., Any])

NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_-]{0,63}$")
HOST_PACKAGE = "_tny_tools"


class ToolError(Exception):
    """Raise from a tool to return an error result with this message only."""


@dataclass(frozen=True)
class Param:
    """Per-parameter metadata, used as ``Annotated[T, tny.Param(...)]``."""

    description: str | None = None
    name: str | None = None
    examples: tuple[Any, ...] | None = None
    json_schema: dict[str, Any] | None = None


def _check_name(name: str, what: str) -> str:
    if not isinstance(name, str) or not NAME_RE.match(name):
        raise ValueError(
            f"invalid {what} name {name!r}: use letters, digits, '_' or '-', "
            "starting with a letter or '_' (at most 64 characters)"
        )
    return name


@dataclass
class Tool:
    """One declared tool. ``fn`` stays callable as ordinary Python."""

    fn: Callable[..., Any]
    name: str
    description: str | None = None
    input_schema: dict[str, Any] | None = None
    sensitive: bool = True
    timeout: float | None = None
    _compiled: Any = field(default=None, repr=False)

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        return self.fn(*args, **kwargs)


class Group:
    """A named set of tools and nested groups."""

    def __init__(self, name: str, description: str | None = None) -> None:
        self.name = _check_name(name, "group")
        self.description = description
        self.tools: dict[str, Tool] = {}
        self.groups: dict[str, Group] = {}

    @overload
    def tool(
        self,
        fn: _F,
        /,
        *,
        name: str | None = None,
        description: str | None = None,
        input_schema: dict[str, Any] | None = None,
        timeout: float | None = None,
    ) -> _F: ...
    @overload
    def tool(
        self,
        fn: None = None,
        /,
        *,
        name: str | None = None,
        description: str | None = None,
        input_schema: dict[str, Any] | None = None,
        timeout: float | None = None,
    ) -> Callable[[_F], _F]: ...
    def tool(
        self,
        fn: Callable[..., Any] | None = None,
        /,
        *,
        name: str | None = None,
        description: str | None = None,
        input_schema: dict[str, Any] | None = None,
        timeout: float | None = None,
    ) -> Any:
        """Declare a tool in this group: ``@group.tool`` or ``@group.tool(...)``."""

        def register(f: Callable[..., Any]) -> Callable[..., Any]:
            if not callable(f):
                raise TypeError("@tool decorates a function")
            tool_name = _check_name(name or f.__name__.strip("_") or f.__name__, "tool")
            if tool_name in self.tools or tool_name in self.groups:
                raise ValueError(f"duplicate tool name {tool_name!r} in {self.name!r}")
            entry = Tool(
                fn=f,
                name=tool_name,
                description=description,
                input_schema=input_schema,
                timeout=timeout,
            )
            self.tools[tool_name] = entry
            try:
                f.__tny_tool__ = entry  # type: ignore[attr-defined]
            except (AttributeError, TypeError):
                pass
            return f

        if fn is not None:
            return register(fn)
        return register

    def group(self, name: str, description: str | None = None) -> Group:
        """A nested group: ``projects = jira.group("projects")``."""
        _check_name(name, "group")
        if name in self.tools:
            raise ValueError(f"group {name!r} collides with a tool in {self.name!r}")
        child = self.groups.get(name)
        if child is None:
            child = Group(name, description)
            self.groups[name] = child
        elif description and not child.description:
            child.description = description
        return child

    def mount(self, other: Group, name: str | None = None) -> Group:
        """Nest an existing group (for example a ``tny.proxy``) under ``name``."""
        key = _check_name(name or other.name, "group")
        if key in self.tools or key in self.groups:
            raise ValueError(
                f"cannot mount {key!r}: the name is taken in {self.name!r}"
            )
        if name:
            other.name = key
        self.groups[key] = other
        return other

    def walk(self, prefix: tuple[str, ...] = ()) -> list[tuple[tuple[str, ...], Tool]]:
        out = [(prefix + (n,), t) for n, t in self.tools.items()]
        for n, g in self.groups.items():
            out.extend(g.walk(prefix + (n,)))
        return out


class Proxy(Group):
    """A group whose tools come from an existing MCP server.

    ``command`` starts a stdio server; ``url`` names a Streamable HTTP one.
    Its tools are listed once when the host loads the module. ``include`` and
    ``exclude`` filter remote tool names; ``rename`` maps remote names to the
    names tny shows.
    """

    def __init__(
        self,
        name: str,
        *,
        command: list[str] | None = None,
        url: str | None = None,
        env: dict[str, str] | None = None,
        cwd: str | None = None,
        headers: dict[str, str] | None = None,
        include: list[str] | None = None,
        exclude: list[str] | None = None,
        rename: dict[str, str] | None = None,
        timeout: float = 30.0,
        description: str | None = None,
    ) -> None:
        super().__init__(name, description)
        if (command is None) == (url is None):
            raise ValueError("tny.proxy needs exactly one of command=[...] or url=...")
        if command is not None and (
            not isinstance(command, (list, tuple))
            or not command
            or not all(isinstance(a, str) for a in command)
        ):
            raise ValueError("tny.proxy command must be a non-empty list of strings")
        self.command = list(command) if command is not None else None
        self.url = url
        self.env = dict(env or {})
        self.cwd = cwd
        self.headers = dict(headers or {})
        self.include = set(include) if include else None
        self.exclude = set(exclude or ())
        self.rename = dict(rename or {})
        self.timeout = float(timeout)
        self.client: Any = None
        self.remote_names: dict[str, str] = {}
        self.loaded = False
        self.load_error: str | None = None


# Module trees: real module name -> root group. The root's own name is the
# extension name (the file stem or folder name).
_roots: dict[str, Group] = {}


def _extension_of(module_name: str) -> tuple[str, tuple[str, ...]]:
    """(extension root module, nested group path) for a defining module."""
    parts = module_name.split(".")
    if parts[0] == HOST_PACKAGE and len(parts) >= 2:
        return ".".join(parts[:2]), tuple(parts[2:])
    return parts[0], tuple(parts[1:])


def root_for(module_name: str) -> Group:
    root_name, _ = _extension_of(module_name)
    root = _roots.get(root_name)
    if root is None:
        short = root_name.split(".")[-1]
        root = Group(short if NAME_RE.match(short) else "_")
        _roots[root_name] = root
    return root


def _group_for(module_name: str) -> Group:
    root_name, path = _extension_of(module_name)
    g = root_for(root_name)
    for part in path:
        if part == "__main__":
            continue
        g = g.group(part)
    return g


def _caller_module(depth: int = 2) -> str:
    frame = sys._getframe(depth)
    return str(frame.f_globals.get("__name__", "__main__"))


@overload
def tool(
    fn: _F,
    /,
    *,
    name: str | None = None,
    description: str | None = None,
    input_schema: dict[str, Any] | None = None,
    timeout: float | None = None,
) -> _F: ...
@overload
def tool(
    fn: None = None,
    /,
    *,
    name: str | None = None,
    description: str | None = None,
    input_schema: dict[str, Any] | None = None,
    timeout: float | None = None,
) -> Callable[[_F], _F]: ...
def tool(
    fn: Callable[..., Any] | None = None,
    /,
    *,
    name: str | None = None,
    description: str | None = None,
    input_schema: dict[str, Any] | None = None,
    timeout: float | None = None,
) -> Any:
    """Declare a tool: ``@tny.tool`` or ``@tny.tool(name=..., description=...)``.

    The input schema comes from the signature: type hints (``Annotated``,
    ``Literal``, enums, dataclasses, ``TypedDict`` and pydantic models
    included) and defaults. The description is the docstring summary; an
    ``Args:`` section documents parameters. ``input_schema`` replaces the
    inferred schema entirely.
    """

    def register(f: Callable[..., Any]) -> Callable[..., Any]:
        module = getattr(f, "__module__", None) or _caller_module(3)
        return _group_for(module).tool(
            f,
            name=name,
            description=description,
            input_schema=input_schema,
            timeout=timeout,
        )

    if fn is not None:
        return register(fn)
    return register


def group(name: str, description: str | None = None) -> Group:
    """A named group in the calling module: ``projects = tny.group("projects")``."""
    return _group_for(_caller_module()).group(name, description)


def proxy(
    command: list[str] | None = None,
    *,
    url: str | None = None,
    name: str | None = None,
    env: dict[str, str] | None = None,
    cwd: str | None = None,
    headers: dict[str, str] | None = None,
    include: list[str] | None = None,
    exclude: list[str] | None = None,
    rename: dict[str, str] | None = None,
    timeout: float = 30.0,
    description: str | None = None,
) -> Proxy:
    """Expose an existing MCP server's tools from this extension.

    Without ``name`` the remote tools sit directly under the extension
    (``github.create_issue`` from ``github.py``); with ``name`` they nest one
    level deeper. Configuration stays in Python: read secrets from
    ``os.environ`` rather than writing them into the file.
    """
    parent = _group_for(_caller_module())
    p = Proxy(
        name or parent.name,
        command=command,
        url=url,
        env=env,
        cwd=cwd,
        headers=headers,
        include=include,
        exclude=exclude,
        rename=rename,
        timeout=timeout,
        description=description,
    )
    if name:
        parent.mount(p, name)
    else:
        if getattr(parent, "_proxy", None) is not None:
            raise ValueError(
                "an extension can mount one unnamed tny.proxy; name the others"
            )
        parent._proxy = p  # type: ignore[attr-defined]
    return p


def reset() -> None:
    """Forget every declaration (tests and host reloads)."""
    _roots.clear()


def roots() -> dict[str, Group]:
    return _roots


def signature_of(fn: Callable[..., Any]) -> inspect.Signature:
    return inspect.signature(fn)
