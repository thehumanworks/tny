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
  under `/tmp` and its path is inserted as inline-code composer text. Text is
  pasted if the clipboard has no image. Helpers are spawned only on paste.
- The shell enables **bracketed paste** (`ESC[?2004h`): a terminal paste
  arrives wrapped in `ESC[200~ … ESC[201~` and is inserted into the composer
  as literal text with `\r`/`\r\n` normalized to `\n` — pasted newlines never
  submit. Only a real Enter keypress submits the draft.
- Status line is off-by-default extras (sandbox, context bytes) like fx.
- Terminal size comes from `TIOCGWINSZ` and then from the terminal itself:
  the shell sends `ESC 7 CSI 999;999 H CSI 6 n ESC 8` at startup and on
  `SIGWINCH` and applies the `CSI rows;cols R` answer, because a sandbox
  shell or web console can leave the pty at 80x24 while the real terminal
  is narrower. The bottom block (status row, composer, popover, streaming
  line) is painted with autowrap off (`CSI ?7 l` … `CSI ?7 h`) so an
  over-wide row is clipped, never soft-wrapped into a second physical row
  the repaint would leave behind ([ADR 0054](adr/0054-terminal-size-probe-and-no-autowrap-block.md)).

## Colors and attributes

SGR output splits into *colors* and *attributes* (bold, dim, reverse video),
resolved once at startup (`tny_color_resolve`,
[ADR 0026](adr/0026-color-vs-attribute-sgr.md)):

- `NO_COLOR` (any value, even empty) disables colors only. Attributes are
  structural: the status bar keeps its reverse video, the banner stays bold,
  reasoning traces stay dim.
- `CLICOLOR_FORCE` (non-empty, not `0`) or `--color=always` forces full
  styling — it beats `NO_COLOR` and applies even when stdout is piped.
- `--color=never` / `--no-color` emits no SGR at all; the status row falls
  back to `── provider  model  mode … ──` delimiters so it never reads as
  ordinary transcript text.

Without a tty the shell runs in dumb mode: no status row, composer, or
overlays — one prompt per stdin line, and approvals auto-deny. Startup says
so (`not a terminal: status bar disabled, approvals auto-deny`), and each
turn ends with a plain `── provider  model  in/out tok ──` transcript line.

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
([ADR 0022](adr/0022-ssh-execution-boundary.md)). Project `AGENTS.md` follows
the remote cwd while attached ([ADR 0040](adr/0040-ssh-agents-md.md));
`/ssh off` restores the local chain. Task discovery is deliberately builtin-only
while attached. If a local user, project, or workflow task is selected, `/ssh`
refuses the transition; run `/task clear`, attach, then select a builtin task.
This prevents local project instructions from crossing into the remote workspace.

## Input

| Input | Action |
| --- | --- |
| text | user prompt |
| `/` at start | command palette (filter as you type) |
| `@` | workspace file picker (gitignore-aware, insert path only) |
| `$` | skill picker (insert skill name, do not load until invoked) |
| Up/Down at draft edge | prompt history |
| Esc or Ctrl-C | interrupt current turn and drop queued messages; another Ctrl-C while cancelling forces termination (twice exits when idle) |
| Ctrl-D | stop an active turn and exit, even with a draft; when idle, exit on an empty draft or delete the next character |
| Enter during a turn | steer the running native turn |
| Ctrl-J / Alt-J / Shift-Enter | insert a newline in the composer |
| Ctrl-V | paste a clipboard image path (or text) |
| Ctrl-R | record dictation; Enter/Ctrl-R transcribes into the editable draft; Esc/Ctrl-C cancels |
| Ctrl-O | optimise the typed/dictated draft using an independent model; review before Enter submits |
| Left with an empty composer | background an active native turn and open all saved sessions; idle Left opens the same dashboard; drafts and focused inputs still edit |
| Ctrl-X / `/agents` | all saved sessions; detach an active foreground turn into background mode |

`/optimise PROMPT` also rewrites the draft; `--model MODEL` and
`--provider NAME` before the prompt override its configured defaults.
Esc/Ctrl-C cancels without changing the draft. The optimiser can explore
relevant project files using read-only tools. `/transcript` shows the full
conversation. See [Prompt optimisation](optimisation.md).

Disable `/` `@` `$` popovers while an approval or clarification is focused so paths like `/tmp/x` stay literal.

