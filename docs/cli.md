# CLI

Design the CLI so humans and coding agents can run it without menus. Every input is a flag or stdin. Interactive prompts are a fallback, never the only path. Each subcommand has `--help` with copy-paste examples.

Binary name: `tny`.

## Command tree

```text
tny                         # interactive TUI, fresh session
tny ask [prompt]            # one turn, then exit
tny edit FILE               # exact-match replacement from stdin
tny ask-user QUESTION       # ask the owning runner frontend (inside terminal)
tny image attach PATH       # attach an image to the next request (inside terminal)
tny resume [last|<id>]      # interactive resume
tny acp                     # ACP server (native loop only)
tny sessions
tny session last|<id>
tny session <id> --wait     # block until a background task finishes ([--timeout S])
tny session stop <id>       # stop a background task ([--kill])
tny providers               # list configured providers and doctor hints
tny tasks                   # list built-in and discovered task presets
tny task show NAME          # inspect one resolved preset
tny backends                # compatibility alias for providers
tny models
tny cursor COMMAND          # complete Cursor sdk.v1 catalog/management/raw RPC
tny permissions
tny workspace list|add|remove|clear
tny status
tny doctor
tny login                   # provider-specific; see --provider
tny logout
tny setup                   # write provider config from flags/env
tny mcp [list]              # list configured MCP servers (source attributed)
tny mcp call SERVER/TOOL    # one MCP tools/call; JSON arguments on stdin
```

Global flags are **leading**:

```text
tny --provider cursor|acp|openai|codex|claude|grok|NAME|acp@AGENT [command]
                            # --backend is an alias; NAME = an OpenAI-compatible
                            # profile; acp@AGENT = settings acp.AGENT
tny --cwd DIR
tny --model ID
tny --effort LEVEL          # reasoning effort (--reasoning-effort is an alias)
tny --system-prompt TEXT    # custom system prompt (docs/adr/0045)
tny --task NAME             # select a runtime task preset (issue #81)
tny --add-dir DIR           # repeatable, process-only
tny --permission-mode ask|auto|yolo   # default: yolo (docs/adr/0001)
tny --max-steps N|unlimited # cap native-loop model calls per turn
                            # (default: unlimited, docs/adr/0024)
tny --max-extension-iterations N|unlimited # cap Python-hook follow-up turns
tny --no-extensions         # skip ~/.tny/extensions for this process
tny --fast                  # paid fast tier (TNY_CAP_FAST providers only)
tny --json                  # where listed
tny --color auto|always|never   # SGR styling; --no-color is never
tny --ephemeral             # conversation/session artifacts stay in memory
tny -r                      # session picker (TUI)
tny -c                      # resume last for this workspace
```

`--no-save` is a compatibility alias for `--ephemeral`. `tny ask` accepts
both spellings after the subcommand as well as in the leading global position.

`tny doctor` reports the effective local-terminal sandbox as `sandbox: os`
or `sandbox: none` and explains the reason. Its JSON shape carries the same
truth in `"sandbox"` plus a human-readable `"sandbox_note"`. `auto` therefore
never claims `os` unless Seatbelt or bubblewrap is launchable, and the default
`yolo` process reports `none` even when `.tny.json` requests `auto` or `os`.
`tny status` uses the same effective-mode resolution.

