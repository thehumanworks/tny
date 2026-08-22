# 0009 — Streamed transcript color is per-line: SGR never crosses a newline

Date: 2026-08-22
Status: accepted

## Context

Reasoning traces stream dim (`\x1b[2m`) into the transcript. The renderer
works at two granularities that both discard SGR state at line boundaries:

- `tui_write` flushes completed lines (everything up to the last `\n`) from
  `t->partial` into the committed transcript, keeping only the tail of the
  current line in `partial`.
- `tui_render` erases and repaints the bottom block every frame, drawing
  `t->partial` from scratch and appending a reset (`\x1b[0m`) after it. The
  composer, status row, and popover emit their own SGR codes in between.

The `TNY_EV_THINKING` handler wrapped each *delta* — not each line — in one
`\x1b[2m … \x1b[0m` pair. A delta that crossed a newline (`"…end.\nNext"`)
left its opening SGR on the flushed line while the tail (`Next\x1b[0m`) sat in
`partial` with no opening code. On the next repaint those characters rendered
in the default color; the following delta re-opened dim, so the rest of the
line was grey. Visible symptom: the first letters of a reasoning line are
white, the rest muted.

Anchoring the fix on terminal SGR carryover (attributes surviving `\n` on the
wire) is not an option: the redraw cycle interleaves resets and other SGR
codes between the flush and the repaint, so any color that depends on state
from a previous line is corrupted sooner or later.

## Decision

**Every physical transcript line owns its color: streamed styled text opens
its SGR after each newline and closes it before the line ends. No transcript
byte relies on SGR state set on an earlier line.**

- New `tui_write_dim(t, s, n)` in `tui_draw.c` splits the delta on `\n` and
  wraps each non-empty segment in `\x1b[2m … \x1b[0m` (plain passthrough when
  color is off). Empty segments (blank lines) carry no SGR noise.
- The `TNY_EV_THINKING` handler uses it instead of wrapping the whole delta.
- Existing single-line emitters (`tui_sys`, `tui_linef` callers, tool rows)
  already satisfy the invariant — they never embed `\n` inside a styled span.
  Future multi-line styled output must go through the same per-line wrapping.

## Consequences

- The `partial` tail of a reasoning delta is self-contained, so the
  erase/repaint cycle can drop and redraw it in any SGR state.
- The committed transcript is also state-independent: lines copied out of
  scrollback or re-emitted after `tui_raw_begin` carry their own color.
- Slightly more SGR bytes on multi-line deltas (8 bytes per line); irrelevant
  next to the text itself.
- Unit tests in `tests/test_tui.c` (`write_dim_*`) pin the flushed/partial
  byte split; they fail if a delta-level wrap ever comes back.
