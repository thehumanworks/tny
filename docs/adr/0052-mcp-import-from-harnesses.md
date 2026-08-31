# 0052 — Opt-in import of MCP servers from other harnesses

Date: 2026-08-31
Status: accepted

## Context

tny's MCP client reads only `~/.tny/mcp.json` ([ADR 0049](0049-mcp-background-warmup.md)).
Users who already run Codex, Claude Code, grok, or Cursor keep the same
servers there and must duplicate them by hand. Those foreign files can name
arbitrary executables and carry credentials, so auto-loading them would
cross a trust boundary.

Issue #88.

## Decision

**Import is off by default.** `~/.tny/settings.json` may name sources:

```json
{ "mcp": { "import_from": ["codex", "claude", "grok", "cursor-agent"] } }
```

An empty or absent list reads no foreign files. Each named source is
independent: a malformed file disables that source with a notice and does
not break startup.

`~/.tny/mcp.json` remains authoritative. On a name collision the native
entry wins; the foreign one is skipped with a notice. Imported servers keep
their original names so `mcp_select_tool` identity stays `mcp:server/tool`;
listings (`tny mcp list --json`, `/mcp`) attribute `source`.

tny never writes another harness's config.

### Pinned paths and shapes

Only these files are considered, and only after the matching global opt-in.
The Claude, grok, and Cursor opt-ins intentionally authorize their documented
project files in the current workspace; an untrusted clone cannot activate
them while the setting is absent.

| Source | Path | Shape imported |
| --- | --- | --- |
| tny | `~/.tny/mcp.json` | `{"servers":{"name":{"command":["…"]}}}` |
| Codex | `$CODEX_HOME/config.toml` (default `~/.codex/config.toml`) | `[mcp_servers.name]` tables: `command` string + `args` array + `env` table. Codex 0.149.1 / current config reference. |
| Claude Code | `~/.claude.json` root/user `mcpServers`, current-project entry under `projects`, then `<workspace>/.mcp.json` | stdio: `command` string + `args`/`env`; remote: `type`/`url`. Claude precedence is local > project > user. |
| grok | `.grok/config.toml` overlays from `<workspace>` back to the git root, then `~/.grok/config.toml` | `[mcp_servers.name]` tables. Pinned to Grok Build source commit `bc7f02eddd3d84085849dc19ed216f11c23b0571` (2026-08-28); the stable channel reported 1.0.13 while that public snapshot reported 1.0.12. Deeper project definitions win. |
| cursor-agent | `<workspace>/.cursor/mcp.json`, then `~/.cursor/mcp.json` | `mcpServers` map, project before user. `"cursor"` remains an accepted settings alias. |

stdio servers (command + optional args/env) run. Remote HTTP / SSE / WebSocket
entries are listed as skipped with a visible notice until remote transport
lands (issue #87). Unknown fields are ignored.

### Bounded TOML

Codex and grok use TOML. A bounded subset parser
(`src/util/toml.c`) covers comments, dotted tables, strings, numbers,
bools, arrays, and inline tables — enough for `[mcp_servers.*]`. Unknown
fields are ignored; malformed recognized MCP fields disable that source.
Arrays of tables and the rest of TOML are skipped. A vendored full TOML
library was rejected on size.

### Trust

Imported config is untrusted data: the same permission model as
hand-configured servers (`mcp:server/tool`), no secrets in logs or
`--json` listings (command lines and env values are omitted from list
output). Explicitly enabling a source authorizes its stdio programs to join
ADR 0049's normal background warm-up; calls still pass through
`mcp_select_tool` and its permission identity, and server output remains
untrusted model data. `tny acp` server mode and libtny still never load the
user MCP profile.

`tny mcp` / `tny mcp list` is the CLI surface; `--json` emits
`kind: "mcp_servers"` with `name`, `source`, `scope`, `transport`, `status`,
`skipped`, and `notices`.

## Consequences

- Default-off keeps today's behavior when `mcp.import_from` is unset: no
  foreign files are opened.
- Enabling Claude, grok, or cursor-agent is also explicit trust for that
  harness's current-workspace project file.
- Users can reuse existing stdio MCP servers without copying JSON.
- wasm: import still parses JSON/TOML, but spawn stays the existing lazy
  clean error (ADR 0049).
- Unit and CLI integration coverage use fixture configs under
  `tests/fixtures/mcp-import/` for every harness.