`TNY_TOOLS=all|terminal+edit|terminal` overrides the `tools` user setting for
the native OpenAI-compatible loop. `all` is the unchanged default; the shell
profiles reduce both advertised and accepted built-ins as documented in
[Tools, MCP, skills, subagents](features/mcp-and-skills.md#native-tool-profiles).
`tny status` and `tny doctor` print the effective `tools` profile and expose it
as the JSON string field `"tools"`. libtny, wasm, and `tny acp` keep `all`; an
explicit profile ignored by wasm or ACP server mode emits one status line.

Color resolution ([ADR 0026](adr/0026-color-vs-attribute-sgr.md)): `NO_COLOR`
(any value, even empty) disables SGR *colors* only — bold/dim/reverse are
structural and stay, so the status bar keeps its reverse video.
`CLICOLOR_FORCE` (non-empty, not `0`) or `--color=always` forces full styling,
beating `NO_COLOR` and applying even when piped. `--color=never` /
`--no-color` emits no SGR at all.

## Task presets

`--task NAME` selects a runtime-owned instruction preset. Resolution order is
the workflow-private `TNY_WORKFLOW_TASK_DIR`, the workspace
`.tny/tasks/NAME.md`, the user `~/.tny/tasks/NAME.md`, then a built-in. The
private workflow variable is always set explicitly by the shell workflow
launcher, including an empty value, so unrelated ambient state cannot leak
into children. SSH workspaces conservatively resolve built-ins only.

Preset names match `[A-Za-z0-9_.-]{1,63}`, may not begin with `.` or contain
`..`, and files must be regular, non-symlink UTF-8 Markdown no larger than 256
KiB with a non-empty body; discovery accepts at most 256 definitions. Plain Markdown is valid. Optional frontmatter may
contain only `name:` (which must match the filename) and `description:`; the
frontmatter and leading blank lines after it are not sent to the model.

`tny tasks` prints deterministic name, source category, validity, and optional
description. `tny --json tasks` emits `{"kind":"tasks","tasks":[...]}`
objects with `name`, `source`, `description`, and `valid`. The source is one of
`workflow`, `project`, `user`, or `builtin`; absolute paths and instruction
bodies are never emitted. `ask --json`, `status --json`, and public session
JSON similarly expose only task name/source/digest metadata.
`tny task show NAME` is the explicit inspection surface and prints the resolved
instructions; `tny --json task show NAME` emits one `kind: "task"` object.
Session listings include the same `task` metadata object (or `null`) alongside
status, without exposing instruction bodies.
The digest is a 40-character SHA-1 integrity marker for the private snapshot;
the snapshot bytes are always checked as the source of truth and the digest is
never treated as a credential.

Create a project preset without rebuilding tny:

```markdown
---
name: release-review
description: Review release-readiness risks
---

Inspect the change for correctness, compatibility, rollback, and test coverage.
Report prioritized findings; do not edit unless explicitly asked.
```

Save it as `.tny/tasks/release-review.md`, then run
`tny --task release-review ask "Review this release"`.

The resolved instruction body travels with the request itself, so the model
adopts the preset without spending tool calls to locate or read the file. The
native openai-compatible loop carries it in the system prompt, after tny's
runtime instructions and project context and before any explicit
`--system-prompt` additions; host providers (cursor, acp), whose pinned
protocols expose no system field, receive the same sections at the top of a
fresh session's first user message
([ADR 0045](adr/0045-system-prompt-flag.md),
[ADR 0048](adr/0048-runtime-task-presets.md)).

The resolved snapshot belongs to the session. Resuming without `--task`
restores it; an explicit task must match the saved name and digest. A task may
not be grafted onto an older session after turns exist, and `/task` may change
or clear a task only before the fresh session's first turn. In browser/wasm
builds, built-ins and presets present in MEMFS work; persistent host user or
project discovery is unavailable.

## Ephemeral mode

Ephemeral mode is available on every conversational entry point:

```text
tny --ephemeral
tny --ephemeral ask "review this workspace"
tny ask --ephemeral --json "list the public CLI"
tny --ephemeral acp
```

The working transcript remains in memory for multi-turn TUI/ACP use, but tny
does not write session JSON, recovery checkpoints, large tool-result blobs, or
TUI prompt history. It also does not import saved conversation state:
`resume`, `--resume`, `-r`, `-c`, session recovery/migration, and TUI
`/resume` are incompatible with the mode.

`ask --json` includes `"ephemeral":true` and emits an empty `session_id`.
The local guarantee is provider-independent. Codex additionally receives
`ephemeral:true` at `thread/start`; the native Responses wire uses
`store:false`; other host agents may apply their own remote retention policy.
Configuration metadata such as the last selected provider/model is not part of
the conversation and retains its existing settings behavior. See
[ADR 0020](adr/0020-ephemeral-sessions.md).

## SSH: run the tools on another machine

`--ssh user@host[:port]` keeps tny local and runs every workspace tool of the
native loop — `read_file`, `edit_file`, `grep_files`, `terminal`, … — on the
remote host over one persistent OpenSSH connection
([ADR 0022](adr/0022-ssh-execution-boundary.md)). The remote host needs
`sshd`, a POSIX `sh` and coreutils; **tny is not required there**.

```sh
tny --ssh dev@example.com --ssh-cwd '~/app'        # TUI, tools act on the box
tny --ssh dev@example.com:2222 ask "run the tests" # one-shot
tny --ssh '[2001:db8::1]:22' ask "df -h"
```

- `--ssh-cwd DIR` sets the remote working directory (default: the login
  directory). A leading `~` resolves against the **remote** home — quote it
  (`--ssh-cwd '~/app'`) so the local shell doesn't expand it to the local
  home first. `--cwd` stays the *local* workspace for settings and sessions.
- The connection is opened before the TUI starts, so OpenSSH prompts for
  passwords / host keys as usual; tool calls then reuse it (`BatchMode`).
  The master lives in `~/.tny/ssh/` and idles out after 10 minutes.
- Works with the native loop providers (`openai`, `claude`, `grok`, any
  openai-compatible profile). Cursor, Codex and ACP hosts execute their own
  tools and are refused with an explanatory error.
- `memory`, `skill`, `subagent`, MCP and web tools stay local; `open_file`
  and `install_skill` report that they are unavailable over `--ssh`.
- `/undo` does not cover remote edits.
- The system prompt tells the model it is in a remote environment on the
  target host and states the remote working directory (instead of the local
  workspace); the TUI status bar shows `ssh user@host:/remote/dir`.
  Project `AGENTS.md` is loaded from the remote cwd, not from `--cwd`;
  `$HOME/.tny/AGENTS.md` still applies as user policy and is labeled local
  ([ADR 0040](adr/0040-ssh-agents-md.md)).

wasm behavior: remote-only — the browser build has no `ssh` to spawn, so
`--ssh` fails at connect with a clear error.

## Provider selection

`--provider` accepts the four builtin names, the two **builtin subscription
profiles** `claude` and `grok` ([ADR 0019](adr/0019-subscription-logins-claude-grok.md),
[backends/openai-compatible.md](backends/openai-compatible.md#builtin-subscription-profiles-claude-and-grok)),
plus any **named OpenAI-compatible provider** (`"openrouter"`, `"xai"`, a
local gateway — any name), defined either way or both:

- a top-level `~/.tny/settings.json` object with a `base_url`, and/or
- `NAME_BASE_URL` in the environment (name uppercased, non-alphanumerics →
  `_`; the env value beats the settings `base_url`)

Named providers run on the openai backend but keep their own name, config,
key env, and saved model (see
[backends/openai-compatible.md](backends/openai-compatible.md)). Env
detection is a lazy in-memory scan at provider-resolution time — startup
paths (`--help`, `--version`, first TUI paint) never run it.

### Named ACP agents ([ADR 0030](adr/0030-settings-schema-and-acp-map.md))

Reusable ACP commands live directly under `acp` and are selected through the
namespaced provider ID `acp@NAME`:

```json
{
  "acp": {
    "claude": {
      "command": "npx",
      "args": ["-y", "@agentclientprotocol/claude-agent-acp"],
      "model": "claude-sonnet-4-6"
    },
    "gemini": { "command": "gemini", "args": ["--acp"] }
  }
}
```

```sh
tny --provider acp@claude
tny --provider acp@gemini ask "review this repository"
```

`command` must be a nonempty string. `args` is an optional array of nonempty
strings; `model` is optional. Fields are validated only when that profile is
selected, so an unused bad entry cannot break another provider's startup.
Profile names use letters, digits, `-`, and `_`. Defining a profile does not
select it; after it is used, normal `last_provider` persistence may restore it
on the next launch.

The effective provider name is the full `acp@NAME`, so sessions and
`models["acp@NAME"]` stay isolated from other ACP agents. Model precedence is
`--model` > `models["acp@NAME"]` > the profile's `model` >
`ACP_NAME_DEFAULT_MODEL` > the agent default (profile punctuation becomes `_`
in the environment variable). The ad-hoc form remains `--provider acp --agent
CMD -- ARGS`; `--agent` cannot be combined with `--provider acp@NAME`.
The older `acp.agents.NAME` command array and `acp:NAME` selector remain
accepted for compatibility.

### `tny provider setup` ([ADR 0018](adr/0018-provider-setup-stored-keys.md))

The guided way to add one:

```text
tny provider setup opencode --base-url https://api.opencode.example/v1 --api-key sk-…
tny provider setup openrouter --base-url https://openrouter.ai/api/v1     --api-key-env OPENROUTER_API_KEY --model anthropic/claude-sonnet-4.6
tny provider setup            # interactive on a tty (key prompted with echo off)
```

Fields merge into the settings profile and `last_provider` switches to it, so
a bare `tny` runs on the new provider immediately. `--api-key` stores the key
in `~/.tny/settings.json` (the file drops to 0600); an environment variable
(`api_key_env`, default `NAME_API_KEY`) always beats a stored key, so
rotation from the shell keeps working. Storing a key clears `api_key_env`
and vice versa. Host providers (cursor/codex/acp) are refused — they have no
base_url/key shape. In the TUI the same flow is `/provider setup [NAME]`
(`/cancel` aborts), which is also how the browser wasm terminal adds
providers; the page URL hash additionally accepts
`NAME_BASE_URL`/`NAME_API_KEY`/`NAME_DEFAULT_MODEL` pairs directly.

`--provider` default, in order:

1. an explicit user default in `~/.tny/settings.json` (`"provider"`)
2. the provider (and its saved model) last used, recorded in `last_provider` / `models.{provider}` — named OpenAI-compatible and `acp@NAME` providers included
3. `openai` if `OPENAI_BASE_URL` or `OPENAI_API_KEY` is set
4. the env-defined provider if **exactly one** `NAME_BASE_URL` + `NAME_API_KEY` pair is set (a lone `*_BASE_URL` from an unrelated tool never hijacks the default; keyless local gateways need an explicit `--provider NAME` once — `last_provider` remembers it)
5. `codex` if a ChatGPT credential exists — `CHATGPT_ACCESS_TOKEN`, `~/.tny/codex-auth.json` (`tny --provider codex login`), or `$CODEX_HOME/auth.json` (`codex login`); `--chatgpt-token` counts too — the subscription drives the native loop, no API key ([backends/codex.md](backends/codex.md))
6. `claude` if a Claude Code OAuth login exists (`CLAUDE_CODE_OAUTH_TOKEN`, or `~/.claude/.credentials.json` from `claude /login`; a bare `ANTHROPIC_API_KEY` never hijacks the default — use `--provider claude`)
7. `grok` if an xAI session exists (`~/.grok/auth.json`, from `tny
   --provider grok login` or the grok CLI)
8. `cursor` if `CURSOR_API_KEY` is set in the environment
9. `openai` (its connect error explains how to configure a key)

A settings.json object or `NAME_BASE_URL` env var named `codex`, `claude` or
`grok` shadows the builtin profile entirely: explicit config wins. See
[settings.md](settings.md) and the published JSON Schema for `model`, `effort`,
`fast`, provider profiles, and named ACP-agent defaults.

## `tny login`

`tny [--provider NAME] login [--device]` signs in to the active provider.
tny never stores tokens itself:

| Provider | What login does |
| --- | --- |
| codex | Native ChatGPT sign-in, no Codex CLI ([ADR 0066](adr/0066-native-chatgpt-login-and-credential-sources.md)): the browser PKCE flow with a `localhost:1455` callback (the redirect URL can also be pasted into the terminal), or `--device` for a verification URL + one-time code on headless machines. The login lands in `~/.tny/codex-auth.json` (`0600`), which tny reads for the ChatGPT Responses backend and refreshes itself. `$CODEX_HOME/auth.json` from `codex login` keeps working as a fallback. |
| claude | Reports the credential tny resolved (`CLAUDE_CODE_OAUTH_TOKEN`, `ANTHROPIC_API_KEY`, `~/.claude/.credentials.json`), else runs `claude setup-token`; the user exports the printed token as `CLAUDE_CODE_OAUTH_TOKEN`. |
| grok | Native RFC 8628 device-code sign-in against `auth.x.ai` — no grok CLI needed, works over SSH/containers ([ADR 0021](adr/0021-native-grok-device-login.md)). tny prints the verification URL + code, polls the token endpoint, and writes the session to `~/.grok/auth.json` in the grok CLI's own store format (both tools share the entry). `GROK_OAUTH2_ISSUER` / `GROK_OAUTH2_CLIENT_ID` override the endpoint (enterprise IdPs, tests). |
| cursor | Reports whether `CURSOR_API_KEY` is set. |
| openai / named | Reports whether an API key resolved (`tny setup` configures one). |

`tny logout` mirrors this: native deletion of `~/.tny/codex-auth.json` for
codex (the Codex CLI's own file is left to `codex logout`), native removal of the xAI entries from `~/.grok/auth.json` for
grok (foreign-issuer entries are kept), an env-var hint otherwise.

## System prompt

`--system-prompt TEXT` (leading global flag, headless and interactive) sets a
user system prompt for the run ([ADR 0045](adr/0045-system-prompt-flag.md)).
Providers with a schema field for it use that field: the openai-compatible
backend prepends the text to its system message (`instructions` on the
responses wire, the `system` role message on chat), ahead of tny's
operational preamble and the AGENTS.md chain. The host protocols expose no
such field on their pinned surfaces (cursor sdk.v1 `CreateAgent`, ACP
`session/new`), so there the text is prepended to the
**first user message** of a fresh session, separated by a blank line.
Resumed sessions and later turns never get it again.

## Reasoning effort

`--effort` (env `TNY_REASONING_EFFORT`, TUI `/effort`) takes the canonical
levels `off | light | medium | high | xhigh | max` and maps them onto each
provider's wire vocabulary ([ADR 0009](adr/0009-reasoning-effort.md)):
cursor `ModelSelection.params`, openai `reasoning.effort` (`reasoning_effort`
on the chat wire; the codex/claude/grok profiles ride the same field). ACP has no
portable knob at protocolVersion 1; the
backend says so in one status line and the agent's default applies.

Providers advertise their real per-model levels through their catalogs;
`tny models` shows them (`[effort: …]` / `"efforts"` in `--json`) and any
advertised token is accepted verbatim (e.g. `--effort minimal` on openai).
Unset means the provider default; `--effort default` clears an inherited
env or settings value.

A default lives in `~/.tny/settings.json` under `"effort"` — one string for
every provider, or a per-provider object like `"models"`
([ADR 0015](adr/0015-settings-default-effort.md)):

```json
{ "effort": "high" }
{ "effort": { "codex": "xhigh", "openai": "medium" } }
```

Precedence: `--effort` / `/effort` (an explicit `default` included) beats
`TNY_REASONING_EFFORT` beats the settings entry beats the provider default.
tny never *writes* the effort back to settings — a scripted
`tny ask --effort X` does not change what tomorrow's session does.

## `tny edit` (stateless exact replacement)

`tny edit FILE` replaces an exact string only when it occurs once. The search
and replacement travel on stdin, never in argv. The default fence form is
convenient from a shell or another coding harness:

```sh
cat <<'TNY_EDIT' | tny edit src/example.c
*** SEARCH
return old_value;
*** REPLACE
return new_value;
*** END
TNY_EDIT
```

The marker lines must match exactly. `--marker STR` changes their prefix when
the payload itself contains a default marker line:

```sh
printf '@@ SEARCH\nold\n@@ REPLACE\nnew\n@@ END\n' |
  tny edit --marker @@ notes.txt
```

One structural line ending before `REPLACE` and `END` is not part of the
payload. Put a blank line before a marker when the exact search or replacement
must end in a newline.

`--json` selects both structured stdin and structured stdout. `old` and `new`
are strings; `replace_all` is an optional boolean whose default is `false`:

```sh
printf '%s\n' '{"old":"draft","new":"final","replace_all":false}' |
  tny edit --json README.md
```

Success writes one object on stdout:

```json
{"kind":"edit","path":"README.md","matches":1,"replaced":1}
```

Without `--json`, success prints one human-readable result line. Progress and
all diagnostics go to stderr. The target is built completely and installed by
atomic temp-file rename only after the match policy succeeds, so every failure
leaves it untouched. Existing symlinks remain symlinks and their target is
edited. On zero matches, stderr includes the single target line closest to the
first non-empty SEARCH line when that closest line is unique; this gives the
caller exact context for widening the search. Multiple matches report their
count and require either a wider search or JSON `replace_all:true`.

| Exit | Meaning |
| ---: | --- |
| 0 | One match replaced, or `replace_all:true` replaced one or more matches |
| 1 | Usage, input parsing, allocation, or file I/O failure |
| 2 | Zero matches, or multiple matches without `replace_all:true` |
| 130 | Interrupted; no partial write |

The verb is configuration-free: it does not load settings or require any
`TNY_*` environment variable. Relative paths use the process current working
directory; absolute paths work directly. `--ssh` is intentionally not part of
this standalone verb. In wasm it works like the `edit_file` tool on the virtual
filesystem (MEMFS in the browser and NODERAWFS in node).

**Inside tny** ([ADR 0063](adr/0063-in-process-intercept-of-first-party-verbs.md)):
typed into the `terminal` tool, `tny edit FILE` with a here-doc or a
`printf … |` payload is dispatched in-process instead of forking. It is
reviewed as `edit_file` on the resolved path, is undoable with `/undo`, and
under `--ssh` edits the file on the remote host. The printed result and exit
code are the ones above.

## `tny ask` (scripts and CI)

```text
tny ask "summarize this repository"
printf 'summarize src/\n' | tny ask --stdin
tny ask --json --ephemeral "list the public CLI"
tny ask --resume last "now add tests"
tny ask -B "audit the Makefile"        # detach; prints the session id
tny --provider cursor --model composer-2 ask "find the login bug"
tny --provider codex --effort xhigh ask "prove this queue is lock-free"
tny --yolo --cwd /tmp/ws ask "run the test suite"
```

Stdout: assistant Markdown (or one JSON object with `--json`).
Stderr: progress, tool lines, diagnostics.
Exit 0 finished, 1 startup/config, 2 run failed, 130 interrupted.

Token/context usage is silent by default. `--print-usage` (or
`TNY_PRINT_USAGE=1`) reports it on stderr, always on its own line after the
answer.

JSON object (keep field names stable):

```json
{
  "output": "…",
  "exit_code": 0,
  "provider": "openai",
  "model": "provider/model",
  "session_id": "…",
  "ephemeral": false,
  "steps": 1,
  "tool_calls": [{"name": "read_file", "status": "success"}],
  "extension_messages": [
    {"kind": "custom", "custom_type": "reviewer", "content": "…"},
    {"kind": "user", "content": "verify again"}
  ]
}
```

`--json` is required on `ask`, `status`, `doctor`, `permissions`, `models`, `session`, `sessions`, `workspace`, `usage`. `tny mcp --json` is optional.

**Inside tny**: a foreground `tny ask` typed into the `terminal` tool is
refused — it would run a second agent loop under the current turn, invisible
to the frontend, to cancellation, and to the step budget. Use `tny ask -B "…"`
(which runs as a real detached child, inheriting the turn's permission mode
and unable to widen it) and collect it with `tny session ID --wait --json`.
See [ADR 0063](adr/0063-in-process-intercept-of-first-party-verbs.md).

## Runner control verbs: `ask-user` and `image attach`

Shell commands launched by the native `terminal` tool receive the runner's
resolved socket path and session id as `TNY_SESSION_SOCK` and
`TNY_SESSION_ID`. This includes the short per-user fallback socket used when a
session directory is too deep for `sun_path`.

```sh
tny ask-user "Which deployment target should I use?"
printf 'Describe the expected fallback behavior' | tny ask-user
tny --json ask-user "Which branch?"
tny image attach screenshots/failure.png
tny --json image attach screenshots/failure.png
```

`ask-user` returns the owning interactive TUI's arbitrary text answer on
stdout. `image attach` validates that the path is under an allowed workspace
root and that its magic bytes identify png/jpeg/gif/webp, then queues it as
user-role image content for the next native provider request (ADR 0008).
`--json` emits `kind: "ask_user"` or `kind: "image_attach"` plus the request's
string correlation id.

Both commands are socket-bound and never read `/dev/tty`. Without
`TNY_SESSION_SOCK` they print exactly
`tny: no session socket (set TNY_SESSION_SOCK or run inside tny)` to stderr and
exit 1. Exit codes are 0 success, 1 usage/configuration, 2 rejected or failed
control operation, and 130 interrupted. wasm, `--ephemeral`,
`TNY_ISOLATE=0`, and the macOS post-TLS in-process fallback have no runner
socket and therefore take this clean-error path. A noninteractive `tny ask`
owner does not wait for a human and preserves the existing
`ask_user_question` fallback string. `tny acp` stays in-process and maps the
question through its ACP client permission callback rather than creating a
runner socket. See [ADR 0058](adr/0058-session-control-channel-roles-and-tool-ops.md).

**Inside tny**: typed directly into the `terminal` tool, both verbs skip the
socket entirely and reach the turn in memory
([ADR 0063](adr/0063-in-process-intercept-of-first-party-verbs.md)); the
output and exit codes are the same. The socket path stays for everything
deeper — a script, a `make` recipe, or another process the command started.

## `tny mcp`

```text
tny mcp
tny mcp list
tny --json mcp
```

Lists configured MCP servers without spawning them. Each row names the
server, its source (`tny` / `codex` / `claude` / `grok` / `cursor-agent`), and
whether it is connected, still starting, skipped, or not yet started.
`--json` emits `{"kind":"mcp_servers","servers":[...],"notices":[...]}`;
each server includes `source`, `scope`, `transport`, `status`, and `skipped`. Foreign
harness configs are read only when `mcp.import_from` in
`~/.tny/settings.json` names them ([ADR 0051](adr/0052-mcp-import-from-harnesses.md));
the default is off. Native `~/.tny/mcp.json` wins on name collision.
Command lines and env values are omitted from the listing so secrets stay
out of `--json`. wasm: the list still works; spawn stays the existing
clean error.

### `tny mcp call SERVER/TOOL`

```text
echo '{"path":"src/main.c"}' | tny mcp call fs/read_text_file
tny --json mcp call deploy/status < args.json
```

One MCP `tools/call`, reachable from any shell — tny's own `terminal` tool,
another harness, or a script ([ADR 0057](adr/0057-shell-first-native-loop.md),
[ADR 0064](adr/0064-cli-verb-conventions.md)).

- **Arguments ride stdin**, never argv: one JSON object, or nothing at all
  (empty stdin, or a terminal on stdin, means `{}`). Anything else — invalid
  JSON, an array, a scalar — is a usage error. The payload is capped at 1 MiB.
- **Permissions** are checked immediately before `tools/call` with the same
  engine and the same identity the native loop uses,
  `mcp:<server>/<tool>`. In the default `yolo` mode
  ([ADR 0001](adr/0001-run-all-agents-in-yolo-mode.md)) the call proceeds. In `ask` mode the
  command never prompts: it fails closed with exit 2 until a rule allows that
  exact identity, e.g. `"permission": {"mcp:deploy/status": "allow"}` in
  `~/.tny/settings.json`.
- **Servers come from `~/.tny/mcp.json`** plus any source named in
  `mcp.import_from`; a repo-local `.mcp.json` is never read on its own. The
  command is a one-shot, so it pays a cold start (spawn + `initialize` +
  `tools/list`) and shuts the server down again on exit.
- **Inside tny** ([ADR 0063](adr/0063-in-process-intercept-of-first-party-verbs.md)):
  typed into the `terminal` tool, `tny mcp call SERVER/TOOL` (optionally with
  an `echo`/`cat` producer for the arguments) is answered by the session's
  already-warmed client. There is no second server process and no cold start;
  the identity, output, and exit codes are unchanged.
- **Output.** The result content goes to stdout, diagnostics to stderr.
  `--json` prints one object:
  `{"kind":"mcp_call","server":…,"tool":…,"ok":true,"result":"…","bytes":N,"truncated":false}`,
  plus `"result_file"` when the result was spilled. Server output is untrusted
  data and is bounded like a tool result: above `max_tool_result_bytes`
  (32 KiB by default) the preview is capped and the whole result is written to
  a `0600` file under `~/.tny/results/` whose path is printed.
- **Exit codes.** 0 the tool answered; 1 usage or configuration (bad
  `SERVER/TOOL`, stdin that is not one JSON object, unknown server, a server
  that will not start); 2 the call was refused or failed (permission denied,
  JSON-RPC error, `isError: true`, timeout); 130 interrupted.
- **wasm:** HTTP MCP servers work (remote-only, subject to CORS); a stdio
  server keeps the existing clean spawn error.

## Multi-agent workflow scripts

The installed `share/tny/tny-workflows.sh` library builds validated dependency
DAGs from ordinary `tny ask --stdin` processes. It provides bounded fan-out,
ordered fan-in context, branch-isolated failures, captured results, and clean
signal propagation under both Bash and Zsh:

```sh
. "$HOME/.local/share/tny/tny-workflows.sh"
tny_workflow_begin
trap 'tny_workflow_cleanup' EXIT

tny_task inspect --provider codex -- "Inspect the implementation"
tny_task test-plan --provider cursor -- "Design the missing tests"
tny_task implement --after inspect --after test-plan --   "Implement and verify using both reports"

tny_workflow_run --jobs 2
tny_result implement
```

Shell tasks are ephemeral by default. The helper exposes the normal provider,
model, effort, workspace, permission, SSH, and ACP-agent selections; it does not
implement provider behavior itself. Full API and failure semantics:
[workflows.md](workflows.md).

## Process isolation (every turn, [ADR 0053](adr/0053-forked-turn-isolation.md))

On native builds **every** turn — foreground `tny ask` and the TUI included
— executes in a detached session-runner process; the invoking `tny` is only
a renderer streaming the runner's events from `<session-dir>/sock`. Killing
the caller (crash, closed terminal, SIGKILL) detaches the turn instead of
killing it: the runner finishes, finalizes the session's
`status`/`exit_code`/`result`, and exits, so `tny ask --resume <id>` (or
`tny resume`) continues the conversation afterwards. `^C` in a foreground
`ask` still cancels the turn; a second `^C` detaches and leaves it running.
In-process turns remain only on wasm, with `--ephemeral`, or with the
`TNY_ISOLATE=0` debug escape hatch.

Every socket client first handshakes as `owner`, `observer`, or `tool`. The
unique owner may control turns and answer prompts; observers can only watch
and detach; tool clients can only send correlated `ask_user` and
`image_attach` requests. While `terminal` waits for a child, the runner pumps
only these socket operations and owner replies—never backend dispatch—so a
child blocked in `tny ask-user` cannot deadlock the active tool call.

### `tny session attach <id>`

Attach to a **live** run — a `-B` task, a foreground turn whose caller
died, or a turn owned by another shell — and stream it: a snapshot of the
output so far, then live events (text to stdout, tool/status lines to
stderr). `^C` detaches and the turn keeps running; cancelling stays
`tny session stop`. Approvals are answered by the owning client (or by the
runner's permission mode when none is attached), never by an attach. On a
session with no live runner, attach exits 1 and points at `tny session
<id>`.

```sh
id=$(tny ask -B "audit the Makefile")
tny session attach $id             # watch it live; ^C detaches
```

## Background one-shots (`tny ask -B`)

`-B` / `--background` runs the identical ask turn detached and defers its
output into the session instead of stdout
([ADR 0031](adr/0031-background-ask.md)). The parent prints the session id
and exits in milliseconds; a forked child runs the turn and finalizes the
session with `status`, `exit_code`, and `result` — the `result` object is
byte-for-byte what foreground `tny ask --json` would have printed, for every
backend.

```sh
id=$(tny ask -B "audit the Makefile")
tny session $id                    # status: running (pid N), live partials
tny session $id --json | jq .result
tny session $id --wait --json | jq -r .result.output   # block until finished
tny session stop $id               # SIGTERM the task's process group
tny ask --resume $id "now fix it"  # follow up once it is done
tny ask --resume $id --steer "drop that — check the tests instead"
```

Output shape: plain mode prints the bare session id on stdout; `--json`
prints `{"kind":"ask_background","session_id":…,"pid":…}`. The parent's
exit code covers the **launch only** — 0 launched, 1 precondition failure.
Turn failures never reach the parent; they surface as the session's
`status`/`exit_code`/`result`.

Where the output goes: the answer lands in the session transcript and the
`result` field; the child's stderr/stdout (progress, tool lines) go to
`<session-dir>/task.log`. See
[features/sessions.md](features/sessions.md) for the on-disk layout,
status lifecycle, and staleness rules.

Reading the output: plain `tny session <id>` is human-readable — after the
stats it prints the transcript (full user/assistant text; one compact `⏺`
line per tool call, `✓` per tool result), then the stored `result` for a
finished run or the live checkpointed partial text for a running one
(refreshed on a ~2 s cadence; rerun the command to poll). A live run with
nothing streamed yet says so and points at `task.log`. `--json` dumps the
raw document for scripts.

Composition and preconditions:

- `-B --resume <id>` backgrounds a follow-up turn on an existing session.
- `-B` rejects `--ephemeral` (exit 1): the printed id would point at
  nothing.
- `--stdin` works: stdin is drained fully before the id is printed.
- `--continue-recovery` is allowed.

### `tny session <id> --wait` (+ `--timeout SECS`)

Blocks until the session's background turn has finished, then prints the
session exactly as the plain/`--json` inspect would
([ADR 0041](adr/0041-session-wait.md)). Liveness is the writer-lock probe,
so a crashed task returns immediately as stale. The exit code is the turn's
`exit_code` — 0 `done`, 2 `error`/stale, 130 `interrupted` — so a script can
branch on it; `--timeout SECS` implies `--wait` and exits 124 (printing the
still-running view) if the turn outlasts it. On a session that is not
running, `--wait` is a plain inspect with the same exit-code mapping.

```sh
id=$(tny ask -B "audit the Makefile")
tny session $id --wait --json | jq -r .result.output
for id in $ids; do tny session $id --wait --timeout 900 >/dev/null || echo "$id failed"; done
```

### `tny session stop <id>` (+ `--kill`)

Stops a running background task by signaling its **process group**
(SIGTERM): the turn cancels cleanly, spawned backend hosts die with the
group, and the session finalizes `status:"interrupted"` with partial output
preserved. On a finished session `stop` is a clean no-op that reports the
status. If the child ignores SIGTERM, `stop` reports a timeout and suggests
`--kill`, which SIGKILLs the group and writes the terminal status on the
child's behalf. `--json` emits `{"kind":"session_stop","status":…}`.

```sh
tny session stop $id
tny session stop $id --kill        # last resort for a wedged task
```

### Resuming a running session

Bare `--resume` on a session whose turn is still running fails, exit 1:

```text
tny: session <id> is still running (pid N)
  watch:     tny session <id>
  attach:    tny session attach <id>
  stop:      tny session stop <id>
  take over: tny ask --resume <id> --steer "new prompt"
```

Taking over must be explicit: `--steer "…"` is interrupt-and-redirect. It
runs the stop sequence (group-SIGTERM, bounded wait), then resumes with the
new prompt, folding the checkpointed partial output into the transcript so
the model sees what it was doing before the interrupt. **Pending tool work
is abandoned by design** — this is "drop that, do this instead", not a
live mid-turn steer (that remains a future ADR; the TUI's in-turn steering
is [ADR 0011](adr/0011-mid-turn-input-steer-or-queue.md)). On a session
that is not running, `--steer` is a plain resume. If the stop sequence
times out, steer errors and suggests `session stop --kill`; it never
SIGKILLs on its own. `--steer` composes with `-B`: redirect, then
re-detach.

wasm behavior: `-B` is **native only** — the browser build has no
`fork(2)` and fails with a clean error
(`tny: --background is not available in the browser build`, exit 1) before
any backend work.

## `tny cursor` management

`tny cursor` starts a short-lived v1.0.30 bridge, negotiates capabilities,
performs one operation, and applies the same authenticated shutdown/process
cleanup as a conversational turn. Readable aliases cover the public catalog
and management surface:

```text
tny cursor ping | version | me | models | repositories
tny cursor create [NAME]
tny cursor resume|reload|close AGENT_ID
tny cursor send AGENT_ID MESSAGE
tny cursor wait|run|conversation RUN_ID
tny cursor runs|agent|messages|artifacts AGENT_ID
tny cursor observe RUN_ID [AFTER_OFFSET]
tny cursor cancel RUN_ID [AGENT_ID]
tny cursor agents
tny cursor archive|unarchive AGENT_ID
tny cursor delete AGENT_ID --yes
tny cursor download AGENT_ID PATH
tny cursor usage AGENT_ID [RUN_ID]
```

Create, resume, and send use the same validated `settings.cursor` option
composition as normal conversations. `download` writes artifact bytes to
stdout incrementally, capped at 8 MiB. Destructive delete requires `--yes`
before a bridge is spawned.

For additive fields and operations without a convenience alias:

```text
tny cursor rpc SERVICE METHOD [JSON|-] [--yes]
```

`SERVICE` may be `SdkAgentService`, `sdk.v1.SdkAgentService`, or the canonical
`/sdk.v1.SdkAgentService`; `METHOD` is case-sensitive. Only the 27 pinned
client-to-bridge routes are accepted. The request must be one UTF-8 JSON object
no larger than 8 MiB, supplied as one argument, on stdin with `-`, or as `{}`
when omitted on a terminal. Unary output remains the bridge JSON object;
server-stream output is one unmodified JSON frame per line. Raw
`DeleteAgent` also requires `--yes`. Prefer stdin for requests containing an
API key or other secret so it does not enter the shell history/process list.

This command is native-only and exits 1 with
`tny: cursor: sdk.v1 management is unavailable in WebAssembly` before reading
credentials or starting bridge work. Provider management is not part of the public libtny ABI;
libtny exposes Cursor conversations through its normal runtime API.

## Provider-specific flags

| Provider | Flags / env |
| --- | --- |
| cursor | `--bridge-bin PATH`, `CURSOR_SDK_BRIDGE_BIN`, `CURSOR_API_KEY` (also pass through to RPCs) |
| codex (builtin profile) | credential precedence `--chatgpt-token` (+ `--chatgpt-account-id`) > `CHATGPT_ACCESS_TOKEN` (+ `CHATGPT_ACCOUNT_ID`) > `~/.tny/codex-auth.json` (`tny --provider codex login`) > `$CODEX_HOME/auth.json` (`codex login`); the winning file auto-refreshes in place, flag/env tokens need no filesystem; account id explicit or from the JWT claim → `https://chatgpt.com/backend-api/codex` on the Responses wire with `chatgpt-account-id` + `OpenAI-Beta: responses=v1`; an `OPENAI_API_KEY` auth.json → `api.openai.com`; default model `gpt-5.6-sol`; `TNY_CODEX_BASE_URL` redirects the ChatGPT-mode URL (mocks/gateways) without shadowing the profile ([backends/codex.md](backends/codex.md)) |
| acp | `--agent CMD` plus extra args after `--`, e.g. `tny --provider acp --agent gemini -- acp`; `--agent ws://host:port` connects to a remote agent instead of spawning ([ADR 0017](adr/0017-wasm-browser-parity.md)) |
| openai | `--base-url`, `--api-key-env NAME`, `--wire-api responses\|chat` (default `responses`; `chat` for legacy-only providers, [ADR 0016](adr/0016-responses-api-default-wire.md)), `OPENAI_BASE_URL`, `OPENAI_API_KEY`, `OPENAI_WIRE_API` |
| named provider | same flags; `NAME_BASE_URL` (beats the settings `base_url`), key from the profile's `api_key_env`, default `NAME_API_KEY` — never `OPENAI_API_KEY`; `NAME_WIRE_API` / profile `wire_api` |
| claude (builtin profile) | credential from `CLAUDE_CODE_OAUTH_TOKEN` > `ANTHROPIC_API_KEY` > `~/.claude/.credentials.json` (`$CLAUDE_CONFIG_DIR` honored); OAuth tokens add `anthropic-beta: oauth-2025-04-20`; chat wire; default model `claude-sonnet-4-6`; `TNY_CLAUDE_BIN` for login |
| grok (builtin profile) | session token from `~/.grok/auth.json` (minted by tny's native device login or the grok CLI; expired OIDC tokens auto-refresh at resolve) → CLI chat proxy (chat wire, `X-XAI-Token-Auth` + `x-grok-model-override` + `x-grok-client-version` headers — the proxy 426s unversioned clients, `TNY_GROK_CLIENT_VERSION` overrides the pin — default model `grok-4.6`); else `XAI_API_KEY` → `api.x.ai` (responses wire, same default model); `GROK_OAUTH2_ISSUER` / `GROK_OAUTH2_CLIENT_ID` override the login endpoint |

Model precedence for every provider: `--model` > saved `models.{provider}` >
the provider object's `model` (openai-compatible only) > `NAME_DEFAULT_MODEL`
from the environment (`CODEX_DEFAULT_MODEL`, `OPENROUTER_DEFAULT_MODEL`, …).

`tny ask` never blocks on an approval. Unresolved permissions fail the run unless `--auto` reviews (native loop) or `--yolo`. Host providers must be pre-authorized or they fail closed.

`--image PATH` (repeatable) attaches image files to the first user message.
The native OpenAI-compatible loop uses `image_url` data URLs; Cursor v1.0.30
uses base64 `SdkImageData` with the same detected MIME type. The native loop
also uses the encoding when the model calls `read_image` mid-turn. Max 8 MiB;
type comes from magic bytes (png/jpeg/gif/webp), not the extension. At most 16 `--image`
flags are accepted. A 17th prints `tny: too many --image flags (max 16)` and
exits 1 before any image file is opened or a backend is connected. That
startup path frees the prompt buffer it may already have allocated, matching
the `--output-schema` and unknown-flag error returns
([ADR 0008](adr/0008-native-loop-images.md)).

## Structured output (`--output-schema`)

`tny ask --output-schema VALUE` constrains the final answer to a JSON Schema
via Chat Completions `response_format` (openai-compatible provider only —
other providers fail at startup with exit 1). VALUE is a file path, or inline
JSON when it starts with `{`. Three shapes are accepted and normalized:

- a bare JSON Schema — wrapped as `{"type":"json_schema","json_schema":{"name":"output","strict":true,"schema":…}}`
- a `json_schema` object (`{"name":…,"schema":…}`) — wrapped, `name` defaults to `output`
- a full `response_format` (`{"type":"json_schema",…}`) — sent as-is

```text
tny ask --output-schema schema.json "extract the TODOs as JSON"
tny ask --output-schema '{"type":"object","properties":{"count":{"type":"integer"}},"required":["count"],"additionalProperties":false}' "how many files?"
```

Stdout is the model's JSON text (inside `output` with `--json`). The tool
loop still runs; the schema constrains the final assistant message.
## `--fast` (speed tier)

`--fast` opts in to the provider's paid fast tier ([ADR 0010](adr/0010-fast-tier-capability.md); `TNY_CAP_FAST` in
`src/core/backend.h`). OpenAI renamed "priority processing" to "fast mode";
the API accepts both spellings. Each capable provider maps the flag to its
own wire field:

| Provider | Wire mapping |
| --- | --- |
| openai | `"service_tier":"priority"` on the chat-completions request (`fast` alias server-side) |
| codex | same as openai — the builtin profile rides the Responses request's `service_tier` |
| cursor | `ModelSelection` param `{"id":"fast","value":"true"}` — fast is a per-model variant, not a request field |
| acp | not supported — `--fast` exits 1 with the capable provider list |

The interactive TUI exposes the same capability as `/fast [fast|priority|default]`.

Provider caveat: `--provider cursor` runs Cursor's own headless loop. Its
built-in tools have no per-call approval RPC, so tny permission rules apply
only to explicitly registered custom-tool callbacks.

## `--max-steps` (agent loop cap)

The native loop runs for as long as the turn needs by default — no step limit
([ADR 0024](adr/0024-unlimited-steps-default.md)). `--max-steps N` caps model
calls per turn; a capped turn stops with "step limit reached" on stderr and
`tny ask` exits 2. `tny status --json` reports the cap as `agent_step_limit`
(`0` means unlimited).
`--max-steps unlimited` (or `0`) clears a cap a repo set through the
`.tny.json` `"steps"` limit. The interactive TUI exposes the same knob as
`/max-steps set N` / `/max-steps clear`. Host providers (cursor, acp)
run their own loops and are not affected.

`--max-extension-iterations N` independently caps continuations requested by
Python `agent_end` hooks; its default is unlimited and `0`/`unlimited` clears
the cap. `--no-extensions` disables the trusted global hooks for the process.
See [extensions.md](extensions.md).

## Help shape

```text
Usage: tny ask [options] [prompt]

Options:
  --json          Write one JSON object to stdout
  --resume last   Continue the latest workspace session
  --ephemeral     Keep conversation/session artifacts in memory only
  --no-save       Compatibility alias for --ephemeral
  --provider NAME cursor | acp | openai | codex | claude | grok | settings profile (--backend also accepted)

Examples:
  tny ask "explain src/main.c"
  tny ask --json --ephemeral "list exported symbols"
  tny --provider cursor --model composer-2 ask "fix the leak"
```

Missing required values print the error, then a correct example, then exit 1. No timed prompts.

### Tested contract

`make test-help-flags` extracts accepted long and short flags from the C argv
parsers and compares them with `tny --help` plus every subcommand's `--help` in
both directions. A parser flag without help text, help text without a parser,
or a dispatched subcommand missing from top-level help fails the test
([ADR 0042](adr/0042-help-flag-alignment.md)). The small source allowlist is
reserved for explicitly justified compatibility or passthrough syntax.