Ctrl-C during a turn takes priority over palettes and overlays. Cancellation
gets five seconds to finish before the TUI kills the runner and verifies
writer-lock release; pressing Ctrl-C again skips that grace period. The shell
reports interruption after confirmation, and the saved session remains resumable.
Ctrl-D, `/quit`, EOF, SIGHUP and SIGTERM use the same bounded shutdown. A caller
crash or SIGKILL still leaves a detached run; find it with `tny sessions` and
stop it with `tny session stop <id> --kill`. See
[ADR 0081](adr/0081-reliable-session-interruption.md).

Typing while a turn runs never writes a note into the transcript: a steered message is echoed as `› text steer`, a queued one sits in a dim `queued (n): …` row above the status row until the turn ends and it is sent through the normal prompt path. Queued messages are dropped (with a one-line note) when the turn is interrupted or fails.

Menus are **transient overlays** ([ADR 0003](adr/0003-transient-menu-overlay.md)): the palette and `/help` draw inside the redrawn bottom block, esc hides them, and the next submit clears them — they never enter the scrollback. Without a tty, menu output degrades to plain transcript lines.

## Saved session inspection and continuation

The saved-session dashboard also opens directly with `tny agents`, without provider
resolution or prewarm. It lists all saved local foreground and background sessions
from every workspace, including unrelated repositories and non-Git directories.
The current cwd's workspace comes first, followed by other workspace paths in
alphabetical order; each section lists its newest sessions first. Directory
headings appear above indented session rows:

```text
/Users/tomas
  > session 1
    session 2
/Users/tomas/projects
    session 3
```

Type to fuzzy-filter workspace paths (case-insensitive characters in order,
not necessarily consecutive). For example, `prjtny` can match
`/Users/tomas/projects/tny`. Every session in a matching workspace remains
available. Backspace removes a character; Esc clears a nonempty filter, then
Esc on an empty filter exits. Ctrl-C/D exit directly. The letter `q` is searchable
text; the separate `--run` task view retains its q shortcut. Up/Down moves between
sessions, skipping headings; Enter opens the selected session. The filter and
selected session survive periodic refreshes, and scrolling repeats the visible
section's heading. If only one display row is available, it shows the selected
session. No matches displays an empty result instead of opening a hidden session. See [ADR 0167](adr/0167-workspace-dashboard-navigation.md).

Opening or continuing a row from another cwd uses that session's original
workspace. Legacy rows in the current workspace's storage bucket also appear
under its known path and match that path's filter. Other legacy rows without
saved workspace metadata explicitly identify the current-cwd fallback before
continuation; their storage bucket and history are preserved. Up/Down selects a row; Enter attaches
as owner when the live runner accepts the unique owner handshake, including an idle completed
runner. This preserves the ongoing turn, permission mode, pending decision,
model and workspace; it does not repost a prompt.

Otherwise Enter opens the saved transcript, clearly labeled **read-only**, with
continuation guidance. This includes ordinary saved foreground sessions,
completed/unlocked sessions and sessions whose writer is held by another owner or
unreachable. Inspection does not resolve
a provider, refresh credentials, start a runner, activate a saved checkpoint, or
save session/settings/auth stores. Missing provider configuration does not hide
saved text. A stored `done` status does not establish writer-lock freedom.

From a view without a saved checkpoint, submitting a prompt explicitly
requests continuation of the **selected session**, retaining its ID and history.
`/continue` in a background view also acquires or attaches that same session,
not `last`. A held lock permits only an owner-handshake attempt: a rival owner or
unreachable runner refuses execution and leaves the saved view retryable. There
is no lock stealing or socket replacement. With the writer lock acquired, tny
reloads the saved conversation under that lock and resolves its provider, model
and workspace before a new runner executes. The old runner keeps ownership
through its final save and socket removal ([ADR 0104](adr/0104-runner-quiescence-ownership.md)).

