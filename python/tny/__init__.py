"""Python tool extensions for tny (docs/extensions.md, ADR 0218).

An extension is a Python module in ``~/.tny/extensions/tools/`` or
``<repo>/.tny/extensions/tools/``: a ``NAME.py`` file, or a ``NAME/`` folder
whose entry point is ``NAME/__main__.py``. The module name is the top-level
name of its tools::

    import tny

    @tny.tool
    def call_advisor(question: str) -> str:
        "Ask the advisor one question."
        ...

A module with one tool is called by its module name (``call_advisor``).
Groups nest names: ``jira.projects.list`` in code mode, ``tny jira projects
list`` on the command line. ``tny.proxy(...)`` exposes an existing MCP server's
tools through the same names.

The decorators only attach metadata, so a module still imports and its
functions still run as plain Python outside tny.
"""

from tny._registry import (
    Group,
    Param,
    Proxy,
    Tool,
    ToolError,
    group,
    proxy,
    tool,
)

__all__ = [
    "Group",
    "Param",
    "Proxy",
    "Tool",
    "ToolError",
    "group",
    "proxy",
    "tool",
]

__version__ = "1"
