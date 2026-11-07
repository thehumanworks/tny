# 0218 — Python tool extensions

Date: 2026-11-07
Status: accepted. Supersedes [0027](0027-python-event-hooks.md) and
[0028](0028-extension-parity-contract.md).

## Context

Adding a capability to tny meant changing C. The event-hook extensions of ADR
0027/0028 could observe and steer a turn, but they could not add a tool, and
few users knew how to write one. Agents already compose work in code mode
(`run_code`), and the CLI already runs any native tool headlessly
(`tny tool`, ADR 0217). The missing piece is a low-friction way to add tools
that every surface sees.

## Decision

A tool extension is a Python module of `@tny.tool` functions, in the style of
FastMCP:

- **Where.** `<repo>/.tny/extensions/tools/` (project) and
  `~/.tny/extensions/tools/` (global). A module is `NAME.py` or a folder
  `NAME/` whose entry point is `NAME/__main__.py`. Names starting with `.` or
  `_` are skipped. A project module shadows a global one of the same name and
  `tny tool check` warns about it. Both scopes are trusted and load by default
  (`--no-project-tools`, `--no-tool-extensions`, `TNY_TOOL_EXTENSIONS=0|global`,
  settings `extensions.tools.enabled|project` turn them off).
- **Names.** The module name is the top-level name. A module with exactly one
  root tool and nothing else collapses to the module name (`call_advisor`).
  Otherwise tools are `module.tool`; `tny.group()` and submodules of a folder
  module nest (`jira.projects.list`). The agent calls the dotted name
  (`tools.jira.projects.list(status="open")` or `tools.call(...)`), the CLI is
  `tny jira projects list --status open`, and MCP uses underscores
  (`jira_projects_list`). Native tools and built-in commands always win.
- **Schemas.** Name, description and input schema are optional decorator
  arguments. Otherwise they come from the function name, the docstring
  (Google, NumPy or Sphinx `Args`) and the signature: `Annotated` +
  `tny.Param`, `Literal`, enums, unions, containers, dataclasses, `TypedDict`
  and pydantic models. With pydantic importable, a pydantic model built from
  the signature publishes the schema and validates arguments; without it a
  built-in mapper produces the same shape and checks types.
- **MCP servers.** `tny.proxy(command=[...])` or `tny.proxy(url=...)` exposes an
  existing stdio or Streamable HTTP MCP server through the same names, with
  `include`, `exclude` and `rename`. Configuration is Python; there is no JSON
  file.
- **Runtime.** The `tny` package (`python/tny/`) is embedded in the binary and
  served to the embedded CPython 3.14 from memory. One host process per tny
  process (`tny --tool-host serve`, private) imports the modules and speaks
  newline-delimited MCP plus `tny/catalog`. The host starts lazily, on the
  first call or on a catalog miss, and stops with its owner. A call that times
  out (`timeout=` on the decorator, `TNY_TOOL_TIMEOUT_MS`, default 300 s) or
  is cancelled kills the host's process tree; the next call starts a new one.
- **Catalog cache.** The catalog is cached in
  `~/.tny/cache/tool-extensions/<fingerprint>.json`. The fingerprint covers the
  binary version, the embedded package and the path, size, mtime and inode of
  every module `.py` file. Listing tools from a warm cache does not start
  Python. Catalogs with proxy or host failures are not cached, since those may
  be transient. `tny tool check` always reloads.
- **Integration.** Extension tools join the nested catalog after native and
  registered custom tools, so `tools.search/describe/call`, `tny tool`,
  `tny mcp serve` and the top-level commands all read one list and run
  `tools_execute`. Calls are checked against the published schema, then by
  the host. Permissions use the `tool` category with the dotted name as a path
  (`{"permission":{"tool":{"jira/*":"allow"}}}`; grants key on
  `tool:jira/projects/list`). The default permission mode stays yolo.
- **Results.** A returned string is the result; other values are JSON (dicts
  also become `structuredContent` over MCP). With a `None` return, printed
  text is the result. `tny.ToolError` returns its message as an error; other
  exceptions report `Type: message (file:line)` with the traceback on stderr.
- **wasm.** No tool host: extensions are off and the commands report unknown
  tools.

The event-hook system (`src/core/extensions.c`, `python/tny_ext`, the hook
settings, `--no-extensions`) is removed with no compatibility layer.

## Consequences

Users and agents add a tool with one Python file, and it is immediately
callable from code mode, the shell and any MCP client. A cold call pays one
interpreter start (about 0.2 s); later calls in the same process reuse the
host, and listing from a warm cache costs only a directory walk. One host
serialises calls within a process. Extensions run with the user's authority;
tny mediates the calls (permissions, timeouts, result bounds), not what the
Python does.

Pydantic is used when importable. Bundling pydantic-core into the static
embedded interpreter is follow-up work; the built-in mapper keeps schemas and
validation working without it.

### Editor support (2026-11-07 addendum)

The `tny` Python distribution (sdk/python) re-exports the extension API, and
its `_registry.py`, `_schema.py` and `_mcp.py` are symlinks to `python/tny/`,
so there is one source for the embedded host and for the package editors type
check against. The decorators are typed with overloads over a bound TypeVar,
so a decorated function keeps its signature. A separate stub-only or
`tny-extensions` distribution was rejected: two distributions providing the
`tny` import package would shadow each other. The flake packages it as
`python-tny` / `python3Packages.tny`, and `programs.tny` (Home Manager, NixOS,
nix-darwin) installs it and a Neovim LSP plugin file pointing the server at it.

### Agent skill (2026-11-07 addendum)

`skills/tny-tool-extensions/SKILL.md` teaches an agent to write extensions:
choosing scope and shape, naming rules, one worked example per style (one
tool, several tools, `tny.group`, a folder module, schema options, stdio and
HTTP `tny.proxy`), results and errors, and a verification loop. It is an
ordinary `SKILL.md`, so tny's existing skill discovery finds it in this
repository, and users install it into `~/.tny/skills` by copying the folder,
with `install_skill`, or with `programs.tny.skill` in the Home Manager
module. A built-in skill registry was not added: the existing
discovery and install paths already cover it.
`test_skill_examples_work` runs every non-proxy example through the binary,
so the skill cannot drift from the API.

## Verification

`make test-tool-extensions` (Python unit tests for schemas, binding, naming,
isolation and the proxy client, run with and without pydantic) and
`tests/integration/test_tool_extensions.py` (every surface against the
binary: discovery, flags and help, code mode, `mcp serve`, proxies,
permissions, off switches, cache invalidation, timeout and cancellation).
`sdk/python/tests/typecheck_usage.py` (run with `mypy --strict` in CI) fails if a decorator erases the function type, and
`sdk/python/tests/test_extension_api.py` checks the package's modules are the
embedded ones.
