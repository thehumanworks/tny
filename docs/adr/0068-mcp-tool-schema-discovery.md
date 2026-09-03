# 0068 — MCP tool schema discovery from the shell: `tny mcp tools`, `tny mcp describe`, and the schema-on-failure hint

Date: 2026-09-03
Status: accepted (extends [ADR 0057](0057-shell-first-native-loop.md) and
[ADR 0063](0063-in-process-intercept-of-first-party-verbs.md))

## Context

Under the shell tool profiles (ADR 0062) the model reaches MCP through
`tny mcp call SERVER/TOOL` with JSON on stdin. The system prompt lists each
tool's namespaced name and one-line description, and nothing else: `tny mcp
list` prints servers, not tools, and no verb prints a tool's `inputSchema`
even though the client caches the whole `tools/list` response. A model's
first call to a tool was therefore a guess at argument names, and a model
reported exactly that failure.

Three fixes were weighed:

1. **A describe verb** — print the cached schema on request and tell the
   prompt to ask before the first call.
2. **Schemas in the prompt** — inject every tool's schema into the shell
   profile prompt. Zero extra calls, but the prompt grows with every server,
   schemas land late on pre-warm, and it reinstates the per-tool
   advertisement the shell-first direction removed.
3. **Validate and teach on failure** — check stdin against the schema before
   `tools/call`, or print the schema when the server rejects the call. No
   new verb, but the first attempt still fails by design, and a JSON Schema
   validator in C is a new dependency that will always be partial.

## Decision

1. **Two catalog verbs.** `tny mcp tools SERVER` prints one line per tool
   (`server/tool — description`) with an indented `arguments:` summary
   (`name* (type)`, `*` = required; `none`; or `unknown (no input schema
   published)`). `tny mcp describe SERVER/TOOL` prints the same for one tool
   plus the full `input schema:` JSON. `--json` emits
   `{"kind":"mcp_tools",…,"tools":[{"name","description","input_schema"}]}` or
   `{"kind":"mcp_tool",…,"input_schema":…}` with the schema verbatim
   (`null` when absent). Neither calls the tool. Exit codes follow `call`:
   0; 1 usage or unknown server; 2 a tool the server does not list.
2. **Answered in-process inside a session.** Typed into `terminal`, both
   verbs are intercepted like `mcp call` (ADR 0063) and served by the warmed
   client, so the round trip costs no server start. Their permission
   identity is `mcp_search_tools`, the native meta-tool they stand for, so
   one ask-mode rule covers the shell verbs and the typed tool alike. The
   one-shot CLI checks the same identity, then cold-starts and shuts down.
3. **The prompt asks for it.** The shell-profile discipline paragraph now
   says: run `tny mcp describe SERVER/TOOL` before the first call to a tool
   and shape the JSON from its input schema; never guess argument names. The
   MCP catalog header under shell profiles names `tny mcp call` and `tny mcp
   describe` instead of `mcp_select_tool`, and the overflow line points at
   `tny mcp tools SERVER` instead of `mcp_search_tools`.
4. **Schema on failure, no validator.** When `tools/call` comes back as a
   JSON-RPC error or `isError: true` and the server lists the tool, `tny mcp
   call` (CLI and intercept) prints one more stderr line, `input schema for
   SERVER/TOOL: {…}`, and the `--json` failure object gains `input_schema`.
   The schema comes from the already-connected server's cache — never a new
   connection — so success, a permission refusal, a config error, and a tool
   the server never listed carry no hint. tny does not validate arguments
   client-side; the server's own error is the verdict and the schema is the
   correction.

Option 1 was chosen because it is the only shape that prevents the failure
without growing every prompt, it is equally useful to humans and scripts
(the shell-first bar), and its cost is one small, cached read. The cheap half
of option 3 rides along because the schema is already in memory and it
catches the case where the model skips `describe`. Option 2 is rejected as
fighting ADR 0057.

## Consequences

- One more intercept kind (`TNY_INTERCEPT_MCP_DESCRIBE`) and three MCP
  client entry points: `mcp_describe`, `mcp_describe_cli`, `mcp_tool_schema`.
- `call_fail` output changes only on tool-level failures, and only when a
  schema exists; existing `mcp_call` consumers see an additional field, never
  a changed one.
- A tool's first shell call now costs one extra intercepted round trip when
  the model follows the prompt. That is the trade against guessing.
- wasm: pure rendering of cached JSON; HTTP servers work remote-only and
  stdio keeps the clean spawn error, as for `call`.
- Tests: `tests/test_mcp.c` (listing, describe text and JSON, unknown tool,
  unknown server, schema hint present on failure and absent on success /
  unlisted tool / config error), `tests/test_intercept.c` (recognised and
  rejected shapes, identity, label), `tests/integration/test_mcp_call.py`
  (the same over the real binary).
