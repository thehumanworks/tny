---
name: tny-tool-extensions
description: Write, test and debug tny Python tool extensions (@tny.tool, tny.group, tny.proxy, folder modules, schemas). Use when asked to add a tool, command or MCP proxy to tny.
---

# Writing tny tool extensions

A tool extension is a Python module of `@tny.tool` functions. Once the file is
in an extension directory, each tool is callable everywhere with no restart
and no registration step:

- **CLI:** `tny MODULE [GROUP...] TOOL --flag value`, or `tny tool call NAME '{json}'`.
- **Code mode:** `tools.MODULE.TOOL(arg=...)` or `tools.call("MODULE.TOOL", json)`.
- **MCP:** `tny mcp serve` serves the tools to other agents.

Full reference: `docs/extensions.md` in the tny repository (ADR 0218).

## Workflow

1. **Pick the scope.**
   - Project: `<repo>/.tny/extensions/tools/`, shared with the repo. This is the default.
   - Global: `~/.tny/extensions/tools/`, for every workspace. Use it only when the user asks for a personal tool.
2. **Pick the shape.** Use the table below.
3. **Scaffold** with `tny tool new NAME [--folder] [--global]` or write the file directly. `NAME` must be a Python identifier that is not a tny command (`ask`, `tool`, `status`, ...).
4. **Verify.** Run `tny tool check`, which must exit 0 and list your tools. Then call each tool once from the CLI. Then run `tny MODULE --help`.
5. **Tell the user** the exact command to run, for example `tny jira projects list --status open`.

| Need | Shape | Resulting names |
| --- | --- | --- |
| One action | `NAME.py` with one `@tny.tool` | `NAME` |
| A few related actions | `NAME.py` with several tools | `NAME.a`, `NAME.b` |
| Sub-commands in one file | `NAME.py` plus `tny.group("sub")` | `NAME.sub.x` |
| Many tools, shared helpers | `NAME/__main__.py` plus submodules | `NAME.x`, `NAME.sub.y` |
| Wrap an existing MCP server | `NAME.py` with `tny.proxy(...)` | `NAME.<remote tool>` |

### Naming rules

- The module (file or folder) name is the top-level name.
- If a module has exactly one tool and no groups, that tool is called by the module name, whatever the function is called.
- The tool name is the function name with leading and trailing `_` removed, or `name=`. Use `name="list"` rather than shadowing the `list` builtin.
- Names use letters, digits, `_` and `-`, start with a letter or `_`, and are at most 64 characters.
- Files and folders starting with `_` or `.` are never loaded as extensions. Put shared code in `_client.py`.
- Native tools (`read_file`, `terminal`, ...) and built-in commands always win over an extension with the same name. A project module shadows a global one with the same name, and `tny tool check` reports it.

## Example 1: one tool

```python
# .tny/extensions/tools/call_advisor.py
import tny


@tny.tool
def call_advisor(question: str, depth: int = 1) -> str:
    """Ask the advisor one question.

    Args:
        question: What to ask.
        depth: How hard to think, 1 to 3.
    """
    if not 1 <= depth <= 3:
        raise tny.ToolError("depth must be 1, 2 or 3")
    return f"advice on {question} (depth {depth})"
```

Call it with `tny call_advisor "why?" --depth 2`, or as `tools.call_advisor(question="why?")` in code mode.

- The docstring's first paragraph is the description the model sees, and `Args:` describes each parameter. Google, NumPy and Sphinx styles all work.
- A parameter without a default is required, and the CLI also accepts it positionally.

## Example 2: several tools in one file

```python
# .tny/extensions/tools/deploy.py
"""Deployment helpers: deploy.status, deploy.start, deploy.wait."""

import asyncio
import enum
from typing import Literal

import tny


class Env(enum.Enum):
    STAGING = "staging"
    PROD = "prod"


@tny.tool
def status(env: Env = Env.STAGING) -> dict[str, str]:
    """Current deployment status for an environment."""
    return {"env": env.value, "state": "healthy"}  # a dict is returned as JSON


@tny.tool(timeout=900)  # seconds; the default limit is 300
def start(
    ref: str, env: Env, strategy: Literal["rolling", "blue-green"] = "rolling"
) -> str:
    """Deploy a git ref.

    Args:
        ref: Branch, tag or commit to deploy.
        env: Target environment.
        strategy: How to roll it out.
    """
    if env is Env.PROD and ref.startswith("wip"):
        raise tny.ToolError("refusing to deploy a wip ref to prod")
    return f"deploying {ref} to {env.value} ({strategy})"


@tny.tool
async def wait(seconds: float = 0.1) -> str:
    """Wait for the rollout to settle."""
    await asyncio.sleep(seconds)  # async tools are run to completion
    return "settled"
```

