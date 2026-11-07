# Python tool extensions

A tool extension is a Python module of `@tny.tool` functions. Drop it in an
extension directory and the tool is available everywhere at once:

| Surface | How it is called |
| --- | --- |
| Code mode (`run_code`) | `tools.jira.projects.list(status="open")` or `tools.call("jira.projects.list", json.dumps({...}))`; `tools.search("jira")` finds it |
| CLI | `tny jira projects list --status open`, or `tny tool call jira.projects.list '{"status":"open"}'` |
| MCP | `tny mcp serve` serves it to any MCP client as `jira_projects_list` |

The decision record is [ADR 0218](adr/0218-python-tool-extensions.md).

> **Trust.** Extensions run as you, with your files, network and credentials.
> Project extensions load by default, like the rest of the repository you
> chose to work in. Turn them off with `--no-project-tools`.

## Quick start

```sh
tny tool new call_advisor          # writes .tny/extensions/tools/call_advisor.py
tny tool new jira --folder         # writes jira/__main__.py and jira/notes.py
tny call_advisor "hello" --times 2
```

```python
# ~/.tny/extensions/tools/call_advisor.py
import tny


@tny.tool
def call_advisor(question: str, depth: int = 1) -> str:
    """Ask the advisor one question.

    Args:
        question: What to ask.
        depth: How hard to think, 1 to 3.
    """
    return f"advice on {question}"
```

The name, description and input schema come from the function name, the
docstring and the signature. Pass them explicitly when you want something
else: `@tny.tool(name="ask", description="...", input_schema={...})`.

## Examples and editor support

[examples/extensions/tools](../examples/extensions/tools) holds working
extensions to copy into an extension directory: `weather.py` (one tool) and
`notes/` (a folder module with a group and a private `_store.py` helper).
`tests/integration/test_tool_extensions.py` runs them through the real binary.

Inside tny, `import tny` resolves to the package embedded in the binary. For
completions and type checking in an editor, give the language server the same
code in one of two ways:

- **Install it.** The `tny` Python package ([sdk/python](../sdk/python))
  exports `tool`, `group`, `proxy`, `Param`, `ToolError` and the rest from the
  very modules the binary embeds, with `py.typed`. Install it into the
  environment your editor uses: `pip install -e <tny checkout>/sdk/python`.
- **Point at a checkout.** Without installing anything, add the checkout's
  `python/` directory to the server's import paths, for example a
  `pyrightconfig.json` next to your extensions
  ([example](../examples/extensions/editor/pyrightconfig.json)), or the
  basedpyright/pyright settings for Neovim in
  [nvim.lua](../examples/extensions/editor/nvim.lua).

