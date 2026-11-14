# 0223 — SDK Toolkit native tool calls

Date: 2026-11-14
Status: accepted

## Context

ADR 0217 made every native tool reachable from scripts through `tny tool`.
SDK users still had no way to call a tool without starting an agent turn.
There were three options: run the tools in process through libtny, shell out
to the `tny` binary, or do both. Tomas chose in-process (2026-11-14), which
keeps the existing Toolkit contract that no `tny` executable is needed.

## Decision

Add two version-1 toolkit operations to the existing `tny_toolkit_job_*` ABI.
They add no new symbols and change no records:

- `list_tools` → `{"tools":[{name, description, parameters}]}`
- `call_tool` `{name, arguments?, permission_mode?}` → `{name, ok, result}`

Both build the toolkit's explicit context, which keeps `library_mode`. They
then run `tools_catalog_json` / `tools_execute` with a fresh permission engine.
The catalog and the refusals are therefore exactly those of embedded runtimes
(ADR 0215):

- File, search and web tools work.
- Process-owning tools (`terminal`, `subagent`, jobs, teams) and ambient-state
  tools (`memory`, skills, MCP, Python tool extensions) are absent. Calling
  them returns an unavailable result.
- Turn-bound tools are hidden and refused through the same `tools_turn_bound`
  predicate as `tny tool`.

A tool outcome, including an error, is data with status OK, not an exception.
The text is local tool output, which callers need in order to act on it.

The permission mode comes from the request first, then the settings file
(workspace, then global), then yolo. A direct call names the tool and its
arguments, which is the decision a reviewer would otherwise make. The
permission rules still apply in `ask` and `auto`, and calls with no reviewer
fail closed.

Python exposes `Toolkit.list_tools` / `call_tool` (also on `AsyncToolkit`),
returning `ToolSpec` and `ToolCallResult`. The name `ToolResult` already
belongs to custom-tool callbacks. TypeScript exposes `listTools` / `callTool`
with frozen results, and copies the arguments with `structuredClone`.

## Consequences

SDK scripts can read, search, edit and fetch through tny's own tools, with its
path roots, bounds and permission rules, and need no binary. Tools that need a
process launcher or ambient user state remain CLI-only (`tny tool`). If SDK
callers ask for `memory`, skills or `terminal`, that will be a separate
decision about embedded state and process ownership. Libraries older than this
release reject the new operations as invalid requests.