- Arguments are validated and converted before your function runs, so `env` arrives as an `Env` member. Bad input never reaches your code.
- On the CLI, enums and `Literal` become choices: `tny deploy start v1.2 --env prod --strategy blue-green`.

## Example 3: groups in one file

```python
# .tny/extensions/tools/issues.py
import tny

labels = tny.group("labels", "Manage issue labels")


@tny.tool(name="list")
def list_issues(state: str = "open", limit: int = 20) -> list[dict[str, object]]:
    """List issues."""
    return [{"id": 1, "state": state}][:limit]


@labels.tool
def add(issue: int, label: str) -> str:
    """Add a label to an issue."""
    return f"#{issue} +{label}"


@labels.tool(name="remove")
def remove_label(issue: int, label: str) -> str:
    """Remove a label from an issue."""
    return f"#{issue} -{label}"
```

This gives `issues.list`, `issues.labels.add` and `issues.labels.remove`, and the CLI runs `tny issues labels add 7 bug`. A group can nest further with `labels.group("colors")`.

## Example 4: a folder module

Use a folder when the tools share a client, configuration or helpers. The entry point is `__main__.py`.

- Submodules that `__main__.py` imports become groups named after the file.
- Use relative imports.
- Read secrets from the environment, never from the file.

```python
# .tny/extensions/tools/jira/__main__.py
"""Jira tools."""

import tny

from . import projects  # noqa: F401  (registers jira.projects.*)
from ._client import client


@tny.tool
def whoami() -> str:
    """The signed-in Jira user."""
    return client().user
```

```python
# .tny/extensions/tools/jira/projects.py
import tny

from ._client import client


@tny.tool(name="list")
def list_projects(status: str = "open") -> list[str]:
    """List project keys."""
    return [p for p in client().projects if status == "open"]


@tny.tool
def create(key: str, title: str = "") -> str:
    """Create a project."""
    return f"created {key} {title}".strip()
```

```python
# .tny/extensions/tools/jira/_client.py
"""Shared helper: the leading underscore keeps it out of the catalog."""

import os
from dataclasses import dataclass, field


@dataclass
class Client:
    user: str
    projects: list[str] = field(default_factory=lambda: ["TNY", "OPS"])


def client() -> Client:
    return Client(user=os.environ.get("JIRA_USER", "ada"))
```

This gives `jira.whoami`, `jira.projects.list` and `jira.projects.create`. To scaffold the same layout, run `tny tool new jira --folder`, which writes `__main__.py` and a sibling `notes.py`.

## Example 5: schemas and descriptions

Supported annotations:

- `str`, `int`, `float`, `bool`, `None`
- `list[...]`, `set[...]`, `tuple[...]`, `dict[str, ...]`
- unions and `Optional`, `Literal`, `enum.Enum`
- dataclasses, `TypedDict`, pydantic models
- `datetime`, `date`, `uuid.UUID`, `pathlib.Path`

```python
# .tny/extensions/tools/catalog.py
from dataclasses import dataclass
from typing import Annotated, Any, TypedDict

import tny


@dataclass
class Item:
    sku: str
    quantity: int = 1


class Filters(TypedDict, total=False):
    color: str
    max_price: float


@tny.tool
def order(items: list[Item], note: str | None = None) -> dict[str, Any]:
    """Place an order. Items arrive as Item instances."""
    return {
        "skus": [i.sku for i in items],
        "count": sum(i.quantity for i in items),
        "note": note,
    }


@tny.tool
def search(
    query: Annotated[str, tny.Param(description="Free text", examples=("red shoes",))],
    limit: Annotated[int, tny.Param(json_schema={"minimum": 1, "maximum": 50})] = 10,
    filters: Filters | None = None,
) -> list[str]:
    """Search the catalog."""
    return [f"{query}:{n}" for n in range(min(limit, 3))]


@tny.tool(
    name="raw",
    description="Echo the arguments back. The schema below replaces the inferred one.",
    input_schema={
        "type": "object",
        "properties": {"payload": {"type": "object"}},
        "required": ["payload"],
    },
)
def raw_tool(**kwargs: Any) -> dict[str, Any]:
    return kwargs  # **kwargs accepts arguments beyond the signature
```

