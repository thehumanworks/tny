# Tools, MCP, skills, subagents

### Explicit image preview tools

`image_generate`, `image_edit`, `image_export`, and `image_contact_sheet` accept
optional boolean `preview` (default false). Recorded replay through
`from_manifest` supports it too. `image_preview` accepts exactly `manifest`
or a complete `job`/integer `item` pair and selects the successful artifact
once before permission, without generating or converting. The owning backend applies configured-true image input, real
continuable-batch readiness, roots, capacity and exact captured-byte digest.
Terminal interception shares the coordinator and prepared identity. Status is
nested separately from producer success; `queued` is not visual approval.
`image_edit` also accepts `job`/`item` as its final reference after artifact and
images, within the same five-reference total. Its producer hash/bytes, optional
manifest and precise producing/projection attempts are retained through
permission and stored in reference provenance. Replay never queries the job.
Native job selectors are unsupported on wasm; stored-provenance replay is shared.
SDK toolkit remains metadata-only. Wasm native-loop tools use shared admission;
socket CLI controls and external transforms return clean unsupported behavior.
See [image preview](../images.md#explicit-conversation-preview-adr-0097).

Native loop only, unless noted.

## Built-in tools

Keep fx names so prompts and muscle memory transfer:

| Area | Tools |
| --- | --- |
| Files | `list_files`, `glob_files`, `grep_files`, `read_file`, `write_file`, `edit_file`, `delete_file`, `rename_file`, `copy_file`, `create_folder`, `file_info` |
| Search | `semantic_search` (lexical, not embeddings), `open_file` |
| Shell | `terminal` (fx runtime name; accept `run_command` as an alias) |
| Web | `web_fetch`; `web_search` only when a provider is configured (see [Web search providers](#web-search-providers)) |
| Images | `read_image` (png/jpeg/gif/webp via magic bytes; `vision` is an alias). A configured-false `image_input` policy hides and refuses this tool and image attachment; image generation remains independent. Tool result is a short text; the pixels are **captured when the tool runs** and go out as a follow-up user `image_url` message ([ADR 0008](../adr/0008-native-loop-images.md), [ADR 0096](../adr/0096-captured-image-queue-and-preview-lifecycle.md)), so rewriting the file later in the same batch cannot change what is sent. `tny ask --image PATH` attaches the same shape on the first user message (max 16 flags; a 17th is exit 1) |
| Skills | `skill`, `install_skill` |
| Subagents | `subagent` (`create`, `message`, `inspect`, `lifecycle`; see [Subagents](#subagents)) |
| Jobs | `job_submit`, `job_control` (`cancel`/`retry`/`rm`), `job_status` (`status`/`wait`/`logs`/`list`): durable ask/image work that outlives the turn ([jobs.md](../jobs.md), [ADR 0093](../adr/0093-durable-native-jobs-and-verified-retry.md)). Native only; hidden in embedded runtimes, under `--ssh`, and — for the execution tools — wherever no child process can be owned |
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
dispatch. The one exception is `subagent`, which answers with its stable
`SUBAGENT_UNSUPPORTED_CONTEXT` line naming the shell fallback
([Subagents](#subagents)). libtny, wasm, and `tny acp` keep `all`. wasm retains the existing
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
| `tny jobs submit ask\|image\|batch …` | the durable job service, with the prompt from `--prompt` or a piped producer | `job_submit` + job/items/outputs/request digest |
| `tny jobs status\|wait\|logs\|list …` | the same service, read-only | `job_status` + job id |
| `tny jobs cancel\|retry\|rm …` | the same service | `job_cancel` / `job_retry` / `job_rm` + job id |
| `tny jobs …` that does not parse | refused with the reason: never handed to the shell, so the classifier cannot bypass the job identities | — |
| `tny ask …` (no `-B`) | refused: a foreground nested agent inside a turn | — |
| `tny ask -B …`, and everything else | `/bin/sh`, unchanged | `terminal` + command |

The result is the verb's own contract — an `exit: N` line then its stdout and
stderr — not a shell transcript, and `tool_start` names the verb
(`tny edit docs/x.md`) instead of the raw command.

A deeper child process that only has `TNY_SESSION_SOCK` reaches the same queue
over the control channel, where a third tool-role operation, `image_preview`,
admits an explicitly requested generated-image preview under stricter rules
(configured-true `image_input`, allowed roots, tool-batch readiness and an
expected-hash check) and answers an explicit status
([`tny ask-user` and `tny image attach`](../cli.md#runner-control-verbs-ask-user-and-image-attach),
[ADR 0096](../adr/0096-captured-image-queue-and-preview-lifecycle.md)). No
shipped command sends it yet.

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

Durable child **native** sessions ([ADR 0087](../adr/0087-explicit-subagent-contract-and-private-launch.md)). Each child is an ordinary workspace session run by a separate `tny ask` process; the parent receives only the child's final answer, never its transcript. Host backends own their own loops: tny never spawns native subagents for them and shows host task events only where the adapter supplies them (e.g. Cursor `cursor/task`).

| Action | Arguments | Result |
| --- | --- | --- |
| `create` | `prompt` (nonempty UTF-8); **omit `id`** | Runs one child turn and returns `subagent ID finished.` with the new durable id and the answer |
| `message` | `id` returned by `create`, `prompt` | Appends one turn to that same child session |
| `inspect` | `id` | Stored identity and metadata: title, turns, provider, model, created/updated, status, exit code, liveness, and the stored answer when the last turn finished `done` |
| `lifecycle` | `id` | `status`, `exit_code`, `running`, `resumable` read from the session and its writer lock |

tny allocates the 16-lowercase-hex id; there is no alias namespace. Any `id` on `create` — a name, or even an existing hex id — is rejected before a child starts rather than silently resuming or overwriting. Other strings, including `last`, are not child ids. Relationship/configure actions and on-disk message queues do not exist; each `message` is one synchronous child turn.

`lifecycle` reports what is recorded, not a description: a live writer lock is `running` (and not resumable); a stored `running` without a live writer is `stale`; stored `done`, `error` and `interrupted` keep their exit code; a session that never recorded a status (for example one written by an in-process `TNY_ISOLATE=0` turn) is `unknown` with `exit_code: null`, never an invented success.

**Launch and inheritance.** The child is this same executable started through the host process seam (`tny_process_spawn`, no shell) in its own process group. Its argv carries only selectors: `--cwd`, `--provider` (the parent's effective profile name, so a remembered host `last_provider` cannot re-route it), `--wire-api`, `--model`, `--effort`, `--permission-mode`, `--ephemeral`, `ask --json --stdin`, and `--resume-id` for `message`. The prompt arrives on stdin. The parent's resolved API key and base URL — including a flag-selected `--api-key-env` key and a secret-bearing gateway URL — go into the child's private environment as `TNY_SUBAGENT_API_KEY` / `TNY_SUBAGENT_BASE_URL`, which the child reads through `--api-key-env` / `--base-url-env`; a `--chatgpt-token` (and `--chatgpt-account-id`) source becomes that child's `CHATGPT_ACCESS_TOKEN` (and `CHATGPT_ACCOUNT_ID`). The parent's own environment is never modified. Like any ambient provider key, these carriers are visible to the child's own tool subprocesses. Ceilings: the child gets `TNY_NESTED=1` with the parent's mode and `TNY_TOOLS` with the parent's profile, and an inherited `TNY_PERMISSION_MODE` is dropped, so a child can never run wider than its creator. Child stdout is bounded (8 MiB) and child stderr is discarded. Cancelling the parent turn sends the child its interrupt, which stops the child's own session runner; after 3 s the child's process tree is killed.

**Stable diagnostics.** Every failure is one tool-result line `error: SUBAGENT_<CODE>: <guidance>`. The code is the contract; the guidance names a valid invocation or fallback and never echoes a supplied value, child stderr, partial output, a provider error body, a key or a base URL.

| Code | When |
| --- | --- |
| `INVALID_ARGUMENT` | Not an object; missing/empty/non-string `action`; unknown field; `id` on `create`; missing or malformed `id` for `message`/`inspect`/`lifecycle`; missing, empty or non-UTF-8 `prompt`; a `prompt` on `inspect`/`lifecycle` |
| `UNSUPPORTED_ACTION` | Any other action, including `relationship` and `configure` |
| `UNSUPPORTED_CONTEXT` | Embedded (libtny), prompt optimisation, `terminal` / `terminal+edit` tool profiles, `--ssh`, host providers, a build that cannot start processes (wasm), `message`/`inspect`/`lifecycle` under an ephemeral parent, or `message` to a host-owned session |
| `SESSION_NOT_FOUND` | No stored session with that id in this workspace |
| `SESSION_BUSY` | The child holds its writer lock (a running turn), or the id is the parent's own session |
| `AUTH_UNAVAILABLE` | The parent has no resolved credential for a non-`http://` provider |
| `LAUNCH_FAILED` | The child process could not be started |
| `CHILD_FAILED` | The child exited nonzero, died on a signal, or its turn reported a failure; names the child id when one was stored |
| `INVALID_RESPONSE` | The child exited 0 without a complete, well-formed turn result for the expected session (missing, malformed, truncated or unstored) |
| `CANCELLED` | The parent turn was cancelled while the child ran |

Validation, unsupported-action and context rejections happen before the permission gate and before any extension event or process. The others are answered inside the correlated `subagent_start` / `subagent_end` extension events that native `create`/`message` emit around the child; a pre-tool deny or stop occurs before the process is started. Host task/subagent events are only advertised when the pinned adapter supplies stable identity and a real terminal boundary.

**Execution modes.** `TNY_ISOLATE=0` keeps parent and child in-process turns (see `lifecycle` above). An ephemeral parent passes `--ephemeral` to its children: `create` works as a one-shot and says no resumable id was stored; `message`, `inspect` and `lifecycle` are `UNSUPPORTED_CONTEXT`. Shell tool profiles hide `subagent`; their fallback is `tny ask -B --json "…"` through `terminal` followed by `tny session <id> --wait`. Under `--ssh` a child would run its tools on this machine rather than the remote host, so the tool is hidden and refused. wasm cannot start processes: `inspect`/`lifecycle` read stored sessions, while `create`/`message` answer `UNSUPPORTED_CONTEXT`.

## Project instructions

Load `AGENTS.md` (and `CLAUDE.md` if `AGENTS.md` is absent) from `$HOME/.tny/`, launch ancestors, and the primary workspace. Narrower path wins on conflict; user text still wins over files. Tool calls can attach target-scoped `AGENTS.md` for that path. Extra dirs do not contribute instructions. `context: false` disables this.

Over `--ssh` / `/ssh` ([ADR 0040](../adr/0040-ssh-agents-md.md)) the chain is `$HOME/.tny/` (labeled as local user policy — tools do not run there) then `AGENTS.md` from the **remote** cwd. Launch-dir and ancestor files are skipped: they describe the local tree, which is not the tool workspace. The remote file is prefixed with a banner that tny itself is local and attached over SSH.

## Image tools

`image_generate` and `image_edit` share the [image service](../images.md) with
`tny image generate/edit`. The full profile advertises them when local ChatGPT
credentials exist; shell profiles get command guidance and in-process
interception. Both default to `gpt-image-2.5-sunburst` with `high` quality;
`model` can select `gpt-image-2.5-flare`, and `quality` accepts `auto`, `low`,
`medium`, `high`, `xhigh`, or `max`. They have separate sensitive permission
identities and include the resolved image provider and all uploaded reference
paths in the grant scope. The operation is resolved once, before the permission
question, and that approved plan is exactly what runs — no record is reread and
no second permission check happens, so approving a call once approves that call
and nothing else ([ADR 0095](../adr/0095-owned-image-plans-and-retained-failure-detail.md)).
A call that reaches the executor without that prepared plan is refused.
Use `read_image` to inspect
the output. They are unavailable under `--ssh` and libtny.

When an image is written but its manifest cannot be finalized, the tool result
keeps the usual `error: ` marker and the object after it reports the retained
artifact (`committed: true` with its path), so the model is told the file
exists instead of assuming nothing happened.

`image_export` and `image_contact_sheet` share the same service with `tny image
export` / `tny image contact-sheet`. They are local transforms, not generation:
no provider is called, nothing is invented, and the output is recorded as a
derived artifact with the hashes of the bytes it consumed. They take an ordered
`sources` array of `{"image": PATH}` and `{"artifact": RECORD}` entries,
`output_file`, an exact `size` (`WIDTHxHEIGHT`), and optional `fit`
(`fit`/`crop`/`pad`), `gravity`, `background`, `format`, `overwrite`,
`persist_manifest`, plus `columns` and `labels` for a sheet. Because they
depend on the host rather than on a provider, they are advertised without image
credentials and hidden only where tny cannot run a local process — libtny,
`--ssh` and wasm — while the optional ImageMagick 7 `magick` executable is
reported as missing at call time with actionable guidance. Each has its own
sensitive permission identity covering the operation, the ordered sources with
the hash of their exact bytes, the destination and every setting that changes
the output; that identity is rechecked at execution, so a source or record
edited after approval needs a new grant. `tny image export` and `tny image
contact-sheet` typed into `terminal` are intercepted into these same tools.
