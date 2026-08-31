# 0051 — Remote MCP is POST-only Streamable HTTP; SSE is unsupported

Date: 2026-08-31
Status: accepted (amends the MCP transport row in
[parity-with-fx.md](../features/parity-with-fx.md); does not change
[0049](0049-mcp-background-warmup.md)'s warm-up, catalog, or
`mcp_select_tool` chokepoint)

## Context

tny's MCP client spoke only stdio JSONL. The product contract
([mcp-and-skills.md](../features/mcp-and-skills.md)) already named Streamable
HTTP and `header_env` / `bearer_token_env`, and fx's trusted profile has
`"type":"http"` entries. Hosted MCP servers (Notion, Prisma, Supabase, …)
are HTTP, not a local command. Leaving HTTP deferred meant a populated
`~/.tny/mcp.json` still started blind for every remote server.

The 2026-07-28 Streamable HTTP binding is one POST per JSON-RPC message and
permits either `application/json` or a request-scoped SSE stream. GET, DELETE,
`Last-Event-ID`, and protocol-level sessions are gone from that revision.
Legacy `2025-06-18` still uses `initialize` plus optional `Mcp-Session-Id`.
The deprecated 2024-11-05 HTTP+SSE transport opens with GET and an
`endpoint` event. Issue #87 explicitly excludes both kinds of SSE: supporting
them would add a long-lived event protocol and fallback path that the native
tool loop does not need.

OAuth (PKCE, DCR, Keychain) is a separate interactive flow. Static header
and bearer-from-env cover the hosted servers users actually paste today.

## Decision

**`~/.tny/mcp.json` `"type":"http"` entries speak Streamable HTTP over the
existing `http_conn` seam. One POST per message. Responses must be one JSON
document; SSE fails actionably and GET is never issued. Auth is `headers` / `header_env` /
`bearer_token_env` only.**

Mechanics (`src/mcp/mcp_http.c`, profile parsing in `src/mcp/mcp.c`):

- Profile: `"type":"http"` plus `url` (`http://` or `https://`). Optional
  `headers` (non-secret), `header_env` (header name → env var),
  `bearer_token_env`. A literal `Authorization` value is rejected so
  credentials never become ordinary profile data. Transport-owned names
  (`Host`, `Content-Type`, `Accept`, `MCP-Protocol-Version`, `Mcp-Method`,
  `Mcp-Name`, `Mcp-Session-Id`) cannot be overridden.
- Each RPC is a new `http_open` + POST to the URL path with
  `Accept: application/json`. Modern era adds
  `MCP-Protocol-Version: 2026-07-28`, `Mcp-Method`, optional `Mcp-Name`
  (Base64 sentinel when the value is not plain ASCII), and per-request
  `_meta.io.modelcontextprotocol/*` in the JSON-RPC params.
- A `text/event-stream` body is rejected immediately with a clear error that
  asks for `application/json` over Streamable HTTP POST. It is never parsed as
  JSON-RPC. `application/json` is accumulated after HTTP chunk decoding and
  parsed as a single object.
- Handshake: POST `server/discover`. If the result lists `2026-07-28`, the
  connection stays modern and never sends `initialize`. A recognized modern
  JSON-RPC error (`-32020`…`-32022`) fails closed. Any other 4xx/empty body
  falls back to legacy `initialize` + `notifications/initialized` and
  stores `Mcp-Session-Id` for later POSTs. HTTP 404/405 on that fallback
  is reported as unsupported HTTP+SSE — we do not GET.
- Warm-up, catalog, search, `mcp_select_tool`, silent-until-named failure,
  and wasm laziness are unchanged from ADR 0049. HTTP uses `http_conn`, so
  wasm fetch works; stdio spawn still fails cleanly without threads.
- Errors never echo configured header values, tokens, or the URL query.

Out of scope: OAuth, `x-mcp-header` param mirroring, `subscriptions/listen`,
MRTR/elicitation, request-scoped SSE, and the deprecated HTTP+SSE GET transport.

## Consequences

- Hosted MCP servers in the trusted profile work the same way stdio servers
  already do: background warm-up, catalog in the system prompt, calls
  through `mcp_select_tool`.
- Binary size stays inside the existing HTTP client; no new library.
- wasm: HTTP MCP is remote-only over fetch; stdio stays a clean spawn
  error. Documented on the MCP page.
- Unit coverage: loopback HTTP mock in `tests/test_mcp.c` (JSON, actionable
  SSE rejection, legacy fallback, env auth, rejected literal Authorization,
  silent 401).
  Default CI remains fixture-only; no live MCP hosts.

## Sources

- https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http
- https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning
- https://modelcontextprotocol.io/specification/2026-07-28/server/discover
- https://modelcontextprotocol.io/specification/2025-06-18/basic/transports
- https://fx.sh/docs/capabilities/mcp.md
