# TUI

Match fx's form: a **Unix shell**, not an IDE. Streaming transcript, a pinned composer, a one-line status footer. No mouse-required panes, no ncurses windows.

## Layout

```text
[transcript: user / assistant / tools / approvals]
[status: backend  model  perm  session  cwd]
> composer
```

- Transcript is append-only with scrollback. Markdown-ish: headings, lists, fenced code, diffs as plain text with `+`/`-` coloring.
- Composer wraps at the terminal width and supports real newlines
  (`Ctrl-J`, `Option-J` / `Alt-J`, `Alt-Enter`, kitty/CSI-u `Shift-Enter`
  `\x1b[13;2u`, or `\\` then Enter). Plain Enter still submits.
- `Ctrl-V` pastes a clipboard image when a helper is installed (`pngpaste` /
  `osascript` on macOS, `wl-paste` / `xclip` on Linux): the file is written
  under `/tmp` and its path is inserted as inline-code composer text (so the
  leading `/` is not parsed as a command). Text is pasted if the clipboard has
  no image. Helpers are spawned only on paste.
- Status line is off-by-default extras (sandbox, context bytes) like fx.

## Ephemeral mode

Start the shell with `tny --ephemeral` to keep the conversation process-local.
The TUI prints an explicit mode line at startup. It keeps the full multi-turn
conversation and prompt history in memory while the shell is running, but it
does not read or write saved sessions, recovery checkpoints, large-result
blobs, or `~/.tny/history`.

`/resume`, `/continue`, the session picker, and other saved-session import paths
are unavailable because they would load persisted conversation state. `/new`
and `/clear` continue to work within the process. Exiting the shell discards
the in-memory session. `--no-save` remains an alias. See
[ADR 0020](adr/0020-ephemeral-sessions.md).

## SSH

`tny --ssh user@host[:port] [--ssh-cwd DIR]` starts the shell locally with
every workspace tool executing on the remote host. Inside a session,
`/ssh user@host[:port] [dir]` attaches (the block is released so OpenSSH can
prompt), `/ssh off` goes back to local tools, and a bare `/ssh` shows the
current target. The provider connection, session and history stay local
([ADR 0022](adr/0022-ssh-execution-boundary.md)).

## Input

| Input | Action |
| --- | --- |
| text | user prompt |
| `/` at start | command palette (filter as you type) |
| `@` | workspace file picker (gitignore-aware, insert path only) |
| `$` | skill picker (insert skill name, do not load until invoked) |
| Up/Down at draft edge | prompt history |
| Esc or Ctrl-C | interrupt current turn and drop queued messages (second Ctrl-C exits if idle) |
| Enter during a turn | steer the running turn (codex `turn/steer`, native loop) or queue the message for when it ends (cursor, acp) — [ADR 0011](adr/0011-mid-turn-input-steer-or-queue.md) |
| Ctrl-J / Alt-J / Shift-Enter | insert a newline in the composer |
| Ctrl-V | paste a clipboard image path (or text) |
| Ctrl-O | full transcript / review |
| Ctrl-X | subagent manager (native loop) |

Disable `/` `@` `$` popovers while an approval or clarification is focused so paths like `/tmp/x` stay literal.

Typing while a turn runs never writes a note into the transcript: a steered message is echoed as `› text steer`, a queued one sits in a dim `queued (n): …` row above the status row until the turn ends and it is sent through the normal prompt path. Queued messages are dropped (with a one-line note) when the turn is interrupted or fails.

Menus are **transient overlays** ([ADR 0003](adr/0003-transient-menu-overlay.md)): the palette and `/help` draw inside the redrawn bottom block, esc hides them, and the next submit clears them — they never enter the scrollback. Without a tty, menu output degrades to plain transcript lines.

## Slash commands (v1)

Mirror fx names where they still make sense. Backend-specific commands degrade to "not available on this backend" instead of crashing.

Sessions: `/help` `/clear` `/new` `/reset` `/resume` `/continue` `/rename` `/compact` `/quit`

Runtime: `/models` `/model` `/effort` `/max-steps` `/permissions` `/sandbox` `/provider` (`/backend`) `/fast` `/status` `/usage`

`/provider setup [NAME]` runs the guided provider wizard through the
composer (name → base url → key or `$ENV_NAME` → model; `/cancel` aborts;
[ADR 0018](adr/0018-provider-setup-stored-keys.md)) — in the browser wasm
terminal this is the primary way to add a provider.
`/provider [NAME]`'s palette hint and `/help` line list the providers usable
right now — builtins, settings.json profiles with a `base_url`, and
`NAME_BASE_URL` env providers — so the accepted names are discoverable
without leaving the TUI.

