# TUI

Match fx's form: a **Unix shell**, not an IDE. Streaming transcript, a pinned composer, a one-line status footer. No mouse-required panes, no ncurses windows.

## Layout

```text
[transcript: user / assistant / tools / approvals]
[status: backend  model  perm  session  cwd]
> composer
```

- Transcript is append-only with scrollback. Markdown-ish: headings, lists, fenced code, diffs as plain text with `+`/`-` coloring.
- Composer supports multiline (`Shift-Enter` / `Alt-Enter` / `\\` then Enter).
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
| Ctrl-O | full transcript / review |
| Ctrl-X | subagent manager (native loop) |

Disable `/` `@` `$` popovers while an approval or clarification is focused so paths like `/tmp/x` stay literal.

## Slash commands (v1)

Mirror fx names where they still make sense. Backend-specific commands degrade to "not available on this backend" instead of crashing.

Sessions: `/help` `/clear` `/new` `/reset` `/resume` `/continue` `/rename` `/compact` `/quit`

Runtime: `/models` `/model` `/permissions` `/sandbox` `/backend` `/status` `/usage`

Tools: `/mcp` `/skills` `/workspace` `/image` `/undo` `/copy` `/trace`

Auth: `/login` `/logout` `/setup` — dispatch to the active backend (Cursor key, Codex CLI login, provider key). No Vercel-only flow.

## Rendering host streams

Normalize before paint:

- Cursor `sdk_message` types `assistant`, `thinking`, `tool_call`, `status`
- Codex `item/agentMessage/delta`, `item/started`, approval server-requests
- ACP `session/update` (message chunks, tool calls, plans)
- OpenAI SSE `choices[].delta` and `tool_calls`

Ignore keepalives and unknown envelope cases. Never block the input loop on a parse error; show a one-line warning and keep the connection.

## Permissions UI

Three choices, same as fx: **Yes** / **Yes, and don't ask again** / **No**. Host backends may send a smaller set (`allow-once`, `allow-always`, `reject-once` on Cursor ACP). Map them onto the same keys (`y` / `a` / `n`) and document the mapping in the prompt.
