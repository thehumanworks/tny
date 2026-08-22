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
  under `/tmp` and a `[Image #N]` placeholder is inserted. Text is pasted
  if the clipboard has no image. Helpers are spawned only on paste.
- Status line is off-by-default extras (sandbox, context bytes) like fx.

## Input

| Input | Action |
| --- | --- |
| text | user prompt |
| `/` at start | command palette (filter as you type) |
| `@` | workspace file picker (gitignore-aware, insert path only) |
| `$` | skill picker (insert skill name, do not load until invoked) |
| Up/Down at draft edge | prompt history |
| Esc or Ctrl-C | interrupt current turn (second Ctrl-C exits if idle) |
| Ctrl-J / Alt-J / Shift-Enter | insert a newline in the composer |
| Ctrl-V | paste clipboard image (or text) |
| Ctrl-O | full transcript / review |
| Ctrl-X | subagent manager (native loop) |

Disable `/` `@` `$` popovers while an approval or clarification is focused so paths like `/tmp/x` stay literal.

Menus are **transient overlays** ([ADR 0003](adr/0003-transient-menu-overlay.md)): the palette and `/help` draw inside the redrawn bottom block, esc hides them, and the next submit clears them — they never enter the scrollback. Without a tty, menu output degrades to plain transcript lines.

## Slash commands (v1)

Mirror fx names where they still make sense. Backend-specific commands degrade to "not available on this backend" instead of crashing.

Sessions: `/help` `/clear` `/new` `/reset` `/resume` `/continue` `/rename` `/compact` `/quit`

Runtime: `/models` `/model` `/effort` `/permissions` `/sandbox` `/backend` `/fast` `/status` `/usage`

`/effort [off|light|medium|high|xhigh|max|default]` changes the reasoning
effort at any point in the conversation and applies from the next turn with
no backend rebind: it rides on codex `turn/start`, cursor
`SendOptions.model.params`, and the openai request body ([ADR
0009](adr/0009-reasoning-effort.md)). `/models` lists the levels each model
actually advertises. `/fast [fast|priority|default]` selects the provider's
paid fast tier (`TNY_CAP_FAST`: openai, cursor, codex) and rebinds where the
tier rides on session start (codex `thread/start`).

Tools: `/mcp` `/skills` `/workspace` `/image` `/undo` `/copy` `/trace`

`/image PATH` and Ctrl-V queue files for the next prompt. The native loop
sends them as `image_url` data URLs ([ADR 0008](adr/0008-native-loop-images.md)).
The model can also call `read_image` on a path.

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

## Browser demo

The GitHub Pages landing terminal (`site/index.html`) is a client-side
preview of this chrome ([ADR 0005](adr/0005-client-side-landing-terminal.md)).
Pass `OPENAI_API_KEY` and optionally `OPENAI_BASE_URL` in the URL hash,
`/login`, or an `OPENAI_*=` assignment. Values are AES-GCM sealed in this
origin and never written in the clear; the base URL is obfuscated in the UI.
Workspace tools, `@` / `$`, and host providers are not available there.

## Startup

First paint never waits on a backend, but the TUI **pre-warms** the selected provider's host right after the banner ([ADR 0002](adr/0002-tui-provider-prewarm.md)): `codex app-server`, the cursor bridge, or the ACP agent is spawned and initialized on a background thread so the first prompt adopts a live connection instead of paying seconds of startup. Pre-warm failures stay silent and resurface on the ordinary lazy path. One-shot CLI commands do not pre-warm.

## Permissions UI

Three choices, same as fx: **Yes** / **Yes, and don't ask again** / **No**. Host backends may send a smaller set (`allow-once`, `allow-always`, `reject-once` on Cursor ACP). Map them onto the same keys (`y` / `a` / `n`) and document the mapping in the prompt.
