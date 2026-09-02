# 0054 — Probe the terminal for its real size; paint the bottom block with autowrap off

Date: 2026-09-02
Status: accepted (relates to 0017 wasm page terminal, 0026 attribute SGR)

## Context

The TUI is a scrolling transcript plus a redrawn bottom block (partial
line, popover, status row, composer). `tui_render` erases the previous
block with `CR`, `CUU <cur_row>`, `ED`, then repaints. That arithmetic
assumes every block row occupies exactly one physical terminal row, which
`tui_size` guarantees by clamping each row to `cols - 1` columns, `cols`
coming from `TIOCGWINSZ`.

`TIOCGWINSZ` reports what the kernel was told, not what the user sees. A
sandbox shell or web console that never forwards a window-size change
leaves the pty at its 80x24 default while the phone terminal on the other
end is 44 columns wide. The reverse-video status row, padded to 79 cells,
soft-wrapped onto two physical rows; `erase_block` moved up one row too
few; every keypress left one more copy of the status row on screen, and a
streamed reply repeated the same way. Two screenshots from a Modal sandbox
over SSH showed twelve identical status rows above the composer.

A second, smaller path to the same failure is glyph width: `tui_push_ansi`
counts one column per code point, so a row that ends in a two-cell emoji
spills by one cell even when `cols` is right.

## Decision

1. **Ask the terminal.** After `tui_size` at startup and on every
   `SIGWINCH`, `tui_size_probe` writes `ESC 7`, `CUP 999;999` (clamps to
   the bottom-right corner, never scrolls), `DSR 6`, `ESC 8`. The answer,
   `CSI rows ; cols R`, arrives on stdin, is decoded by `tui_decode_one` as
   `TUI_K_CPR`, and `tui_size_report` replaces `rows`/`cols` and marks the
   block dirty only when they changed. A report with row 1 is ignored: it
   is xterm's modified F3 (`CSI 1 ; m R`), never a corner report. A
   terminal that does not answer costs nothing.
2. **Paint the block with DECAWM off.** `tui_render` wraps the block in
   `CSI ? 7 l` … `CSI ? 7 h`. Until the probe answers, or when a row is
   wider than the column counter believes, the terminal clips the row at
   its right margin instead of wrapping it, so the cursor-up count stays
   right and the worst case is a truncated status row. The transcript is
   written before the block and after the restore, so it still soft-wraps.
3. `tui_size` also tries fds 0 and 2 when stdout has no window size.

## Consequences

- One extra escape round-trip at startup and per resize; no new state
  beyond the existing `rows`/`cols`.
- Both failure modes are covered by unit tests that drive `tui_render`'s
  bytes through a small VT model narrower than `t->cols`
  (`render_narrow_terminal_leaves_one_status_row`,
  `render_partial_line_at_the_margin_does_not_duplicate` in
  `tests/test_tui.c`). The integration screen model understands `ESC 7`,
  `ESC 8` and `CUP` with arguments.
- wasm: xterm.js answers DSR 6 and honours DECAWM; the page's
  `Module.tnyWinsize` remains the primary source and the probe agrees with
  it.
