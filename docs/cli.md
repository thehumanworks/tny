# CLI

### Explicit image preview

Image producers accept opt-in `--preview`; without it stdout and permission
identities remain metadata-only. `tny image preview --manifest RECORD --json`
selects a successful artifact without regenerating it. Native typed tools use
boolean `preview` and the `image_preview` selector; terminal interception has
the same semantics. Generation/export success stays exit 0 when preview fails,
with a separate JSON `preview` status and stderr guidance. Preview-only failure
exits 1. A queued receipt is not proof of delivery or inspection. No implicit
conversion, retry or manual attachment fallback is performed. Use
`image preview --job ID --item N` to select a succeeded image item, or add the
pair to `image edit` as its final reference within the existing five-reference
limit. The producer identity and attempts are pinned before permission; later
job/manifest changes cannot substitute another input. See [images](images.md).

Design the CLI so humans and coding agents can run it without menus. Every input is a flag or stdin. Interactive prompts are a fallback, never the only path. Each subcommand has `--help` with copy-paste examples.

Binary name: `tny`.

`score` and `choose` use the independent [tnyjev decision module](tnyjev.md).
They require `TYPESAFE_API_KEY`, accept explicit or piped state, and return
plain values or `--json`. They do not start a chat session or execute routes.

## Command tree

```text
tny                         # interactive TUI, fresh session
tny ask [prompt]            # one turn, then exit
tny speak                   # speak stdin aloud; --output-file exports MP3
tny score QUESTION          # Jev P(yes), 0..1; explicit state or stdin
tny choose --choices JSON   # Jev route key for state; no route execution
tny edit FILE               # exact-match replacement from stdin
tny ask-user QUESTION       # ask the owning runner frontend (inside terminal)
tny image generate          # prompt on stdin; --output-file required
tny image edit              # prompt on stdin; --image PATH and --output-file required
tny image export            # local resize/crop/re-encode; --image and --size required
tny image contact-sheet     # local ordered grid; repeat --image, --size required
tny image attach PATH       # attach an image to the next request (inside terminal)
tny jobs submit ask|image|batch   # durable work; prints a job id immediately
tny jobs status|wait|cancel|retry|logs|rm <id>
tny jobs list               # durable jobs in this workspace's state directory
tny resume [last|<id>]      # interactive resume
tny agents                  # all saved sessions; --json for scripts
tny web search QUERY        # override, else Codex login, else DuckDuckGo
tny web fetch URL           # bounded HTTP fetch
tny sessions
tny session last|<id>
tny session <id> --wait     # block until a background task finishes ([--timeout S])
tny session stop <id>       # stop a background task ([--kill])
tny providers               # list configured providers and doctor hints
tny tasks                   # list built-in and discovered task presets
tny task show NAME          # inspect one resolved preset
tny backends                # compatibility alias for providers
tny models
tny permissions
tny workspace list|add|remove|clear
tny status
tny doctor
tny login                   # provider-specific; see --provider
tny logout
tny setup                   # write provider config from flags/env
tny mcp [list]              # list configured MCP servers (source attributed)
tny mcp tools SERVER        # a server's tools with their argument names
tny mcp describe SERVER/TOOL # one tool's description and input schema
tny mcp call SERVER/TOOL    # one MCP tools/call; JSON arguments on stdin
```

Global flags are **leading**. `--cwd` selects the agent's local primary
workspace for both the TUI and headless CLI, independent of the directory from
which tny was launched. It accepts absolute or relative paths, and a leading
`~` or `~/` expands using the local `HOME` (not shell globbing or `~user`).
Unlike `--ssh-cwd`, it does not select a directory on the remote host. In the
wasm build it selects a directory in the browser's virtual filesystem; the
browser cannot access the host machine's directories.

```text
tny --provider openai|codex|grok|NAME [command]
                            # --backend is an alias; NAME = an OpenAI-compatible
                            # HTTP profile
tny --cwd DIR               # local primary workspace for TUI or headless CLI
tny --cwd '~/project'       # expand ~ to local HOME (quote to avoid shell expansion)
tny --worktree [NAME]       # create/enter ~/.tny/worktrees/NAME; random by default
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
For Codex ChatGPT subscription logins, `tny status` and `/status` also show the
weekly allowance left and its reset time. API-key logins are excluded.
See [subscription usage](backends/codex.md#subscription-usage-status-tny-status)
for the endpoint, JSON fields, and unavailable-data behavior.

`TNY_TOOLS=all|terminal+edit|terminal` overrides the `tools` user setting for
the native OpenAI-compatible loop. `all` is the unchanged default; the shell
profiles reduce both advertised and accepted built-ins as documented in
[Tools, MCP, skills, subagents](features/mcp-and-skills.md#native-tool-profiles).
`tny status` and `tny doctor` print the effective `tools` profile and expose it
as the JSON string field `"tools"`. libtny and wasm keep `all`; an
explicit profile ignored by wasm emits one status line.

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

Automatic workflow learning is **on by default**, without selecting a preset.
`--no-self-improve`, `TNY_SELF_IMPROVE=0`, or `"self_improve": false` disables it.
`status --json` and `doctor --json` expose `self_improve`. See
[instruction improvement](instruction-improvement.md) for scope and evidence.

The bundled `self-improve` preset guides optional broader instruction experiments:
`tny --task self-improve ask "Design an experiment for our review task"`.
It does not start optimization or grant promotion authority by itself. See
[instruction improvement](instruction-improvement.md) for the optional workflow,
independent evaluator contract and published benchmark limitations.

The bundled `task-creation` preset lets an agent author tasks for you:

```sh
tny --task task-creation ask "Create a task named release-review for reviewing release readiness"
# With a specific provider and model:
tny --provider aiproxy --model grok-4.6 --task task-creation ask \
  "Create a task named release-review for reviewing release readiness"
