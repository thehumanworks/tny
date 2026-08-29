# CLI

Design the CLI so humans and coding agents can run it without menus. Every input is a flag or stdin. Interactive prompts are a fallback, never the only path. Each subcommand has `--help` with copy-paste examples.

Binary name: `tny`.

## Command tree

```text
tny                         # interactive TUI, fresh session
tny ask [prompt]            # one turn, then exit
tny resume [last|<id>]      # interactive resume
tny acp                     # ACP server (native loop only)
tny sessions
tny session last|<id>
tny session <id> --wait     # block until a background task finishes ([--timeout S])
tny session stop <id>       # stop a background task ([--kill])
tny providers               # list configured providers and doctor hints
tny backends                # compatibility alias for providers
tny models
tny permissions
tny workspace list|add|remove|clear
tny status
tny doctor
tny login                   # provider-specific; see --provider
tny logout
tny setup                   # write provider config from flags/env
```

Global flags are **leading**:

```text
tny --provider cursor|codex|acp|openai|NAME|acp@AGENT [command]
                            # --backend is an alias; NAME = an OpenAI-compatible
                            # profile; acp@AGENT = settings acp.AGENT
tny --cwd DIR
tny --model ID
tny --effort LEVEL          # reasoning effort (--reasoning-effort is an alias)
tny --system-prompt TEXT    # custom system prompt (docs/adr/0045)
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

Color resolution ([ADR 0026](adr/0026-color-vs-attribute-sgr.md)): `NO_COLOR`
(any value, even empty) disables SGR *colors* only — bold/dim/reverse are
structural and stay, so the status bar keeps its reverse video.
`CLICOLOR_FORCE` (non-empty, not `0`) or `--color=always` forces full styling,
beating `NO_COLOR` and applying even when piped. `--color=never` /
`--no-color` emits no SGR at all.

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
5. `codex` if a `codex login` exists (`$CODEX_HOME/auth.json`, default `~/.codex/auth.json`) — subscriptions need no API key
6. `claude` if a Claude Code OAuth login exists (`CLAUDE_CODE_OAUTH_TOKEN`, or `~/.claude/.credentials.json` from `claude /login`; a bare `ANTHROPIC_API_KEY` never hijacks the default — use `--provider claude`)
7. `grok` if an xAI session exists (`~/.grok/auth.json`, from `tny
   --provider grok login` or the grok CLI)
8. `cursor` if `CURSOR_API_KEY` is set in the environment
9. `openai` (its connect error explains how to configure a key)

A settings.json object or `NAME_BASE_URL` env var named `claude` or `grok`
shadows the builtin profile entirely: explicit config wins. See
[settings.md](settings.md) and the published JSON Schema for `model`, `effort`,
`fast`, provider profiles, and named ACP-agent defaults.

## `tny login`

`tny [--provider NAME] login [--device]` signs in to the active provider.
tny never stores tokens itself:

| Provider | What login does |
| --- | --- |
| codex | Connects to `codex app-server` and calls `account/login/start` — the browser flow prints (and tries to open) the `authUrl`; `--device` uses the device-code flow and prints `verificationUrl` + `userCode`. tny pumps the socket until `account/login/completed`; the host writes `$CODEX_HOME/auth.json`, which tny auto-detects afterwards. Hosts without the RPC fall back to `codex login`. |
| claude | Reports the credential tny resolved (`CLAUDE_CODE_OAUTH_TOKEN`, `ANTHROPIC_API_KEY`, `~/.claude/.credentials.json`), else runs `claude setup-token`; the user exports the printed token as `CLAUDE_CODE_OAUTH_TOKEN`. |
| grok | Native RFC 8628 device-code sign-in against `auth.x.ai` — no grok CLI needed, works over SSH/containers ([ADR 0021](adr/0021-native-grok-device-login.md)). tny prints the verification URL + code, polls the token endpoint, and writes the session to `~/.grok/auth.json` in the grok CLI's own store format (both tools share the entry). `GROK_OAUTH2_ISSUER` / `GROK_OAUTH2_CLIENT_ID` override the endpoint (enterprise IdPs, tests). |
| cursor | Reports whether `CURSOR_API_KEY` is set. |
| openai / named | Reports whether an API key resolved (`tny setup` configures one). |

`tny logout` mirrors this: `codex logout` where the host CLI owns the
credential, native removal of the xAI entries from `~/.grok/auth.json` for
grok (foreign-issuer entries are kept), an env-var hint otherwise.

## System prompt

`--system-prompt TEXT` (leading global flag, headless and interactive) sets a
user system prompt for the run ([ADR 0045](adr/0045-system-prompt-flag.md)).
Providers with a schema field for it use that field: the openai-compatible
backend prepends the text to its system message (`instructions` on the
responses wire, the `system` role message on chat), ahead of tny's
operational preamble and the AGENTS.md chain. The host protocols expose no
such field on their pinned surfaces (cursor sdk.v1 `CreateAgent`, codex
`thread/start`, ACP `session/new`), so there the text is prepended to the
**first user message** of a fresh session, separated by a blank line.
Resumed sessions and later turns never get it again.

## Reasoning effort

`--effort` (env `TNY_REASONING_EFFORT`, TUI `/effort`) takes the canonical
levels `off | light | medium | high | xhigh | max` and maps them onto each
provider's wire vocabulary ([ADR 0009](adr/0009-reasoning-effort.md)):
codex `turn/start.effort`, cursor `ModelSelection.params`, openai
`reasoning.effort` (`reasoning_effort` on the chat wire). ACP has no
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

`--json` is required on `ask`, `status`, `doctor`, `permissions`, `models`, `session`, `sessions`, `workspace`, `usage`.

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

## Provider-specific flags

| Provider | Flags / env |
| --- | --- |
| cursor | `--bridge-bin PATH`, `CURSOR_SDK_BRIDGE_BIN`, `CURSOR_API_KEY` (also pass through to RPCs) |
| codex | `--codex-ws URL` to attach (attach-or-fail); without it tny first tries `TNY_CODEX_WS`, then a live registered host from `~/.tny/codex-host.json` (loopback only, written by whichever tny spawned the server — a running TUI, typically), and only then spawns `codex app-server` on an ephemeral port (never a fixed port that could collide). Discovery failures fall back to spawning silently (`docs/adr/0004`). `--codex-bin`, `--ws-token-file`, `CODEX_REMOTE_TOKEN` |
| acp | `--agent CMD` plus extra args after `--`, e.g. `tny --provider acp --agent gemini -- acp`; `--agent ws://host:port` connects to a remote agent instead of spawning ([ADR 0017](adr/0017-wasm-browser-parity.md)) |
| openai | `--base-url`, `--api-key-env NAME`, `--wire-api responses\|chat` (default `responses`; `chat` for legacy-only providers, [ADR 0016](adr/0016-responses-api-default-wire.md)), `OPENAI_BASE_URL`, `OPENAI_API_KEY`, `OPENAI_WIRE_API` |
| named provider | same flags; `NAME_BASE_URL` (beats the settings `base_url`), key from the profile's `api_key_env`, default `NAME_API_KEY` — never `OPENAI_API_KEY`; `NAME_WIRE_API` / profile `wire_api` |
| claude (builtin profile) | credential from `CLAUDE_CODE_OAUTH_TOKEN` > `ANTHROPIC_API_KEY` > `~/.claude/.credentials.json` (`$CLAUDE_CONFIG_DIR` honored); OAuth tokens add `anthropic-beta: oauth-2025-04-20`; chat wire; default model `claude-sonnet-4-6`; `TNY_CLAUDE_BIN` for login |
| grok (builtin profile) | session token from `~/.grok/auth.json` (minted by tny's native device login or the grok CLI; expired OIDC tokens auto-refresh at resolve) → CLI chat proxy (chat wire, `X-XAI-Token-Auth` + `x-grok-model-override` + `x-grok-client-version` headers — the proxy 426s unversioned clients, `TNY_GROK_CLIENT_VERSION` overrides the pin — default model `grok-4.6`); else `XAI_API_KEY` → `api.x.ai` (responses wire, same default model); `GROK_OAUTH2_ISSUER` / `GROK_OAUTH2_CLIENT_ID` override the login endpoint |

Model precedence for every provider: `--model` > saved `models.{provider}` >
the provider object's `model` (openai-compatible only) > `NAME_DEFAULT_MODEL`
from the environment (`CODEX_DEFAULT_MODEL`, `OPENROUTER_DEFAULT_MODEL`, …).

`tny ask` never blocks on an approval. Unresolved permissions fail the run unless `--auto` reviews (native loop) or `--yolo`. Host providers must be pre-authorized or they fail closed.

`--image PATH` (repeatable) attaches image files to the first user message as
`image_url` data URLs on the native OpenAI-compatible loop. The same encoding
is used when the model calls `read_image` mid-turn. Max 8 MiB; type comes
from magic bytes (png/jpeg/gif/webp), not the extension.

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
| codex | `thread/start` `serviceTier:"priority"` (same tier; the value every app-server release accepts) |
| cursor | `ModelSelection` param `{"id":"fast","value":"true"}` — fast is a per-model variant, not a request field |
| acp | not supported — `--fast` exits 1 with the capable provider list |

The interactive TUI exposes the same capability as `/fast [fast|priority|default]`.

Provider caveats: `--provider cursor` runs Cursor's own headless loop — the bridge exposes no per-call approval RPC, so tny's permission mode does not apply (a status line says so); it also rejects `--image`. `--provider codex` ignores `--image` with a status line (no documented image input item).

## `--max-steps` (agent loop cap)

The native loop runs for as long as the turn needs by default — no step limit
([ADR 0024](adr/0024-unlimited-steps-default.md)). `--max-steps N` caps model
calls per turn; a capped turn stops with "step limit reached" on stderr and
`tny ask` exits 2. `tny status --json` reports the cap as `agent_step_limit`
(`0` means unlimited).
`--max-steps unlimited` (or `0`) clears a cap a repo set through the
`.tny.json` `"steps"` limit. The interactive TUI exposes the same knob as
`/max-steps set N` / `/max-steps clear`. Host providers (cursor, codex, acp)
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
  --provider NAME cursor | codex | acp | openai | settings profile (--backend also accepted)

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