`/effort [off|light|medium|high|xhigh|max|default]` changes the reasoning
effort at any point in the conversation and applies from the next turn with
no backend rebind: it rides on codex `turn/start`, cursor
`SendOptions.model.params`, and the openai request body ([ADR
0009](adr/0009-reasoning-effort.md)). `/models` lists the levels each model
actually advertises. `/fast [fast|priority|default]` selects the provider's
paid fast tier (`TNY_CAP_FAST`: openai, cursor, codex) and rebinds where the
tier rides on session start (codex `thread/start`).

`/max-steps set N` caps the native loop at N model calls per turn;
`/max-steps clear` removes the cap (the default is unlimited — [ADR
0024](adr/0024-unlimited-steps-default.md)). The value is read at step
boundaries, so it applies immediately with no backend rebind. Host providers
run their own loops and ignore it.

Tools: `/mcp` `/skills` `/workspace` `/image` `/undo` `/copy` `/trace` `/ssh`

`/image PATH` explicitly queues a file for the next prompt. The native loop
sends it as an `image_url` data URL ([ADR 0008](adr/0008-native-loop-images.md)).
Ctrl-V instead inserts the materialized clipboard-image path as ordinary text,
so it works with hosts that have no image-input capability ([ADR
0025](adr/0025-clipboard-images-paste-as-paths.md)). The model can then call
`read_image` or its own file/image tool on that path.

Transcript spacing: one blank line between the echoed user prompt and the
first agent output, and one blank line before the next model iteration
after a tool batch.

Auth: `/login` `/logout` `/setup` — dispatch to the active backend (Cursor key, Codex CLI login, provider key). No Vercel-only flow.

## Rendering host streams

Normalize before paint:

- Cursor `sdk_message` types `assistant`, `thinking`, `tool_call`, `status`
- Codex `item/agentMessage/delta`, `item/started`, approval server-requests
- ACP `session/update` (message chunks, tool calls, plans)
- OpenAI SSE `choices[].delta` and `tool_calls`

Ignore keepalives and unknown envelope cases. Never block the input loop on a parse error; show a one-line warning and keep the connection.

Reasoning traces render dim, one SGR pair per physical line: color never
depends on state from a previous line, because the renderer flushes the
transcript per line and repaints the partial line from scratch every frame
([ADR 0012](adr/0012-self-contained-sgr-lines.md)).

## Browser terminal

The GitHub Pages landing terminal (`site/index.html`) runs the real TUI:
the tny binary compiled to WebAssembly inside xterm.js
([ADR 0017](adr/0017-wasm-browser-parity.md), superseding 0005's JS
preview). The page is mobile-first: xterm is fitted to the mount (never
left at the 80-column default), welcome text wraps to the current column
count, header links are 44px tap targets, and the visual viewport shrinks
the pane when the on-screen keyboard opens. Pass `OPENAI_API_KEY` and
optionally `OPENAI_BASE_URL` in the URL hash or paste them at the
pre-launch prompt; both pass through `sanitizeApiKey` at intake and stay
in the tab. The native openai loop, sessions, skills, and fs tools run on
MEMFS (per-tab, not persisted); codex is attach-only, ACP is
`--agent ws://` remote-only, cursor errors cleanly; `terminal`/`open_file`
return the missing-host tool error. The provider must allow CORS —
`api.openai.com` does not; use a CORS-open gateway or a loopback server.

## Startup

First paint never waits on a backend, but the TUI **pre-warms** the selected provider's host right after the banner ([ADR 0002](adr/0002-tui-provider-prewarm.md)): `codex app-server`, the cursor bridge, or the ACP agent is spawned and initialized on a background thread so the first prompt adopts a live connection instead of paying seconds of startup. Pre-warm failures stay silent and resurface on the ordinary lazy path. One-shot CLI commands do not pre-warm.

In ephemeral mode, pre-warm may still create process-local provider state.
Codex receives `ephemeral:true` on `thread/start`; adapters without a portable
no-store field retain their ordinary provider-side policy while tny continues
to make no local conversation write.

## Permissions UI

Three choices, same as fx: **Yes** / **Yes, and don't ask again** / **No**. Host backends may send a smaller set (`allow-once`, `allow-always`, `reject-once` on Cursor ACP). Map them onto the same keys (`y` / `a` / `n`) and document the mapping in the prompt.
