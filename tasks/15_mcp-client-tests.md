# 15 — Test the stdio MCP client

High. From the test-depth review. Independent of 16–19 (same native-loop
gap class, different modules).

`src/mcp/mcp.c` is a stdio JSON-RPC client: spawn, 30s timeout, line
buffering, `tools/list` cache, `tools/call`. Docs
(`docs/features/mcp-and-skills.md`) treat MCP as a native-loop feature.
The only test hit is permission identity for `mcp_select_tool` in
`tests/test_core.c`. There is no fake server, no `mcp.json` fixture, no
search/select/call round-trip, and no “repo-local MCP must not load”
test. `rpc()` interpolates the method name into JSON unescaped.

## Work

- Add a fake stdio MCP server (JSONL on stdin/stdout) under `tests/`
  that implements `initialize`, `tools/list`, and `tools/call`, and can
  emit split/partial lines, notifications, and a mismatched `id`.
- Cover `mcp_features`, `mcp_search_tools`, and `mcp_call_tool` against
  a trusted `~/.tny/mcp.json` (or `$TNY_HOME`) pointing at that fake
  command. Assert spawn, list cache, call args, timeout, and process
  teardown via `mcp_shutdown_all`.
- Assert repo-local files (`./mcp.json`, `.mcp.json`, project
  `.cursor/mcp.json` and similar) are never read or spawned, even when
  present in the workspace.
- Add a case that a method/name with JSON metacharacters cannot break
  the request frame (escape in `rpc()`, or reject).
- State wasm behavior on the MCP docs page (works / remote-only /
  clean error) if it is not already explicit; keep the suite fixture-
  only (no live MCP hosts).

## Acceptance

- `make test` fails if `tools/call` against the fake server is broken,
  if a workspace `mcp.json` is loaded, or if an unescaped quote in the
  method name is sent raw.
- Unrelated suites (cursor MCP *event* mapping, permission subject
  strings) stay green and are not treated as coverage for this client.
- Default CI remains fixture-only; no network MCP servers.