- `tny.Param(description=, examples=, json_schema=, name=)` adds metadata to one parameter. `json_schema` merges keys such as `minimum` or `pattern` into it.
- `@tny.tool(name=, description=, input_schema=, timeout=)` overrides the inferred values. An explicit `input_schema` is published as is, so validate inside the function.
- When pydantic is importable it builds the schema and validates; otherwise a built-in mapper produces the same shape.
- The tool host runs CPython 3.14, so modern syntax (`X | None`, `list[int]`) is fine.

## Example 6: proxy an existing MCP server

```python
# .tny/extensions/tools/github.py
"""github.create_issue, github.search_code, ... from the GitHub MCP server."""

import os

import tny

tny.proxy(
    ["github-mcp-server", "stdio"],  # a stdio server
    env={"GITHUB_TOKEN": os.environ.get("GITHUB_TOKEN", "")},
    exclude=["delete_repository"],  # or include=[...]
    rename={"search_code": "grep"},  # remote name -> local name
)
```

```python
# .tny/extensions/tools/linear.py
"""Mix a remote server (nested as linear.api.*) with local tools."""

import os

import tny

tny.proxy(
    url="https://mcp.linear.app/mcp",  # Streamable HTTP
    headers={"Authorization": f"Bearer {os.environ.get('LINEAR_TOKEN', '')}"},
    name="api",  # nest under linear.api
    timeout=60,
)


@tny.tool
def triage_url(team: str) -> str:
    """Link to a team's triage view."""
    return f"https://linear.app/team/{team}/triage"
```

- Without `name=`, remote tools sit directly under the module, and a module may hold only one unnamed proxy. Name the others.
- tny contacts the server when the catalog is rebuilt and on each call.
- A server that fails shows up in `tny tool check` as a warning, and the rest of the catalog still loads.

## Results and errors

- A `str` return value is the result as is. Any other value is returned as JSON, and a `dict` is also MCP `structuredContent`.
- With no return value, whatever the tool printed is the result. When a value is returned, printed text goes to stderr: on the terminal in the CLI, and in `~/.tny/logs/tool-extensions.log` during agent turns.
- `raise tny.ToolError("message")` returns a clean error with that text. Any other exception returns `Type: message (file.py:line)`.
- CLI exit codes are 0 for ok, 1 for a usage error, 2 for a tool error, and 130 when interrupted.

## Verify before you finish

```sh
tny tool check                      # every module imports; lists tools, warnings, tracebacks
tny tool list | grep jira           # the names you expect
tny tool describe jira.projects.list
tny jira --help                     # the commands, from the schemas
tny jira projects list --status open
echo '{"status":"open"}' | tny jira projects list -
tny tool call jira.projects.create '{"key":"TNY"}' --json
```

- `tny tool check` exits 1 when any module fails to import, and prints the traceback. Fix that first.
- The catalog is cached by file fingerprint, so editing a file is enough. There is nothing to reload.
- To unit test, call the functions directly. The decorators return the original function, so `call_advisor("q")` works in plain Python with tny's `python/` directory or the `tny` package on `sys.path`.

## Pitfalls

- **Import time matters.** A module is imported whenever the catalog is rebuilt. Keep network calls and heavy setup inside the tool functions, not at module top level. `tny.proxy` is the exception: it only declares the server.
- **Secrets.** Read them with `os.environ`. Never write tokens into the extension file; project extensions are committed.
- **Keep the description short.** The model sees it; one plain sentence per tool works best.
- **Trust.** Extensions run as the user with full access. Project extensions load by default. `--no-project-tools` or `TNY_TOOL_EXTENSIONS=global` limits loading to global ones, and `--no-tool-extensions` turns them all off.
- **Permissions.** Calls use the `tool` category with the dotted name as a path, for example `{"permission":{"tool":{"jira/*":"allow"}}}`. These rules only apply in `ask` or `auto` mode, since the default is yolo.
- **Unavailable runtimes.** Extensions don't run on wasm, in `libtny` embeds or during prompt optimisation.
- **Editor support.** For completions and type checking, install the `tny` Python package (`sdk/python`) or add tny's `python/` directory to the language server's extra paths. With Nix, `programs.tny.python.enable` and `programs.tny.neovim.enable` do this for you.
