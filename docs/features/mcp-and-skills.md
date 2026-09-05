# Tools, MCP, skills, subagents

Native loop only, unless noted.

## Built-in tools

Keep fx names so prompts and muscle memory transfer:

| Area | Tools |
| --- | --- |
| Files | `list_files`, `glob_files`, `grep_files`, `read_file`, `write_file`, `edit_file`, `delete_file`, `rename_file`, `copy_file`, `create_folder`, `file_info` |
| Search | `semantic_search` (lexical, not embeddings), `open_file` |
| Shell | `terminal` (fx runtime name; accept `run_command` as an alias) |
| Web | `web_fetch`; `web_search` only when a provider is configured (see [Web search providers](#web-search-providers)) |
| Images | `read_image` (png/jpeg/gif/webp via magic bytes; `vision` is an alias). Tool result is a short text; pixels go out as a follow-up user `image_url` message ([ADR 0008](../adr/0008-native-loop-images.md)). `tny ask --image PATH` attaches the same shape on the first user message (max 16 flags; a 17th is exit 1) |
| Skills | `skill`, `install_skill` |
| Subagents | `subagent` (`create`, `inspect`, `message`, `relationship`, `configure`, `lifecycle`) |
| MCP | `mcp_search_tools`, `mcp_select_tool`, `mcp_features` only; namespaced `server/tool` names ride a system-prompt catalog, never the tools array ([ADR 0049](../adr/0049-mcp-background-warmup.md)) |
| Speech | `speak` (text, optional voice): automatic ephemeral playback using the Codex login, independent of the chat provider; advertised only with credentials and a player. [Speech contract](../speech.md) |
| Runtime | `ask_user_question`, `memory`, `read_tool_result` |

Large results: bounded preview + session handle; `read_tool_result` reads a byte range or literal search. Background commands persist pid, cwd, log path, detected URL.

### Native tool profiles

The user setting `tools` and higher-precedence `TNY_TOOLS` select what the
native OpenAI-compatible loop both advertises and accepts ([ADR
0062](../adr/0062-native-tool-profiles-advertise-and-enforce.md)):

| Profile | Advertised built-ins |
| --- | --- |
| `all` (default) | The complete table above |
| `terminal+edit` | `terminal`, `edit_file`, `read_image`; also `ask_user_question` in an interactive session |
| `terminal` | `terminal`, plus `read_image` because pixels cannot ride terminal stdout (ADR 0008) |

libtny custom tools are appended in every profile. Shell profiles hide the MCP
meta-tools and use `tny mcp call SERVER/TOOL`; replaying or directly requesting
any hidden built-in, including `mcp_select_tool`, returns `unknown tool` before
dispatch. libtny, wasm, and `tny acp` keep `all`. wasm retains the existing
clean error for `terminal`; profiles do not add a browser shell.

Foreground terminal results in either shell profile begin with `exit:`,
`bytes:`, and `cwd:`. The preview is capped at the smaller of the configured
tool-result limit and 8 KiB. A truncated result adds `full: PATH`; the `0600`
file contains output from byte zero and lives under the session's `results/`
directory, or `~/.tny/results/` without a session. Collection has a 64 MiB
hard cap. The `all` profile keeps the existing result shape and
`read_tool_result` handles.

Which profile should be the default is a measured question, not a taste
question: `tests/bench/bench_tools.py` runs all three arms over a frozen task
set and the numbers are recorded in the Measurement section of [ADR
0057](../adr/0057-shell-first-native-loop.md). See
[ci.md](../ci.md#benchmarks) for how to run it.

`memory` writes `~/.tny/memories.json` only when asked. Do not inject it into every prompt. In [ephemeral mode](../adr/0020-ephemeral-sessions.md), `memory set` is rejected so a conversation cannot create durable user memory; `get` and `list` may still read existing memories.

No browser/CDP tools in v1.

### First-party `tny` verbs inside `terminal` ([ADR 0063](../adr/0063-in-process-intercept-of-first-party-verbs.md))

A single simple `tny …` command typed into `terminal` is **not** run as a
nested process. tny recognises it while preparing the tool call and dispatches
it in-process, so it keeps the permission engine, session grants, `/undo`, the
warmed MCP client, and the `--ssh` route:

| Command | Runs | Permission identity |
| --- | --- | --- |
| `tny edit [--json] [--marker M] FILE` | the shared exact-match editor with the `edit_file` undo hook; over `--ssh`, `cat` + local replace + atomic write-back on the remote host | `edit_file` + resolved path |
| `tny mcp call SERVER/TOOL` | one `tools/call` on the session's already-warmed client — never a second server | `mcp:server/tool` |
| `tny mcp tools SERVER`, `tny mcp describe SERVER/TOOL` | the warmed client's cached `tools/list`: argument names, or one tool's full input schema | `mcp_search_tools` |
| `tny memory get\|set\|list …` | the `memory` tool | `memory` |
| `tny skill show NAME` | the `skill` tool | `skill` |
| `tny image attach PATH` | the same queue as `read_image`, allowed roots only | `read_image` |
| `printf … \| tny speak [--voice NAME] [--json]` (or quoted heredoc) | the shared speech service, on the tny host | `speak` |
| `tny ask-user [--json] QUESTION` | the frontend ask hook, with no socket round trip | `ask_user_question` |
| `tny ask …` (no `-B`) | refused: a foreground nested agent inside a turn | — |
| `tny ask -B …`, and everything else | `/bin/sh`, unchanged | `terminal` + command |

The result is the verb's own contract — an `exit: N` line then its stdout and
stderr — not a shell transcript, and `tool_start` names the verb
(`tny edit docs/x.md`) instead of the raw command.

**Payloads still ride stdin.** Two shapes are understood: a here-doc
(`tny edit FILE <<'EOF' … EOF`) and one left-hand producer piped in
(`printf '…' | tny edit FILE`, `echo '{…}' | tny mcp call s/t`,
`cat args.json | tny mcp call s/t`). `printf` must carry no `%` conversion and
`echo` no backslash, because shells disagree about those.

**Everything else runs in the shell exactly as before**: a second command
(`;`, `&&`, `||`, `&`), any redirection other than that here-doc, a second
pipe, a substitution or variable (`$(…)`, `` ` ` ``, `$VAR`), a glob, an
env-assignment prefix (`FOO=bar tny …`), a global flag other than `--json`
before the verb, `background: true`, or any verb not in the table. The
standalone binary still works there — it just runs cold, without the session's
permissions, undo, or warm MCP client.

Every `terminal` child is started with `TNY_NESTED=1` and `TNY_NESTED_MODE`
naming the turn's effective permission mode; a nested tny cannot widen it (see
[permissions](permissions.md)).

wasm: not applicable. `terminal` cannot start a child process in the browser
and returns its existing clean tool error, so no command reaches the
recogniser.

### Web search providers

tny ships no search engine. `web_search` is **advertised to the model only
when `~/.tny/settings.json` names a provider** ([ADR
0055](../adr/0055-web-search-gating-and-command-provider.md)); without one the
tool is absent from the native loop's tools array, so the model never burns a
call to learn there is no provider. A direct call (SDK, `--json` replay) still
gets the runtime error `no web search provider configured`.

| Key | Shape | Behaviour |
| --- | --- | --- |
| `web_search_command` | shell command template | Runs through the `terminal` tool's path (same cwd, `--ssh` remote, 60 s timeout, bounded output); the result is the command's exit code plus its stdout/stderr |
| `web_search_url` | URL template | `GET` over HTTP(S), same bounded body as `web_fetch` |

Both templates take the placeholder as `{query}` or `{{query}}`; every
occurrence is replaced. The query is always **percent-encoded** (only
`A-Za-z0-9-_.` pass through), for both keys: the encoded form is valid inside a
URL and is a single safe shell word, so a query such as `$(id)` can never reach
the shell unquoted. Put the placeholder inside the URL argument, not as a bare
search phrase, if the command wants human-readable text. If both keys are set,
`web_search_command` wins.

```json
{
  "web_search_command": "lightpanda fetch --dump markdown --log-level fatal --strip-mode full \"https://search.brave.com/search?q={{query}}&source=web\"",
  "web_search_url": "https://html.duckduckgo.com/html/?q={query}"
}
```

wasm: `web_search_url` works as before (fetch); `web_search_command` returns
the clean error `web_search_command is not available in wasm`, while the tool
stays advertised because a provider is configured.

For a token-efficient setup that saves result pages to disk and hands the
model only an index to `read_file`, see
[`examples/web-search/`](../../examples/web-search/README.md).

## MCP client

Authoritative profile: `~/.tny/mcp.json`. A clone cannot opt itself into MCP
authority: project files are considered only after the user's global settings
explicitly enable their harness source.

Opt-in import ([ADR 0051](../adr/0052-mcp-import-from-harnesses.md)): `mcp.import_from` in `~/.tny/settings.json` may list `"codex"`, `"claude"`, `"grok"`, and/or `"cursor-agent"` (`"cursor"` alias). Off by default — no foreign file is opened until named. Claude `.mcp.json`, Grok Build `.grok/config.toml`, and cursor-agent `.cursor/mcp.json` project files load only after their global source opt-in. Native names win on collision. Stdio servers run; the current tree lists remote HTTP/SSE/WS entries as `skipped: unsupported transport` behind the transport capability seam, ready for issue #87. `tny mcp list --json` attributes `source`, `scope`, and `transport`. tny never writes those files. wasm: parse works, spawn stays the existing clean error.

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

From a shell ([ADR 0057](../adr/0057-shell-first-native-loop.md), [ADR 0064](../adr/0064-cli-verb-conventions.md)): `tny mcp call SERVER/TOOL`
runs one `tools/call` for any harness with a shell — tny's own `terminal`
tool, Claude Code, Codex, CI. The JSON arguments ride stdin (empty stdin, or a
terminal on stdin, means `{}`; argv would be a quoting footgun), the result
content goes to stdout, diagnostics to stderr, and `--json` prints one
`{"kind":"mcp_call",…}` object. The permission identity is the same
`mcp:server/tool` the native loop uses, checked with the same engine
immediately before `tools/call`: in the default `yolo` mode it passes, and in
`ask` mode the command never prompts — it fails closed with exit 2 until a
rule names that identity. Exit codes: 0 ok, 1 usage/config (bad spec, stdin
that is not one JSON object, unknown server), 2 refused or failed
(`isError: true`, a JSON-RPC error, a timeout), 130 interrupted.

**Schema discovery** ([ADR 0068](../adr/0068-mcp-tool-schema-discovery.md)):
`tny mcp tools SERVER` lists a server's tools with their argument names
(`name* (type)`, `*` = required) and `tny mcp describe SERVER/TOOL` prints one
tool's description and full `inputSchema` (`--json`: `mcp_tools` / `mcp_tool`
objects carrying the schema verbatim). The shell-profile prompt tells the
model to run `describe` before its first call to a tool and never to guess
argument names; the MCP catalog header under shell profiles names both verbs.
As a backstop, a `tny mcp call` that the server rejects (JSON-RPC error or
`isError: true`) prints the tool's input schema after the error — on stderr
and as `input_schema` in `--json` — so the retry is informed. Neither verb
calls the tool; inside a session both are answered by the warmed client under
the `mcp_search_tools` identity.

Cross-harness rules are unchanged by the CLI: servers come from the
user-global `~/.tny/mcp.json` plus any `mcp.import_from` source; a project
`.mcp.json` is never read on its own. A one-shot `tny mcp call` outside a
session pays a cold start (spawn, `initialize`, `tools/list`) and shuts the
server down again on exit; inside a running tny session the warmed client
answers instead. Server output stays untrusted data and is bounded like a
tool result: above `max_tool_result_bytes` the preview is capped and the full
result is written to a `0600` file under `~/.tny/results/` whose path is
printed (`result_file` in `--json`). wasm: HTTP servers work remote-only,
stdio keeps the clean spawn error, so `tny mcp call` against a stdio server in
the browser reports that error and exits 1.

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
server, and the host backends (cursor, ACP client), which cannot see
tny's `skill` tool. The system-prompt catalog is unchanged.

The native transcript stores the text the model saw; a top-level
`skill_injections` record in `session.json` keeps the typed text for
`/transcript` and `tny session <id>` and marks the skill delivered, so a later
mention sends a one-line reminder instead of the body until compaction drops
it. In the TUI a builtin slash command always wins over a same-named skill.

## Subagents

Child **native** sessions. One-off or persistent. Parent/child messages queued on disk so the child transcript is not dumped into the parent. Children cannot raise permission mode above the creator unless a human set it in the manager (Ctrl-X); a `tny ask -B` the model starts from `terminal` is held to the same rule by `TNY_NESTED` ([ADR 0063](../adr/0063-in-process-intercept-of-first-party-verbs.md)). Host backends: no tny-spawned subagents; show host task events if they exist (e.g. Cursor `cursor/task`).

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
