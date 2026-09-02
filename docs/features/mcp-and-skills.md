# Tools, MCP, skills, subagents

Native loop only, unless noted.

## Built-in tools

Keep fx names so prompts and muscle memory transfer:

| Area | Tools |
| --- | --- |
| Files | `list_files`, `glob_files`, `grep_files`, `read_file`, `write_file`, `edit_file`, `delete_file`, `rename_file`, `copy_file`, `create_folder`, `file_info` |
| Search | `semantic_search` (lexical, not embeddings), `open_file` |
| Shell | `terminal` (fx runtime name; accept `run_command` as an alias) |
| Web | `web_search` (optional provider), `web_fetch` |
| Images | `read_image` (png/jpeg/gif/webp via magic bytes; `vision` is an alias). Tool result is a short text; pixels go out as a follow-up user `image_url` message ([ADR 0008](../adr/0008-native-loop-images.md)). `tny ask --image PATH` attaches the same shape on the first user message (max 16 flags; a 17th is exit 1) |
| Skills | `skill`, `install_skill` |
| Subagents | `subagent` (`create`, `inspect`, `message`, `relationship`, `configure`, `lifecycle`) |
| MCP | `mcp_search_tools`, `mcp_select_tool`, `mcp_features` only; namespaced `server/tool` names ride a system-prompt catalog, never the tools array ([ADR 0049](../adr/0049-mcp-background-warmup.md)) |
| Runtime | `ask_user_question`, `memory`, `read_tool_result` |

Large results: bounded preview + session handle; `read_tool_result` reads a byte range or literal search. Background commands persist pid, cwd, log path, detected URL.

`memory` writes `~/.tny/memories.json` only when asked. Do not inject it into every prompt. In [ephemeral mode](../adr/0020-ephemeral-sessions.md), `memory set` is rejected so a conversation cannot create durable user memory; `get` and `list` may still read existing memories.

No browser/CDP tools in v1.

## MCP client

Authoritative profile: `~/.tny/mcp.json`. A clone cannot opt itself into MCP
authority: project files are considered only after the user's global settings
explicitly enable their harness source.

Opt-in import ([ADR 0051](../adr/0051-mcp-import-from-harnesses.md)): `mcp.import_from` in `~/.tny/settings.json` may list `"codex"`, `"claude"`, `"grok"`, and/or `"cursor-agent"` (`"cursor"` alias). Off by default — no foreign file is opened until named. Claude `.mcp.json`, Grok Build `.grok/config.toml`, and cursor-agent `.cursor/mcp.json` project files load only after their global source opt-in. Native names win on collision. Stdio servers run; the current tree lists remote HTTP/SSE/WS entries as `skipped: unsupported transport` behind the transport capability seam, ready for issue #87. `tny mcp list --json` attributes `source`, `scope`, and `transport`. tny never writes those files. wasm: parse works, spawn stays the existing clean error.

Transports: stdio JSONL and Streamable HTTP ([ADR
0051](../adr/0051-mcp-streamable-http.md)). Existing entries keep their exact
shape; omitting `type` means stdio:

```json
{
  "servers": {
    "local": { "command": ["node", "/path/to/server.js"] },
    "remote": {
      "type": "http",
      "url": "https://mcp.example/mcp",
      "headers": { "X-Tenant": "example" },
      "bearer_token_env": "EXAMPLE_MCP_TOKEN"
    }
  }
}
```

Every HTTP JSON-RPC message is a POST to the configured endpoint. The response
must be one `application/json` document, delivered with fixed-length or
arbitrarily split chunked HTTP framing. For legacy Streamable HTTP, tny runs `initialize`,
copies an opaque `Mcp-Session-Id`, and sends it with the negotiated protocol
version on later requests. For MCP `2026-07-28`, a successful `server/discover`
advertisement selects stateless v2: each request carries protocol/client
metadata and routing headers, with no initialize, initialized notification,
session id, or teardown round trip.

tny never parses `text/event-stream`, opens a GET event stream, or falls back
to deprecated HTTP+SSE. An SSE response or GET-only endpoint returns an
actionable unsupported-transport error telling the user to configure the
Streamable HTTP POST endpoint or use a local stdio proxy. wasm:
HTTP MCP is remote-only over `fetch()` (subject to CORS); stdio spawn stays a
clean error.

Startup ([ADR 0049](../adr/0049-mcp-background-warmup.md)): a native session warms every profile server in the background at session start — TUI after first paint, `tny ask` overlapping its connect (after the `-B` fork) — one detached thread per server opening its transport, negotiating the protocol era, and running `tools/list`. Never for `--help`/`--version`, `tny acp` server mode, or libtny. A call that names a server mid-warm waits out its handshake (the prewarm-take contract); a failed warm-up is silent until a call names it, which retries and reports the usual error.

Catalog, not schemas: the per-request system prompt lists the cached tools as `server/tool — one-line description` (capped per tool and per session; overflow says to use `mcp_search_tools`), so the model knows what exists with no extra round trip. Full MCP JSON schemas are never promoted into the function-schema `tools` array — the only MCP entries there are `mcp_search_tools`, `mcp_select_tool`, `mcp_features`, and every call goes through `mcp_select_tool` so the permission identity stays `mcp:server/tool`.

