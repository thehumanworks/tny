You are the implementing agent. Work directly in the current directory with
your own tools (read/edit files, run shell commands). Do NOT launch `tny`,
nested agents, or detached sessions — do the work yourself, in this turn.

## Goal

Implement GitHub issue #87 for the tny repo you are in: extend the MCP client
(`src/mcp/`) to support **remote MCP servers** over streamable HTTP and
stateless MCP v2. **Do not implement SSE** — SSE-only servers must get a
clean, actionable error.

Read `CLAUDE.md`, `docs/architecture.md`, `docs/settings.md`, the existing
`src/mcp/` and `src/net/` code, and relevant ADRs in `docs/adr/` before
writing code.

## Issue body (authoritative spec)

## Context

The MCP client (`src/mcp/`) currently speaks **stdio JSONL only**, configured via the trusted profile `~/.tny/mcp.json`. Remote MCP servers cannot be used without a local stdio proxy.

## Goal

Extend the MCP client to connect to remote servers over HTTP:

- **Streamable HTTP** transport (the current MCP spec transport): POST JSON-RPC to the endpoint, handle both single-JSON responses and chunked streaming responses on the same request, honor `Mcp-Session-Id`.
- **MCP v2 stateless** mode: request/response with no session establishment, for servers that advertise it.
- **Explicitly out of scope: SSE.** The deprecated HTTP+SSE transport will not be supported — no `GET`-based event stream, no fallback. Servers that only offer SSE get a clean, actionable error.

## Design notes / constraints

- Config: extend `~/.tny/mcp.json` entries with a transport discriminator, e.g. `{"type": "http", "url": "...", "headers": {...}}` alongside the existing stdio shape. Keep existing configs working unchanged.
- Reuse the existing HTTP stack in `src/net/` (picohttpparser); no new vendored deps if avoidable. C11 only, size budget applies (stripped `tny` < 2.0 MiB).
- Streaming parsing must survive arbitrary split boundaries (see `chunked_survives_every_split_boundary` in `tests/test_net.c`) — protocol mocks send whole frames, real transports don't.
- Blocking waits go through `tny_poll`, never raw `poll(2)` (wasm constraint). State the **wasm behavior** (works / remote-only / clean error) in the MCP docs page per ADR 0017.
- Auth headers are secrets: never log them; follow the existing no-secrets-in-repo rules.
- Native loop owns MCP (invariant) — host backends are unaffected.
- Record the transport decision (and the SSE non-support) in a new ADR.

## Acceptance criteria

- [ ] Remote server configured with `type: http` connects, lists tools, and executes tool calls via streamable HTTP.
- [ ] Stateless MCP v2 servers work without session round-trips.
- [ ] SSE-only servers produce a clear error naming the unsupported transport.
- [ ] Integration fixtures + split-boundary streaming tests under `tests/`; `nix/source.nix` / `nix/tests.nix` updated for any new fixtures.
- [ ] Docs updated (MCP section + new ADR); `make quality` and `make test` pass.

Related: importing MCP server configs from other agent harnesses is tracked separately.


## Boundaries

- You MAY edit any file in this worktree. Do NOT commit — leave all changes
  uncommitted in the working tree.
- C11 only; reuse `src/net/` (picohttpparser); no new vendored dependencies
  unless truly unavoidable (and then pinned under `third_party/` with a
  VERSION file and a docs note).
- Keep existing stdio MCP configs working byte-for-byte.
- Blocking waits via `tny_poll`, never raw poll(2). State the wasm behavior
  of the new transport in the MCP docs (works / remote-only / clean error).
- Never log Authorization headers or other secrets.
- Add a new ADR under `docs/adr/` documenting the transport decision and the
  explicit SSE non-support. Update public docs (`docs/settings.md` or the
  relevant MCP docs page) for the new `~/.tny/mcp.json` `{"type":"http",...}`
  shape.
- Add unit + integration tests under `tests/`, including split-boundary
  streaming tests for the HTTP response parsing (see
  `chunked_survives_every_split_boundary` in `tests/test_net.c` as the
  pattern). Update `nix/source.nix` / `nix/tests.nix` if you add fixture
  dirs, targets, or tools.

## Verification you must run before finishing

1. `make test` — must pass.
2. `make quality CLANG_FORMAT='uvx clang-format@21.1.2' CLANG_TIDY='uvx clang-tidy@22.1.8' RUFF='uvx ruff@0.14.0'` — must pass (use `make format` to fix style).
3. Size check: build Release, strip, `wc -c` — stripped tny must stay < 2.0 MiB; report the number.

If a gate fails, fix and rerun until green.

## Deliverable

End your answer with:
1. A file-by-file summary of the change.
2. Verbatim tail of the passing `make test` and `make quality` output.
3. The stripped binary size in bytes.
4. Any spec judgment calls you made and why.