With Nix, `programs.tny.python.enable` and `programs.tny.neovim.enable` in the
flake's Home Manager, NixOS or nix-darwin module do both for you
([nix.md](nix.md#modules-programstny)).

Decorators keep the function's own type, so `@tny.tool def f(x: int) -> str`
still reads as `(x: int) -> str` and wrong arguments are flagged. The tool host
runs CPython 3.14; set the checker's Python version to match.

## Where extensions live

| Directory | Scope |
| --- | --- |
| `<repo>/.tny/extensions/tools/` | Project: shared with the repository |
| `~/.tny/extensions/tools/` | Global: yours, in every workspace |

A module is either a file `NAME.py` or a folder `NAME/` whose entry point is
`NAME/__main__.py`. Names starting with `.` or `_` are ignored, so `_helpers.py`
is a safe place for shared code. When both directories define the same name,
the project module wins and `tny tool check` reports the shadowing.

## Names and groups

The module name is the top-level name. How the tools under it are named:

- A module with exactly one tool, and no groups, is called by its module name:
  `call_advisor.py` with one tool gives `call_advisor`.
- Otherwise each tool is `MODULE.TOOL`: `jira.py` with `whoami` and `search`
  gives `jira.whoami` and `jira.search`.
- `tny.group("projects")` nests a group, and so does a submodule of a folder
  module: `jira/projects.py` imported from `jira/__main__.py` gives
  `jira.projects.list`.

```python
# jira/__main__.py
import tny
from . import projects  # tools in jira/projects.py nest under jira.projects


@tny.tool
def whoami() -> str:
    "The signed-in user."
    ...
```

```python
# jira/projects.py
import os
import tny


@tny.tool(name="list")  # `list` is a builtin, so name it explicitly
def list_projects(status: str = "open") -> list[dict]:
    "List projects."
    token = os.environ["JIRA_TOKEN"]  # secrets come from the environment
    ...
```

Native tools (`read_file`, `terminal`, ...) and built-in commands (`tny ask`,
`tny status`, ...) always win over an extension of the same name.

## Schemas and validation

Supported annotations: `str`, `int`, `float`, `bool`, `None`, `list[...]`,
`set[...]`, `tuple[...]`, `dict[str, ...]`, unions and `Optional`, `Literal`,
`enum.Enum`, dataclasses, `TypedDict`, pydantic models, `datetime`, `date`,
`uuid.UUID` and `pathlib.Path`. Parameters without a default are required.

Describe a parameter in the docstring (`Args:` in Google style, NumPy
`Parameters`, or Sphinx `:param x:`), or inline:

```python
from typing import Annotated
import tny


@tny.tool
def create(key: Annotated[str, tny.Param(description="Project key", examples=("TNY",))]) -> str:
    ...
```

Arguments are validated before your function runs, and converted to the
annotated types (an enum member, a dataclass instance, a pydantic model). When
pydantic is importable, a pydantic model built from the signature does both
the schema and the validation; otherwise a built-in mapper produces the same
schema shape and checks types. A `**kwargs` parameter allows extra arguments.

## Results and errors

- A `str` return is the result. Any other value is returned as JSON, and a
  `dict` is also MCP `structuredContent`.
- With no return value, whatever the tool printed is the result. When a value
  is returned, printed text goes to stderr instead.
- `raise tny.ToolError("message")` returns `message` as a tool error.
- Any other exception returns `Type: message (file.py:line)` as an error, with
  the traceback on stderr.
- `async def` tools are run to completion.
- `@tny.tool(timeout=600)` raises the time limit for one tool (seconds). The
  default is 300 seconds, or `TNY_TOOL_TIMEOUT_MS`.

In the CLI, stderr is your terminal. Inside agent turns it is appended to
`~/.tny/logs/tool-extensions.log`.

## Exposing an existing MCP server

```python
# github.py — the server's tools appear as github.create_issue, ...
import os
import tny

tny.proxy(
    ["github-mcp-server", "stdio"],
    env={"GITHUB_TOKEN": os.environ["GITHUB_TOKEN"]},
    exclude=["delete_repository"],
)
```

`tny.proxy(command=[...])` starts a stdio server; `tny.proxy(url="https://...",
headers={...})` uses Streamable HTTP. `include=` and `exclude=` filter remote
tool names and `rename={"remote": "local"}` renames them. Without `name=`, the
tools sit directly under the module; `tny.proxy(..., name="gh")` nests them
one level deeper, and a module can mix proxies with its own `@tny.tool`s. The
server is contacted when the catalog is rebuilt and on calls; keep secrets in
the environment, not in the file.

## The command line

```sh
tny jira --help                           # the module's commands
tny jira projects list --help             # one tool's options, from its schema
tny jira projects list --status done --tag a --tag b --archived
tny jira projects create TNY              # required parameters can be positional
echo '{"status":"done"}' | tny jira projects list -   # JSON object on stdin
tny jira whoami --json                    # {"kind":"tool","name":...,"ok":...,"result":...}
```

Options map to parameters: `--max-results` sets `max_results`; booleans take
`--flag` / `--no-flag`; list parameters repeat (`--tag a --tag b`) or take a
JSON array; object parameters take JSON. Flags override fields read from
stdin. Exit codes match `tny tool call`: 0 ok, 1 usage error, 2 tool error,
130 interrupted.

Other commands:

- `tny tool list` and `tny tool describe NAME` include extension tools.
- `tny tool check [--json]` reloads every extension and prints its tools,
  warnings and import errors with tracebacks; it exits 1 when a module fails.
- `tny tool new NAME [--folder] [--global]` writes a starter module:
  `NAME.py`, or with `--folder` a `NAME/__main__.py` entry point that imports a
  sibling `NAME/notes.py` (tools `NAME.hello` and `NAME.notes.add`).
- `tny mcp serve [--all]` serves the extension tools over stdio MCP, for
  example to another agent harness. `--all` adds tny's native tools.

## Code mode

Agents see extension tools in the nested catalog of `run_code`:

```python
print(tools.search("jira"))                       # names and descriptions
print(tools.describe("jira.projects.list"))       # the full schema
print(tools.jira.projects.list(status="open"))    # attribute form, keyword arguments
print(tools.call("call_advisor", json.dumps({"question": "why?"})))
```

Every call goes through the same permission engine, result bounds and
cancellation as native tools.

## Permissions

Extension tools use the `tool` permission category, with the dotted name
written as a path. The default mode is yolo; in `ask` or `auto` mode, rules
apply and the last matching rule wins:

```json
{"permission": {"tool": {"jira/*": "allow", "jira/projects/delete": "deny"}}}
```

A session grant ("don't ask again") covers one tool, keyed
`tool:jira/projects/list`.

## Turning extensions off

| Switch | Effect |
| --- | --- |
| `--no-tool-extensions`, `TNY_TOOL_EXTENSIONS=0` | No extensions at all |
| `--no-project-tools`, `TNY_TOOL_EXTENSIONS=global` | Only `~/.tny/extensions/tools/` |
| settings `{"extensions":{"tools":{"enabled":false}}}` | No extensions at all |
| settings `{"extensions":{"tools":{"project":false}}}` | Only global extensions |

Extensions are also off in library (`libtny`) embeds, during prompt
optimisation, and on wasm, where no tool host can run.

## How it works

The `tny` package is built into the binary. The first time a process needs
an extension tool, it starts one tool host, `tny --tool-host serve`, which
imports the modules in the embedded CPython 3.14 and answers MCP requests on a
pipe. The host lives as long as its tny process. A timed-out or cancelled call
kills the host and the next call starts a fresh one.

The catalog is cached in `~/.tny/cache/tool-extensions/`, keyed by a
fingerprint of every module file (path, size, mtime, inode) and the tny
version, so listing tools does not start Python until a file changes.
Proxy listings are re-read only when files change or on `tny tool check`;
catalogs with proxy failures are never cached.

The decorators only attach metadata, so a module still imports and its
functions run as plain Python outside tny. To unit test an extension, put
tny's `python/` directory on `sys.path` (it holds the same `tny` package that
is built into the binary) and call the functions directly.
