# ADR 0182: Inline and fullscreen TUI display

Status: accepted

## Decision

Keep inline display as the default. Add an optional fullscreen renderer to
the existing ANSI shell, with the composer anchored to the terminal bottom
and a bounded recent transcript above it. Retain the shared input, session,
provider, permissions, and event-loop paths; introduce no TUI framework.

User settings `ui.mode` (`inline` or `fullscreen`) and
`ui.alternate_screen` (boolean, default true) configure future launches.
The headless `settings` command and the `/settings` section edit these same
keys. Explicit leading `--inline` / `--fullscreen` and `--alt-screen` /
`--no-alt-screen` flags override defaults without persisting. TUI edits take
effect next launch, avoiding disruptive display changes during a turn.

Fullscreen enters DEC alternate screen mode by default and clears the visible
screen before drawing. Opting out of the alternate buffer still clears the
primary screen. Cleanup restores terminal modes and leaves the alternate
screen on normal exit and handled termination signals. A non-TTY always uses
the existing plain stream behavior, regardless of display settings. The
browser's xterm.js terminal uses the same renderer; settings live in its
virtual filesystem.

## Consequences and verification

Inline keeps native scrollback. Fullscreen retains a bounded transcript tail
for redraw and resize; it is not a replacement for saved session history.
Reused noninteractive CLI command output must join that transcript, while
interactive authorization prompts retain direct access to the terminal.

Generated properties cover display bounds and settings preservation.
PTY tests cover the settings/flag precedence matrix, rendering at different
sizes, resize, screen clearing, and terminal restoration. Plain-stream tests
verify that screen controls do not leak into headless use. All inference
tests use existing mocks.