tny task show release-review
tny --task release-review ask "Review this release"
```

In the TUI, select `/task task-creation` in a fresh session, then describe the
task you want. The preset guides the agent to create `.tny/tasks/NAME.md` in the
workspace, or `~/.tny/tasks/NAME.md` when you request a user-wide task, and to
validate its resolved contents with `tny task show` and `tny tasks`. It explains
the supported format and asks the agent to preserve existing definitions and
avoid accidental shadowing. These are authoring instructions subject to the
selected model and normal tool permissions; they do not add a new write API
or grant authority. The authored workflow runs only when requested. Start a
new invocation, or `/new` followed by `/task NAME`, to use it.

When filesystem or CLI access is unavailable, the agent can provide the exact
Markdown to save and identify the unverified steps. Browser files are
temporary MEMFS files, not persistent host tasks; Node wasm uses the host
filesystem. Over SSH, custom task discovery remains unavailable. See
[ADR 0112](adr/0112-bundled-task-creation.md) and the filesystem clarification in
[ADR 0113](adr/0113-task-creation-filesystem-clarification.md).

Task instructions follow `--system-prompt` additions in the native system context. They do not modify the user message.

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
```

The working transcript remains in memory for multi-turn TUI use, but tny
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

- Works with all native HTTP profiles, including `openai`, `codex`, `grok`, and configured gateways.

wasm behavior: remote-only — the browser build has no `ssh` to spawn, so
`--ssh` fails at connect with a clear error.

## Provider selection

`--provider NAME` (`--backend` alias) selects `openai`, `codex`, `grok`, or a
named OpenAI-compatible HTTP profile. Names come from a settings object with
`base_url` or a `NAME_BASE_URL` environment variable. Both Responses (default)
and Chat Completions (`wire_api: "chat"`) use the native tools/MCP/session loop.
No vendor agent binary is required.

Precedence: explicit flag, settings `provider`, remembered `last_provider`
(`last_backend` compatibility alias), OpenAI env, exactly one complete named
env pair, native Codex OAuth credentials, Grok OAuth credentials, then openai.
An explicit or remembered unknown/removed selector fails rather than falling
back. Installed binaries and Claude auth artifacts do not affect selection.

### `tny provider setup`

```sh
tny provider setup openrouter --base-url https://openrouter.ai/api/v1 --api-key-env OPENROUTER_API_KEY
tny provider setup aiproxy --base-url https://your-gateway.example/v1 --api-key-env AIPROXY_API_KEY
tny provider setup  # interactive environment-variable name prompts
```

AIProxy's example URL is a placeholder: supply your gateway URL. Setup stores
only configuration and environment-variable names, never API keys. Existing
`api_key` settings and `--api-key` persistence fail with a migration diagnostic;
export the key, set `api_key_env`, and delete the stored key. Generic auth header
and wire options remain available. `/provider setup [NAME]` is the TUI flow.
The browser may still accept a key into the tab's ephemeral environment.