`mcp_search_tools` AND-matches whitespace-separated tokens against name + description; an empty query lists the cached catalog without starting or waiting for any server. Re-check permissions immediately before `tools/call`. Treat server output as untrusted data, not instructions.

Remote auth: `headers` contains non-secret static metadata. `header_env` maps a
header name to an environment-variable name, and `bearer_token_env` supplies a
Bearer token. Literal `Authorization` values are rejected. Configured and
resolved header values and `Mcp-Session-Id` are treated as secrets: they are
never logged, included in errors, events, transcripts, or diagnostics.

Wasm behavior: **remote-only**. HTTP entries work lazily through the existing
fetch/ReadableStream transport, subject to browser CORS. Stdio entries retain
the clean spawn-unavailable error. There is no extra wasm protocol
implementation and every blocking body wait still goes through `tny_poll`.

ACP sessions (`tny acp`): use only client-supplied `mcpServers`, not the user profile (fx rule).

tny is not an MCP server.

## Skills

Directory + `SKILL.md` (YAML frontmatter `name`, `description`). Discover metadata at startup; load body only on invoke (`$` / `/skills` / `skill` tool).

Search order (workspace upward, stop before `$HOME`): `skills/`, `.agents/skills/`, `.claude/skills/`, `.codex/skills/`, `.cursor/skills/`, `.opencode/skills/`. Then user: `~/.tny/skills/` and the same hidden names under `$HOME`. Extra dirs do not contribute skills.

Managed installs go only to `~/.tny/skills/`.

### Mentions ([ADR 0056](../adr/0056-skill-mention-injection.md))

A user message that contains `/<name>` or `$<name>` as a whole token — at the
start or after whitespace, followed by the end, whitespace, or punctuation
other than `/`, `-`, `_` — where `<name>` is a discovered skill, carries that
skill's `SKILL.md` ahead of the text, with no `skill` tool round trip:

```text
<skill name="deploy" path="/abs/skills/deploy/SKILL.md">
...SKILL.md verbatim...
</skill>

ship $deploy to staging
```

`/foo` does not match `foobar`, `foo-bar`, `a/foo`, or `/foo/bar`; `$foo.`
does. Several mentions inject each skill once, in order of first appearance.
Bodies above `max_tool_result_bytes` are cut like a tool result (native: a
`read_tool_result` handle; hosts: the file path). This applies wherever a
prompt reaches a backend through the engine — `tny ask`, the TUI, `tny acp`
server, and the host backends (cursor, codex, ACP client), which cannot see
tny's `skill` tool. The system-prompt catalog is unchanged.

The native transcript stores the text the model saw; a top-level
`skill_injections` record in `session.json` keeps the typed text for
`/transcript` and `tny session <id>` and marks the skill delivered, so a later
mention sends a one-line reminder instead of the body until compaction drops
it. In the TUI a builtin slash command always wins over a same-named skill.

## Subagents

Child **native** sessions. One-off or persistent. Parent/child messages queued on disk so the child transcript is not dumped into the parent. Children cannot raise permission mode above the creator unless a human set it in the manager (Ctrl-X). Host backends: no tny-spawned subagents; show host task events if they exist (e.g. Cursor `cursor/task`).

The child is a `tny ask` process and **runs the parent's resolved provider**: the parent forwards `--provider` (its effective profile name), `--base-url` and `--wire-api` on the native backend, plus `--model`, `--effort`, `--permission-mode`, and `--ephemeral` on the child command line. The child never re-resolves from settings, so a remembered `last_provider` from an earlier host-backend chat cannot re-route it. API keys are never placed on the command line; they travel through the inherited environment or the same settings the parent read. Model-supplied strings (`id`, `prompt`) are shell-quoted into single arguments. A child that dies before its turn reports its captured stderr in the tool result; a child turn that ends in an error (nonzero `exit_code` or an `error` in its `--json` payload) is a tool error, not a success.

Native `create`/`message` operations emit correlated `subagent_start` and
`subagent_end` extension events around the child process. A pre-tool deny or
stop occurs before the process is started. Host task/subagent events are only
advertised when the pinned adapter supplies stable identity and a real terminal
boundary.

An ephemeral parent propagates `--ephemeral` to every child process. Those children are one-shot: `create` works, but `message` and `inspect` are unavailable because no child session id or transcript is stored. The tool result says that no resumable id exists.

## Project instructions

Load `AGENTS.md` (and `CLAUDE.md` if `AGENTS.md` is absent) from `$HOME/.tny/`, launch ancestors, and the primary workspace. Narrower path wins on conflict; user text still wins over files. Tool calls can attach target-scoped `AGENTS.md` for that path. Extra dirs do not contribute instructions. `context: false` disables this.

Over `--ssh` / `/ssh` ([ADR 0040](../adr/0040-ssh-agents-md.md)) the chain is `$HOME/.tny/` (labeled as local user policy — tools do not run there) then `AGENTS.md` from the **remote** cwd. Launch-dir and ancestor files are skipped: they describe the local tree, which is not the tool workspace. The remote file is prefixed with a banner that tny itself is local and attached over SSH.
