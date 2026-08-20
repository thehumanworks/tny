# 0003 — In-TUI menus are transient overlays, never transcript

Date: 2026-08-20
Status: accepted

## Context

The TUI draws a scrolling transcript plus a redrawn bottom block (partial
line, popover, status, composer). The slash *popover* already lived in the
redrawn block, but the `/help` command menu was written into the transcript:
after opening the palette and hitting enter, thirty rows of command listing
were committed to the scrollback forever. The user's report: the buffer must
be clean after interacting with the menu — without clearing the rest of the
buffer.

## Decision

**Menu-style output renders in a transient overlay inside the redrawn block
and is dropped when the interaction ends.**

- New `overlay` buffer on the tui struct, drawn between the partial line and
  the popover (`tui_draw.c`). It never touches `t->out`, so it can never
  enter the scrollback.
- Cleared on: any submit (`tui_submit` entry), esc, ctrl-c. The popover keeps
  its own lifecycle; both can coexist (read /help while typing the next
  command).
- The whole block must fit the screen or `erase_block`'s cursor-up
  arithmetic breaks, so the overlay gets only the rows that the status,
  composer, popover, and partial line leave over (`tui_overlay_budget`);
  overflow is elided with an explicit "… N more rows" line.
- Overlay lines may carry SGR color; `tui_push_ansi` passes SGR through at
  zero display width, drops every other escape, and re-emits the reset even
  when the line is cut at the width limit.
- Without a tty the overlay falls back to plain transcript lines — pipes and
  the dumb mode still get `/help` text on stdout.

## Consequences

- `/help` shows "(esc hides this menu)" and leaves zero residue; verified by
  a pty integration test that renders the escape stream through a small
  terminal emulator and asserts on the *visible screen*, not the byte stream.
- Future menus (pickers, palettes, wizards) should use `tui_overlay_linef`
  instead of `tui_linef` when their content is navigation, not conversation.