Optional ACP clients use `tny --agent claude-agent-acp --model sonnet ask "hello"`
or named `--provider acp@NAME` profiles (see [settings](settings.md#optional-acp-client-profiles)).
For literal adapter arguments, use `--agent executable -- arg1 arg2 -- ask "hello"`.
There is no shell evaluation. Adapter subprocesses start on explicit model
catalog discovery or a turn, never help/version or provider selection. The
external adapter owns account authentication, so Claude account access uses
`claude-agent-acp` with its existing Claude login. Native HTTP remains usable
without any vendor executable.

ACP server mode (`tny acp`), Cursor bridge (`tny cursor`, `--bridge-bin`), and
the built-in Claude HTTP subscription profile remain removed. See the
[ACP client capability matrix](backends/acp.md) for negotiated model/effort,
MCP tool access, resumption, and adapter-specific SSH support.

## `tny login`

`tny [--provider NAME] login [--device]` signs in to the active provider.
Native OAuth logins persist refreshable tokens; BYOK API keys remain in env:

| Provider | What login does |
| --- | --- |
| codex | Native ChatGPT sign-in, no Codex CLI ([ADR 0066](adr/0066-native-chatgpt-login-and-credential-sources.md)): the browser PKCE flow with a `localhost:1455` callback (the redirect URL can also be pasted into the terminal), or `--device` for a verification URL + one-time code on headless machines. The login lands in `~/.tny/codex-auth.json` (`0600`), which tny reads for the ChatGPT Responses backend and refreshes itself. `$CODEX_HOME/auth.json` from `codex login` keeps working as a fallback. |
| grok | Native RFC 8628 device-code sign-in against `auth.x.ai` — no grok CLI needed, works over SSH/containers ([ADR 0021](adr/0021-native-grok-device-login.md)). tny prints the verification URL + code, polls the token endpoint, and writes the session to `~/.grok/auth.json` in the grok CLI's own store format (both tools share the entry). `GROK_OAUTH2_ISSUER` / `GROK_OAUTH2_CLIENT_ID` override the endpoint (enterprise IdPs, tests). |
| openai / named | Reports whether an API key resolved (`tny setup` configures one). |

`tny logout` mirrors this: native deletion of `~/.tny/codex-auth.json` for
codex (the Codex CLI's own file is left to `codex logout`), native removal of the xAI entries from `~/.grok/auth.json` for
grok (foreign-issuer entries are kept), an env-var hint otherwise.

## System prompt

`--system-prompt TEXT` adds a user system prompt ahead of the operational preamble and AGENTS.md chain: `instructions` on Responses, a system message on Chat Completions.

## Reasoning effort

`--effort` (env `TNY_REASONING_EFFORT`, TUI `/effort`) accepts `off | light | medium | high | xhigh | max`. The native backend maps this to `reasoning.effort` on Responses and `reasoning_effort` on Chat Completions.

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
convenient from a shell or another coding harness. Local replacement preserves
read/write/execute permission bits, including executable scripts, independently
of umask. It does not copy set-ID/sticky bits, ownership, ACLs or extended
attributes. A metadata error leaves the original untouched
([ADR 0134](adr/0134-preserve-edit-permissions-and-require-integration-tests.md)):

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
tny --provider openai --model gpt-5.4 ask "find the login bug"
tny --provider codex --effort xhigh ask "prove this queue is lock-free"
tny --yolo --cwd /tmp/ws ask "run the test suite"
```

Stdout: assistant Markdown (or one JSON object with `--json`, or one canonical
event per line with `--events=jsonl`).
Stderr: progress, tool lines, diagnostics.
Exit 0 finished, 1 startup/config, 2 run failed, 130 interrupted.

Token/context usage is silent by default. `--print-usage` (or
`TNY_PRINT_USAGE=1`) reports it on stderr, always on its own line after the
answer.

Native `ask --json` results include `usage`: `input_tokens`, `output_tokens`,
`requests`, `cached_input_tokens`, `uncached_input_tokens`, and
`cache_write_tokens`, summed across the turn's tool rounds and retries that
reported usage. `requests` counts those reported responses. A missing cache
breakdown produces `null`, not a measured zero; `usage` itself is `null` when
no native response reported usage. These fields also appear in background
results. The existing usage event reports the turn totals and the latest
request's input-token count; the public C event layout is unchanged.
See [ADR 0077](adr/0077-openai-prompt-cache-routing.md).

OpenAI cache routing groups related tasks by workspace and tool profile.
`TNY_OPENAI_CACHE_SCOPE=session` restores per-conversation routing, useful
when a busy workspace's requests compete for cache capacity. This affects
cache affinity only; conversations and their saved transcripts remain
independent. See [ADR 0078](adr/0078-workspace-shared-prompt-cache.md).

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

### `--events=jsonl` (canonical event stream, [ADR 0090](adr/0090-canonical-foreground-ask-events.md))

```sh
tny ask --events=jsonl "summarize this repository" | jq -c 'select(.type=="tool_end")'
tny ask --events=jsonl --progress=none --ephemeral "list the public CLI"
```

Stdout becomes the turn's event stream: one JSON object per line, in engine
order, and nothing else — no Markdown, no recovery replay, no final blob. The
objects are the public event schema of
[ADR 0030](adr/0030-public-event-schema.md) (`sdk/schema/events.json`), the
same events `libtny` hands to the Python and TypeScript SDKs:

```json
{"schema_version":1,"sequence":7,"timestamp_ms":1234,"provider":"openai",
 "session_id":"…","turn_id":"…:1:0","type":"turn_end","kind":7,"stop_reason":0}
```

- Envelope on every line: `schema_version`, `sequence` (monotonic, session
  local — it does not start at 1), `timestamp_ms` (monotonic), `provider`,
  `session_id`, `turn_id`, `type`, and the numeric `kind`.
- Payload keys are the registry's fields for that `type`, always present.
  An unavailable string is `""`; an unreported `cost` is `null` with
  `has_cost:false`. `kind`, `stop_reason`, `permission_options` and
  `error_code` are the frozen ABI's numbers (`include/tny/tny.h`), never
  re-spelled names.
- `--ephemeral` streams the same way; its `session_id` is an in-memory id
  that is never persisted (the `--json` blob reports `""` for the same run).
- The turn runs in this process (like `TNY_ISOLATE=0`), because the detached
  runner speaks its own private wire; `-B`/`--background` and `--json` are
  therefore refused rather than silently reinterpreted.

Exit codes follow the delivered stream, not optimism: `0` only after an
actual `turn_end` with stop reason `done` was written; `2` when the stream
could not be written, when the backend drained without a terminal event, or
for any non-success stop reason; `130` when an interrupt reached the turn —
including an interrupt while a stalled consumer was blocking the writer. A
missing `turn_end` is never invented, and success is never reported after a
write failure. The turn's real output still reaches the saved session
`result`, with the honest status and exit code.

Failures before the turn is accepted (option conflicts, unreadable schema,
missing session, no prompt) print one stable object on **stderr** and no
events at all:

```json
{"schema_version":1,"kind":"ask_error","code":"option_conflict","message":"…"}
```

`code` is one of `invalid_option`, `option_conflict`, `no_prompt`, `session`,
`session_busy`, `provider`, `internal`, `start_failed`, plus the post-start
stream outcomes `stream_io`, `no_terminal` and `cancelled`. The object is a
CLI diagnostic (`kind`), never an event (`type`), so the two cannot be
confused.

`--progress=none` is independent of `--events`: it drops the successful human
status, plan, tool and extension lines from stderr. Errors, permission
refusals and the machine diagnostics above still print. It works with the
default Markdown output and with `--json` too.

A slow consumer only slows delivery: the writer waits in bounded slices,
keeps event order, retains no more than the engine's bounded event queue, and
never changes the file-status flags of the stdout description it inherited.
Every chunk goes out only after the poll seam reports stdout writable, so an
interrupt is never waiting behind a blocking write — including on a stdout
that was *already* full before the first event. wasm: the browser build's
seam answers for its own stdout (`Module.__tnyPollStdout`, ready by default
because the page's print hooks and node's `writeSync` do not defer), so the
writer asks there too instead of guessing.

An interrupt also reaches a `terminal` tool that is still running: the
command and the processes it started are stopped, the tool result reports
`exit code: 130` with a cancellation line, and the turn ends `interrupted`
(exit `130`). Background terminal results now provide an opaque `task_id` and
structured [inspect/wait calls](features/mcp-and-skills.md#background-terminal-completion),
not a PID to poll. Wait deadlines and observation cancellation do not cancel
the task. A `background: true` terminal command is deliberately detached
and keeps running, as it does for any other turn outcome.

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
root and that its magic bytes identify png/jpeg/gif/webp, then queues its
**loaded bytes** as user-role image content for the next native provider
request (ADR 0008, ADR 0096). `--json` emits `kind: "ask_user"` or
`kind: "image_attach"` plus the request's string correlation id.

The channel also carries a third tool-role operation, `image_preview`, for an
explicitly requested generated-image preview
([ADR 0096](adr/0096-captured-image-queue-and-preview-lifecycle.md)). It takes
`id`, `path` and a 64-character lowercase hex `expected_sha256`, and it is a
distinct operation: it never falls back to `image_attach`. The receiving runner
decides the outcome — allowed roots, the owning turn's readiness, the
configured-true `image_input` policy and the hash of the bytes it just captured
— and answers the existing `ok`/`error` fields plus two **optional** additions,
`status` (`queued`, `unsupported`, `unavailable_session`, `turn_not_ready`,
`failed`) and a safe `error_code`. Existing clients and the `ask_user` /
`image_attach` replies are byte-for-byte unchanged. There is no acknowledgment
retry: a socket that dies after the request was written is unknown delivery,
never success and never a second enqueue. No shipped command sends
`image_preview` yet; `tny image generate --preview` arrives with the preview
integration slice.

Interactive questions use the native runner question channel. Headless calls use the documented `ask_user_question` fallback string.

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

### `tny mcp tools SERVER` / `tny mcp describe SERVER/TOOL`

```text
tny mcp tools fs
tny mcp describe fs/read_text_file
tny --json mcp describe fs/read_text_file
```

The server's cached `tools/list`, so a caller can shape arguments from the
contract instead of guessing ([ADR 0068](adr/0068-mcp-tool-schema-discovery.md)).
`tools` prints one line per tool — `server/tool — description` — followed by
an indented `arguments:` summary (`name* (type)`, `*` = required, `none`, or
`unknown (no input schema published)`). `describe` prints the same for one
tool plus the full `input schema:` JSON. `--json` emits
`{"kind":"mcp_tools","server":…,"tools":[{"name","description","input_schema"}]}`
or `{"kind":"mcp_tool","server":…,"tool":…,"description":…,"input_schema":…}`,
the schema verbatim (`null` when the server publishes none). Neither verb
calls the tool. Outside a session the server cold-starts and is shut down
again; inside tny's `terminal` tool the warmed client answers in-process, and
the permission identity is the native `mcp_search_tools` meta-tool. Exit
codes: 0; 1 usage or unknown server; 2 a tool the server does not list.

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
- **A failed call shows the contract.** When the server answers with an
  error or `isError: true` and lists the tool, a second stderr line prints
  `input schema for SERVER/TOOL: {…}` and the `--json` object gains
  `"input_schema"`, so the retry is shaped by the schema rather than another
  guess ([ADR 0068](adr/0068-mcp-tool-schema-discovery.md)). Success, a
  permission refusal, and a tool the server never listed carry no hint.
- **wasm:** HTTP MCP servers work (remote-only, subject to CORS); a stdio
  server keeps the existing clean spawn error.

## Quick ephemeral questions from Zsh

After `make install PREFIX="$HOME/.local"`, add this **after** your plugins
and keymap setup in `~/.zshrc`:

```zsh
source "$HOME/.local/share/tny/tny.zsh"
```

Type your question directly at the shell prompt. Press **Ctrl-X**, release
Ctrl, then press **`a`**. Do not press Enter to submit the question: Enter
still executes ordinary shell commands. No quotes or prefix are needed. Punctuation and pasted multiline text are sent literally through
`tny ask --ephemeral --stdin`.

Answers stream in place. Success clears the input; failure or Ctrl-C keeps
it for retry with the same binding. Press Ctrl-C at the shell prompt to clear
it. Empty input does nothing; cancel an unfinished shell command before asking
from a secondary prompt. The binding works in emacs and both vi keymaps.
The prompt returns below the complete answer, including with multiline shell
prompts and wrapped output ([ADR 0073](adr/0073-quick-ask-preserves-rendered-output.md)).

The widget uses your configured provider/model and current directory. Optional
Zsh settings (an executable path and an array, never an evaluated string):

```zsh
TNY_BIN="$HOME/.local/bin/tny"
TNY_ASK_FLAGS=(--provider codex --model gpt-5.6-luna)
# Optional alternative binding, after sourcing:
bindkey -M viins '^Xq' tny-ask
```

Prompts do not enter normal Zsh history or tny's saved conversations. Terminal
scrollback and provider retention still apply; tools retain normal tny
permissions. The script installs Ctrl-X then a in the emacs, viins, and vicmd
maps, replacing an existing binding for that sequence. It does not change
Enter or select a different editing mode. It is native Zsh integration;
Bash/Fish and browser wasm users can use the regular ephemeral CLI.
[Decision and alternatives: ADR 0072](adr/0072-zsh-ephemeral-quick-ask.md).

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
tny_task test-plan --provider openai -- "Design the missing tests"
tny_task implement --after inspect --after test-plan --   "Implement and verify using both reports"

tny_workflow_run --jobs 2
tny_result implement
```

Shell tasks are ephemeral by default. The helper exposes the normal provider,
model, effort, workspace, permission, and SSH selections; it does not
implement provider behavior itself. Full API and failure semantics:
[workflows.md](workflows.md).

## Process isolation (every turn, [ADR 0053](adr/0053-forked-turn-isolation.md))

On native builds **every** turn — foreground `tny ask` and the TUI included
— executes in a detached session-runner process; the invoking `tny` is only
a renderer streaming the runner's events from `<session-dir>/sock`. A caller
crash or SIGKILL detaches the turn: the runner finishes, finalizes the session's
`status`/`exit_code`/`result`, and exits, so `tny ask --resume <id>` (or
`tny resume`) continues the conversation afterwards. `^C` in a foreground
`ask` cancels the turn; a second `^C` forces termination. Cancellation also
escalates automatically after five seconds. Foreground SIGHUP/SIGTERM and
TUI Ctrl-D/quit/EOF stop the run using bounded shutdown. Explicit `ask -B`
and observer detach still leave their runs active. See
[ADR 0081](adr/0081-reliable-session-interruption.md).
In-process turns remain only on wasm, with `--ephemeral`, or with the
`TNY_ISOLATE=0` debug escape hatch.
`TNY_THREADS=1` keeps the bounded per-file fan-out of `grep_files`,
`semantic_search` and image source loading on the calling thread
([ADR 0132](adr/0132-bounded-fan-out-for-independent-file-work.md)); the
output is identical either way, only the wall-clock time differs.

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

## `tny agents` and active-turn backgrounding

Press **Left with an empty composer** to open the agents dashboard. During an
active saved native turn, the runner saves its background marker and acknowledges
detachment immediately, while the same HTTP or ACP stream or tool keeps running.
Repeated Left is idempotent. There is no tool-boundary wait, restart, repeated
prompt or step-limit reset. Idle Left opens the same dashboard. Nonempty drafts
and focused modal/question/permission inputs retain ordinary cursor editing.
See [ADR 0166](adr/0166-global-sessions-and-immediate-backgrounding.md).

A successful handoff opens the same dashboard as `tny agents`. Workspace sections
show the current cwd first, then other paths alphabetically, with newest sessions
first within each section. Type to fuzzy-filter directory paths; matching is
case-insensitive and allows gaps between characters. Backspace edits the filter;
Esc clears it, then exits when empty. Ctrl-C/D exit directly, while `q` is ordinary
search text. Up/Down selects a session beneath its directory heading; headings
are not selectable. The selection survives refreshes and stays visible while
scrolling. No matches leaves no selectable session. Enter attaches when a live
runner accepts the unique owner handshake,
including halfway through a turn or while idle after completion; the active turn
is not reposted. Otherwise it opens a labeled **saved read-only transcript**, even
when a rival owns the connection or a held-lock runner is unreachable. Completed
unlocked rows also open read-only. Opening the saved view does not resolve a
provider, refresh credentials, start a runner, activate a checkpoint, or save
session/settings/auth stores. Unavailable provider configuration does not prevent
inspection.

Without a saved checkpoint, submit a prompt to explicitly continue the selected
session with its existing ID and history. `/continue` in that background view
acquires/attaches the same session. A held lock permits an owner-handshake attempt,
not takeover; a rival or unreachable owner refuses execution and leaves inspection
retryable. New execution requires acquiring the writer lock, reloading the saved
conversation under it, and resolving the selected provider/model/workspace.
Stored completion is not
writer quiescence ([ADR 0104](adr/0104-runner-quiescence-ownership.md)). Saved
checkpoints require `/continue`, not a typed prompt, as described below.

`tny agents --json` (or non-TTY plain output) lists **all saved local sessions**
under the user's tny state directory without starting a provider. This includes
foreground and background sessions from unrelated repositories, linked worktrees,
and non-Git directories, regardless of the launch cwd. It does not impose the
`tny sessions` page limit. Each JSON row includes `workspace` and its physical
`workspace_bucket`; plain rows show it alongside the session, while the TUI groups
rows beneath directory headings. A saved foreground session with no status
field is labeled `saved`; a stored running session with no live writer is `stale`.
Inspection uses the selected session's saved text, without provider resolution.
Live attachment retains the runner's permission mode, pending decision, model and
workspace. New execution runs in the selected session's original workspace, using
its saved provider and model plus current settings and launch flags for other
options. Legacy missing metadata uses the
[existing fallbacks](features/sessions.md#explicit-continuation-and-ownership),
not reconstructed historical configuration. A legacy row without workspace metadata remains
viewable from anywhere; it explicitly labels the workspace unknown and identifies
the current cwd that continuation will use. Its physical storage bucket and session
ID are preserved. A session with an attached owner can
be inspected but not taken over; detach its owner first to make owner attachment
available. This lists local tny sessions only.

`tny agents --run RUN_ID` instead shows the task tree of an opt-in durable DAG
job. The run ID is the job ID, not an arbitrary session ID. Each row shows its
stable task index, lead/worker role, label, execution state and separate
verification state. This view is status-only: Up/Down scrolls and q leaves the
run active; Enter does not launch or resume a task. With `--json`, the result is
`{"kind":"agents","run":{...}}`, where `run` is the authoritative jobs status
record. Ordinary batches and invalid IDs are refused. The existing unfiltered
session dashboard and JSON shape are unchanged. See [durable jobs](jobs.md).

The saved read-only view allows only `/help`, `/clear`, `/transcript`, `/copy`,
`/trace`, `/agents`, `/quit`, `/exit` and `/continue`; `/cancel` requires an attached
runner. Other commands, including session mutations, settings/provider switches
and auth commands, are blocked. Attached background replicas also block direct
mutations; authorized turn controls use the runner connection instead. `/clear`
only clears the display. Quit from a background view or dashboard detaches without
saving the replica and leaves work running. Ctrl-C explicitly cancels only an
attached turn; `tny session stop ID --kill` remains a separate CLI operation.
Handoff-origin ask/auto permissions wait up to five minutes for owner
reattachment, then report timeout/denial. Ordinary unattended `tny ask -B`
continues to deny an unanswered permission promptly. Foreground exit/interrupt
behavior is unchanged. Handoff and new-runner continuation from a saved background
view are unavailable in wasm and in-process modes, with refusal before mutation;
there is no unlocked in-process fallback. A successful live attachment can remain
usable when starting a replacement runner is unavailable. Ephemeral mode cannot
open saved sessions. See [ADR 0166](adr/0166-global-sessions-and-immediate-backgrounding.md).

**Selecting a saved handoff checkpoint does not activate it.** While a saved
checkpoint is present, including consumed or invalid checkpoints, a typed prompt
is refused: it is **not submitted or queued**. Use `/continue` in that background
view to validate the checkpoint through existing recovery; consumed or invalid
work is rejected. Recovering valid unconsumed work adds no new user message and
may release retained tool calls and other effects. Recovery requires compatible
original configuration. CLI `tny resume ID` is unchanged: it remains an explicit
recovery entry point without an extra prompt.
Outside a background view, `/continue` still resumes the latest workspace session.
`agents --json` reports `running` for active work and `live` for a held writer,
not a guarantee that its owner slot is available; a completed live runner still
reports `status:"done"`.

See [ADR 0108](adr/0108-checkpoint-recovery-and-hosted-tool-boundaries.md) for
recovery validation and hosted-tool boundaries, and [ADR 0154](adr/0154-agents-session-inspection-and-continuation.md)
for the explicit inspection/continuation boundary. No force-takeover control or
new background daemon is added.

## `tny web search|fetch`

`tny web search "C11 atomics"` uses an explicit command or URL search override,
else the Codex/ChatGPT login, else DuckDuckGo when no such login exists. This
selection does not depend on the conversation provider or model. The search-only
Codex model defaults to `gpt-5.6-sol`; `web_search_model` overrides it independently.
`web_search_timeout_seconds` sets its 1–300 second deadline (default 120), including
pumped token refresh. An invalid login or failed Codex request is an error, not a
silent fallback. API-key-only Codex auth is not a ChatGPT subscription login. `tny web fetch https://example.com` fetches
one URL. `--json` returns kind, ok and result; errors exit 2. Query templates are
percent encoded. Search challenges and incomplete responses are reported rather
than presented as search results. These verbs also execute in process when
called through the native terminal tool, preserving its permission identity.
Builtin Codex retains inline hosted search; the explicit CLI always uses the
independent search service and labels Codex or DuckDuckGo provenance. `web` applies
local settings/permissions and ChatGPT flags without selecting or refreshing an
unrelated chat provider. See [ADR 0109](adr/0109-provider-independent-codex-search.md). See [search providers](features/mcp-and-skills.md#web-search-providers).

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

Stops any running session by signaling its **process group**
(SIGTERM): the turn cancels cleanly, spawned backend hosts die with the
group, and the session finalizes `status:"interrupted"` with partial output
preserved. On a finished session `stop` is a clean no-op that reports the
status. If the child ignores SIGTERM, `stop` reports a timeout and suggests
`--kill`, which SIGKILLs the group and writes the terminal status on the
child's behalf after verifying writer-lock release. On macOS/Linux the force
step also kills descendants in separate process groups, including spawned
hosts and tools. No successful stop is reported while the writer lock remains
held. `--json` emits `{"kind":"session_stop","status":…}`.

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

## Provider-specific flags

| Provider | Flags / env |
| --- | --- |
| codex (builtin profile) | credential precedence `--chatgpt-token` (+ `--chatgpt-account-id`) > `CHATGPT_ACCESS_TOKEN` (+ `CHATGPT_ACCOUNT_ID`) > `~/.tny/codex-auth.json` (`tny --provider codex login`) > `$CODEX_HOME/auth.json` (`codex login`); the winning file auto-refreshes in place, flag/env tokens need no filesystem; account id explicit or from the JWT claim → `https://chatgpt.com/backend-api/codex` on the Responses wire with `chatgpt-account-id` + `OpenAI-Beta: responses=v1`; an `OPENAI_API_KEY` auth.json → `api.openai.com`; default model `gpt-5.6-sol`; `TNY_CODEX_BASE_URL` redirects the ChatGPT-mode URL (mocks/gateways) without shadowing the profile ([backends/codex.md](backends/codex.md)) |
| openai | `--base-url`, `--api-key-env NAME`, `--wire-api responses\|chat` (default `responses`; `chat` for legacy-only providers, [ADR 0016](adr/0016-responses-api-default-wire.md)), `OPENAI_BASE_URL`, `OPENAI_API_KEY`, `OPENAI_WIRE_API`. `--base-url-env NAME` reads the base URL from environment variable `NAME` with `--base-url` precedence, keeping a secret-bearing gateway URL off argv (native subagent children use it, [ADR 0087](adr/0087-explicit-subagent-contract-and-private-launch.md)); an empty `NAME` or combining it with `--base-url` is a startup error (exit 1) |
| named provider | same flags; `NAME_BASE_URL` (beats the settings `base_url`), key from the profile's `api_key_env`, default `NAME_API_KEY` — never `OPENAI_API_KEY`; `NAME_WIRE_API` / profile `wire_api` |
| grok (builtin profile) | session token from `~/.grok/auth.json` (minted by tny's native device login or the grok CLI; expired OIDC tokens auto-refresh at resolve) → CLI chat proxy (chat wire, `X-XAI-Token-Auth` + `x-grok-model-override` + `x-grok-client-version` headers — the proxy 426s unversioned clients, `TNY_GROK_CLIENT_VERSION` overrides the pin — default model `grok-4.6`); else `XAI_API_KEY` → `api.x.ai` (responses wire, same default model); `GROK_OAUTH2_ISSUER` / `GROK_OAUTH2_CLIENT_ID` override the login endpoint |

Model precedence for every provider: `--model` > saved `models.{provider}` >
the provider object's `model` (openai-compatible only) > `NAME_DEFAULT_MODEL`
from the environment (`CODEX_DEFAULT_MODEL`, `OPENROUTER_DEFAULT_MODEL`, …).

`tny ask` never blocks on an approval. Unresolved permissions fail the run unless `--auto` reviews (native loop) or `--yolo`.

`--image PATH` (repeatable) attaches image files to the first user message.
The native OpenAI-compatible loop uses `image_url` data URLs. The native loop
also uses the encoding when the model calls `read_image` mid-turn. Max 8 MiB;
type comes from magic bytes (png/jpeg/gif/webp), not the extension. At most 16 `--image`
flags are accepted. A 17th prints `tny: too many --image flags (max 16)` and
exits 1 before any image file is opened or a backend is connected. That
startup path frees the prompt buffer it may already have allocated, matching
the `--output-schema` and unknown-flag error returns
([ADR 0008](adr/0008-native-loop-images.md)).

Settings may declare per-provider image input. When
`"image_input": {"NAME": false}` covers the effective provider, `--image`
fails with exit 1 and `tny: image input is disabled for this provider by
settings.json image_input` before a session is created or the provider is
contacted, `read_image` is neither advertised nor executable, and queued
images are never flushed. `true` means *configured, unverified* — tny checks
no live entitlement — and absent means unknown, which keeps the existing
explicit behavior. See [images.md](images.md#conversation-image-input-image_input)
and [ADR 0089](adr/0089-image-input-policy-and-shared-gates.md).

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

The interactive TUI exposes the same capability as `/fast [fast|priority|default]`.



## `--max-steps` (agent loop cap)

The native loop runs for as long as the turn needs by default — no step limit
([ADR 0024](adr/0024-unlimited-steps-default.md)). `--max-steps N` caps model
calls per turn; a capped turn stops with "step limit reached" on stderr and
`tny ask` exits 2. `tny status --json` reports the cap as `agent_step_limit`
(`0` means unlimited).
`--max-steps unlimited` (or `0`) clears a cap a repo set through the
`.tny.json` `"steps"` limit. The interactive TUI exposes the same knob as
`/max-steps set N` / `/max-steps clear`. The cap applies to every native HTTP profile.

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
  --provider NAME openai | codex | grok | settings profile (--backend also accepted)

Examples:
  tny ask "explain src/main.c"
  tny ask --json --ephemeral "list exported symbols"
  tny --provider openai --model gpt-5.4 ask "fix the leak"
```

Missing required values print the error, then a correct example, then exit 1. No timed prompts.

### Tested contract

`make test-help-flags` extracts accepted long and short flags from the C argv
parsers and compares them with `tny --help` plus every subcommand's `--help` in
both directions. A parser flag without help text, help text without a parser,
or a dispatched subcommand missing from top-level help fails the test
([ADR 0042](adr/0042-help-flag-alignment.md)). The small source allowlist is
reserved for explicitly justified compatibility or passthrough syntax.

## Dictation

`tny dictate` records the microphone and prints prompt text. Enter finishes
recording; Ctrl-C cancels. `--seconds N` enables timed capture;
`--input-file speech.wav` transcribes a PCM WAV without a microphone.
`--stt-provider codex|xai` is independent of the agent's `--provider`.
`TNY_STT_PROVIDER` sets the STT default; without it, `codex` remains the default.
The leading global `--xai-api-key KEY` works for standalone and TUI dictation:

```sh
tny --xai-api-key "$XAI_API_KEY" dictate --stt-provider xai --input-file speech.wav
TNY_STT_PROVIDER=xai tny --provider grok
# Inside the TUI: /dictate xai, then Enter to transcribe, then Enter to send.
```

xAI credential precedence is flag, `XAI_API_KEY`, named `xai` settings
(`api_key_env`), then Grok login only when no explicit key source is configured.
A missing explicit environment variable, empty or CR/LF-bearing value fails locally. Grok login credentials refresh only at
transcription start. STT uses `https://api.x.ai/v1/stt` with its service-selected
model; chat/profile base URLs and models are ignored. `--check` opens no audio,
makes no request, and performs no refresh; success means local prerequisites,
not verified entitlement. Windows/wasm remain file-only.
`--json` returns one object with `kind`, `provider`, and `text`.
See [Dictation](dictation.md) for account credentials, devices, bounds,
cancellation, and platform support.

## Prompt optimisation

`tny optimise PROMPT` rewrites a draft using relevant project files, and prints
the result without executing it. `--stdin` accepts a piped draft, including
`tny dictate --seconds 10 | tny optimise --stdin`. `--model` and `--provider`
override the independent default: OpenRouter `inception/mercury-2.5`.
`--json` prints `kind`, `provider`, `model`, and `text`.
`tny optimise --optimise-timeout 600 PROMPT` sets the timeout in seconds.
The default is 300 seconds; valid values are integers from 1 to 86400.
Precedence: this subcommand flag, `TNY_OPTIMISE_TIMEOUT`, project `.tny.json`
`optimise.timeout_seconds`, user settings, then the default. Invalid selected
values fail before a provider request. There is no step cap, even if the
conversation or project sets one; cancellation and the timeout still apply.
The TUI offers `/optimise PROMPT` and Ctrl-O, followed by draft review and
explicit Enter to submit. See [Prompt optimisation](optimisation.md).

## Speech

`printf 'The tests passed.' | tny speak` plays ephemeral speech using your
ChatGPT login, independently of the chat provider. See [Speech](speech.md)
for voices, availability, export, agent tools and platform behavior.

## Durable jobs (`tny jobs`)

`tny jobs submit ask --prompt "…"` (or a piped prompt) returns a 32-hex job id,
its private metadata path and the per-item log paths immediately, then a
detached supervisor runs the work as real `tny ask --events=jsonl` /
`tny image … --json` children. `tny jobs submit batch --request FILE` takes a
bounded JSON document: one kind, 1–64 items, concurrency 1–16.
`status`/`wait`/`logs`/`list` read; `cancel`/`retry`/`rm` change state, each
with its own permission identity. `wait --timeout S` exits 124 without
cancelling. A job whose supervisor is gone reads as `interrupted` with
`cleanup:"unknown"`, never as succeeded, and `retry --failed` verifies every
carried successful session, log and artifact hash before spending anything.
Not available in the browser build, where the execution operations refuse
before any side effect. See [jobs.md](jobs.md) and `tny jobs --help`.

## Image generation, editing and local exports

`printf 'An orange robot' | tny image generate --output-file robot.png --json`
generates one image using the ChatGPT login.
`printf 'Make it blue' | tny image edit --image robot.png --output-file blue.png`
edits from local references. `--image-provider` selects independently of the
chat provider; Codex defaults to `gpt-image-2.5-sunburst` with `high` quality.
Use `--model gpt-image-2.5-flare` to override the image model and `--quality`
to select `auto`, `low`, `medium`, `high`, `xhigh`, or `max`.
`--check` checks local credentials
without a request. Neither operation needs a runner socket.

`tny image export --image photo.jpg --output-file thumb.png --size 256x256`
resizes, crops, pads or re-encodes one local image, and
`tny image contact-sheet --image a.png --image b.png --output-file sheet.png
--size 512x256 --columns 2 --labels numbers` composes an ordered grid. Both are
local and explicit: they need the optional ImageMagick 7 `magick` executable on
`PATH`, while generation, editing and replay never invoke it. The canvas is
always exactly `--size`, an existing destination needs `--overwrite`, sources
are never modified, and the result is a derived artifact recorded as such. Run
`tny image export --help` for the full option set. See
[images.md](images.md) for flags, result schema, limits, permissions and platform
behavior, and `tny image --help` for examples.

### Collective mode

`--swarm[=N]` (global or ask-local) and interactive `/swarm [N]` select collective
facilitation. N is 1..16 collaborators excluding the lead; omission lets the lead
choose. See [collective swarm](collective-swarm.md) for persistence, admission,
`mailbox publish`, bounded `mailbox wait --timeout-ms`, and platform limits.
`mailbox status --run RUN` reports retained history and caller backlog capacity
without delivery, acknowledgment or reservation. Root/operator `team review` and
`team review-read` preserve immutable contribution snapshots and separately labelled
reviewer claims; see [review continuity](swarm-review.md). They never execute checks
or establish acceptance.

`--swarm-file PATH` (global or ask-local) strictly validates, snapshots and activates
a [purposeful swarm](purposeful-swarms.md), including version-2
[contribution contracts](swarm-factory.md). It is mutually exclusive with
`--swarm`; the file's flattened participants set the cap. `tny swarm validate FILE
[--json]` performs local, side-effect-free validation. Activation requires a native
local saved lead session, while resume uses the persisted canonical snapshot rather
than silently rereading a changed file.
