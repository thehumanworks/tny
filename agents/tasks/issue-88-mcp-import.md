You are the implementing agent. Work directly in the current directory with
your own tools (read/edit files, run shell commands). Do NOT launch `tny`,
nested agents, or detached sessions — do the work yourself, in this turn.

## Goal

Implement GitHub issue #88 for the tny repo you are in: **opt-in import of MCP
server configs** from other agent harnesses (codex, Claude Code, grok,
cursor-agent), off by default.

Read `CLAUDE.md`, `docs/settings.md`, `docs/cli.md`, the existing `src/mcp/`
config parsing, and relevant ADRs in `docs/adr/` before writing code.

## Issue body (authoritative spec)

## Context

tny's MCP client reads only the trusted profile `~/.tny/mcp.json`. Users who already run codex, Claude Code, grok, or cursor-agent maintain the same MCP server lists there and must duplicate them by hand for tny.

## Goal

Optionally discover and reuse MCP server definitions from the harnesses tny supports:

- **codex** — `~/.codex/config.toml` (`[mcp_servers.*]` tables)
- **Claude Code** — `~/.claude.json` / `.mcp.json` (project scope) `mcpServers` maps
- **grok** — its MCP config file
- **cursor-agent** — `~/.cursor/mcp.json` / project `.cursor/mcp.json`

(Verify exact paths/formats against current releases at implementation time; pin what we parse in the docs.)

## Requirements

- **Off by default.** A setting (e.g. `mcp.import_from = ["codex", "claude", ...]` in tny settings) enables it, per-source. No import happens without explicit opt-in — this is a security boundary: those files can name arbitrary executables and carry credentials.
- `~/.tny/mcp.json` remains authoritative; on name collisions the native profile wins. Imported servers are namespaced or clearly attributed in `--json` / TUI listings so the user can tell where a server came from.
- Import is read-only: tny never writes to other harnesses' config files.
- Only import shapes tny can actually run: stdio servers, plus remote HTTP servers once the remote-transport work lands (see linked issue). Unsupported entries (e.g. SSE-only) are skipped with a visible notice, not an error.
- Parsing lives in the config layer, tolerant of unknown fields; a malformed foreign config disables that source with a warning rather than breaking startup. Note: codex uses TOML — decide whether a minimal vendored TOML reader is worth it or whether codex import is deferred; record in the ADR.
- Treat imported config as untrusted data: same permission model applies to imported servers as hand-configured ones; no secrets logged.
- CLI surface per `docs/cli.md` conventions (e.g. `tny mcp list --json` shows source of each server).
- New ADR documenting the setting, precedence, and trust model; `docs/settings.md` updated.

## Acceptance criteria

- [ ] With the setting off (default), behavior is byte-for-byte unchanged and no foreign files are read.
- [ ] With a source enabled, its stdio MCP servers appear and run; collisions resolve in favor of `~/.tny/mcp.json`.
- [ ] Malformed/missing foreign configs degrade gracefully with a warning.
- [ ] Unit tests with fixture configs for each supported harness format; `nix/source.nix` / `nix/tests.nix` updated.
- [ ] Docs + ADR; `make quality` and `make test` pass.

Depends on / relates to #87 (remote MCP transports) for importing remote-server entries.


## Coordination note

Remote HTTP transport (#87) is being built in a **parallel branch** — it is
NOT in your tree. Structure the import layer so foreign entries are
translated into tny's internal MCP config shape; import stdio entries fully,
and for remote/http-shaped foreign entries emit the "skipped: unsupported
transport" notice path the issue requires (keyed off what the current config
layer actually supports, so it lights up automatically once #87 merges).
Do not implement any HTTP transport yourself.

## Boundaries

- You MAY edit any file in this worktree. Do NOT commit — leave all changes
  uncommitted in the working tree.
- C11 only. For codex's TOML config: prefer a minimal hand-rolled parser
  scoped to the `[mcp_servers.*]` shapes we need, or defer codex import — make
  the call and record it in the ADR. No large vendored TOML library.
- With the setting off (default): no foreign file is opened at all.
- `~/.tny/mcp.json` wins name collisions; imported servers show their source
  in `tny mcp list` / `--json` output per `docs/cli.md` conventions.
- Import is read-only; malformed foreign config disables that source with a
  warning, never breaks startup. Treat imported config as untrusted data.
- New ADR under `docs/adr/` (setting, precedence, trust model);
  update `docs/settings.md` and `docs/cli.md`.
- Unit tests with fixture configs for each supported harness format under
  `tests/`. Update `nix/source.nix` / `nix/tests.nix` for new fixtures.

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
4. Any spec judgment calls you made (esp. the codex TOML decision) and why.