New execution uses the selected session's saved provider/model/workspace with
current settings and launch flags for the remaining options. Legacy missing
metadata uses the [existing fallbacks](features/sessions.md#explicit-continuation-and-ownership),
not reconstruction of old permissions, effort, credentials or other configuration
that was never saved.
A provider or configuration error leaves inspection available. Live attachment
keeps the existing runner's effective configuration instead.

**Checkpoint recovery is an explicit action.** Opening a saved row never
activates its saved continuation. A typed prompt is refused while a saved
checkpoint is present, including consumed or invalid checkpoints: it is **not
submitted or queued**. Use `/continue` to validate the checkpoint through existing
recovery; consumed or invalid work is rejected. Recovering valid unconsumed work
adds no new user message and may release retained tool calls and other effects,
not just display more text. CLI `tny resume ID` retains its existing explicit
recovery behavior. Outside a background view, `/continue` still resumes the
latest workspace session.

The saved read-only view allows only `/help`, `/clear`, `/transcript`, `/copy`,
`/trace`, `/agents`, `/quit`, `/exit` and `/continue`. `/clear` clears the display,
not the saved conversation. `/cancel` is available only when attached to a
runner. Session mutations (`/new`, `/reset`, `/resume`, `/rename`, `/compact`,
`/undo`), settings/provider switches and auth commands are blocked. The same
direct-mutation restrictions apply to an attached background replica: turn,
steering, permission and cancellation controls go through its owner connection,
never local replica saves. Read-only/save guards are separate from detach-on-exit.

`/agents` and Ctrl-X return to the list immediately without restarting work.
Quit from a background view or dashboard detaches without saving the replica;
Ctrl-C can explicitly cancel an attached turn, but a saved read-only view has no
cancellation authority. Ordinary foreground quit/interrupt behavior is unchanged.
Handoff-origin pending permissions retain the runner's mode and wait for an owner
(five-minute bound). Streaming and tool-running turns continue while the list is open.

Handoff and starting a new runner from a saved background view require native
runner support. wasm and in-process modes reject continuation before mutation,
not by starting an unlocked in-process writer; ephemeral mode cannot open saved
sessions. An already successful live attachment remains usable where supported,
even if starting a replacement runner is unavailable.
[ADR 0166](adr/0166-global-sessions-and-immediate-backgrounding.md) defines fresh
executable runner startup and immediate background detachment. Caller-side TLS
initialization does not disable the native runner. Older handoff checkpoints
remain subject to [ADR 0108](adr/0108-checkpoint-recovery-and-hosted-tool-boundaries.md)
recovery validation; current backgrounding does not create a restart checkpoint.
[ADR 0154](adr/0154-agents-session-inspection-and-continuation.md) separates
saved-row inspection from explicit continuation and checkpoint activation.

Each interactive dashboard entry clears the visible screen and terminal scrollback
and paints the list from the top-left corner, separate from the chat or shell
output ([ADR 0138](adr/0138-full-screen-agents-dashboard.md)). Terminals without
scrollback-erasure support still clear the visible screen. A Left-arrow handoff
clears only when the runner acknowledges the saved background marker.
Periodic refreshes do not clear again. Saved transcripts and composer drafts are
preserved; reattachment replays the saved chat. Non-TTY and `--json` listings keep
their existing plain/structured output without screen controls.

## Slash commands (v1)

Mirror fx names where they still make sense. Backend-specific commands degrade to "not available on this backend" instead of crashing.

Sessions: `/help` `/clear` `/new` `/reset` `/resume` `/continue` `/rename` `/compact` `/quit`

Runtime: `/models` `/model` `/effort` `/max-steps` `/permissions` `/sandbox` `/provider` (`/backend`) `/fast` `/status` `/usage`

`/status` includes the [Codex subscription weekly allowance](backends/codex.md#subscription-usage-status-tny-status)
and reset date/time when the active Codex profile uses a ChatGPT login.
API-key logins are excluded. `/usage` remains local token-usage accounting.

`/provider setup [NAME]` runs the guided provider wizard through the
composer (name → base url → environment variable name → model; `/cancel` aborts;
[ADR 0153](adr/0153-environment-keys-and-oauth-credentials.md)) — in the browser wasm
terminal this is the primary way to add a provider.
`/provider [NAME]`'s palette hint and `/help` line list the providers usable
right now — builtins, settings.json profiles with a `base_url`, and
`NAME_BASE_URL` env providers — so the accepted names are discoverable
without leaving the TUI.

Changing effort updates the native HTTP request parameters for the next iteration.

`/max-steps set N` caps the native loop at N model calls per turn;
`/max-steps clear` removes the cap (the default is unlimited — [ADR
0024](adr/0024-unlimited-steps-default.md)). The value is read at step
boundaries, so it applies immediately with no backend rebind.

Tools: `/mcp` `/skills` `/workspace` `/image` `/undo` `/copy` `/trace` `/ssh`

Workspace: `/worktree [NAME]` creates or enters a Git checkout in
`~/.tny/worktrees`, starts a fresh session there, and offers merge/remove/keep
when exiting. Enter or rejection keeps the worktree. See [Worktrees](worktrees.md)
for branch naming, reuse and cleanup rules; `--worktree [NAME]` selects the
same mode at startup.

Input: `/dictate [PROVIDER]` records the local microphone and inserts the
transcript at the caret. Codex/ChatGPT handles dictation by default even when
the conversation uses Grok or another provider. Enter finishes recording;
another Enter sends the editable draft. Cancel leaves the draft unchanged.
See [Dictation](dictation.md) for recorder setup, limits, and file/CLI use.

A `/name` line whose first token is not a builtin but names a discovered
skill is a prompt, sent with that `SKILL.md` ahead of the text ([ADR
0056](adr/0056-skill-mention-injection.md)); `$name` anywhere in a message
does the same. Builtins always win on a name collision.

`/task` lists available runtime presets and selects `review`, `optimizer`,
`document`, `retro`, `task-creation`, or a custom name. Selection is session-scoped: it must be
made before the first turn; after a turn or on a resumed host conversation,
changing or clearing the task is rejected. `/new` starts a fresh session.
On resume, tny restores the saved task snapshot when no task was explicitly
selected; an explicit selection must match the saved name and digest.

`/image PATH` explicitly queues a file for the next prompt (max 8 queued;
further `/image` prints `too many images queued`). The native loop
sends it as an `image_url` data URL ([ADR 0008](adr/0008-native-loop-images.md)).
`tny ask --image` is a separate cap of 16 flags (see [cli.md](cli.md)).
Ctrl-V instead inserts the materialized clipboard-image path as ordinary text,
so it works with hosts that have no image-input capability ([ADR
0025](adr/0025-clipboard-images-paste-as-paths.md)). The model can then call
`read_image` or its own file/image tool on that path.

Transcript spacing: one blank line between the echoed user prompt and the
first agent output, and one blank line before the next model iteration
after a tool batch.

Auth: `/login` and `/logout` manage native Codex/Grok subscriptions; `/setup` configures an HTTP profile with an environment-variable name for its key.

## Rendering host streams

Normalize before paint:

- Codex `item/agentMessage/delta`, `item/started`, approval server-requests
- OpenAI SSE `choices[].delta` and `tool_calls`

Ignore keepalives and unknown envelope cases. Never block the input loop on a parse error; show a one-line warning and keep the connection.

Leading whitespace of a streamed reply is not painted: the transcript starts
at the first visible byte, however many deltas the blank run spans. A
reasoning stream that is empty or whitespace-only paints nothing at all (no
dim `· ` marker, no blank line); the marker appears with its first visible
byte. Whitespace after that point is untouched. Plain `tny ask` follows the
same rule on stdout; `--json`, the NDJSON stream and the session keep the
raw deltas.

Reasoning traces render dim, one SGR pair per physical line: color never
depends on state from a previous line, because the renderer flushes the
transcript per line and repaints the partial line from scratch every frame
([ADR 0012](adr/0012-self-contained-sgr-lines.md)).

## Browser terminal

The browser runs the same native HTTP loop over fetch, subject to provider CORS. Its filesystem and environment intake are per-tab and ephemeral. Host OS tools fail cleanly when unavailable.

## Startup

First paint never waits on a provider. Each native turn connects lazily in a detached session runner ([ADR 0053](adr/0053-forked-turn-isolation.md)); the TUI renders its stream. A crashed TUI leaves an in-flight turn finishing into the session. MCP may warm independently. wasm and ephemeral turns run in process. Provider host prewarming is superseded by [ADR 0152](adr/0152-native-http-only-providers.md).

In ephemeral mode, pre-warm may still create process-local provider state.
Codex receives `ephemeral:true` on `thread/start`; adapters without a portable
no-store field retain their ordinary provider-side policy while tny continues
to make no local conversation write.

## Permissions UI

Permission prompts expose **Yes** / **Yes, and don’t ask again** / **No**, mapped to `y` / `a` / `n`. The native loop owns these gates for every provider.
`/swarm [N]` enables [collective mode](collective-swarm.md) in an idle saved local
conversation, preserving history and rebinding its runner. N optionally caps
collaborators (1..16, lead excluded); `/new` permits another cap.
Purposeful file-defined membership is selected before launch with
`tny --swarm-file PATH`; there is no mid-session file-loading slash command.
