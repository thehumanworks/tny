# ADR 0183: Fullscreen transcript scrollback

Status: accepted

## Decision

Extend the ANSI fullscreen renderer from ADR 0182 with retained transcript
scrollback. `ui.scrollback_lines` accepts integers from 1 to 1,000,000 and
defaults to 50,000. The headless `settings` CLI and
`/settings ui.scrollback_lines N` save the same setting for the next launch.
Inline and plain modes keep their existing output behavior.

Retention counts completed newline-delimited logical lines, independently of
terminal width, with the current unfinished streaming line retained in addition.
There is no separate byte cap: retained memory varies with line length. Navigation counts wrapped display rows: mouse wheel events move three
rows, Shift-Up/Down one row, PageUp/Down one viewport page, and Ctrl-Home/End
the oldest retained output or live bottom. Plain arrows retain composer and
prompt-history behavior. Resize recomputes wrapping from retained text.

A view away from the bottom stays anchored while new output arrives. Returning
to the bottom follows live output again. Eviction clamps a view whose anchor
has expired to the oldest remaining output. Fullscreen enables mouse reporting
and disables it during terminal cleanup, with either buffer choice.

## Consequences and verification

Fullscreen history becomes usable without depending on alternate-buffer
terminal scrollback. The retained view is bounded and process-local; persisted
session history remains separate and accessible through `/transcript`.
No ncurses dependency, new event loop or provider path is introduced. The
browser terminal uses the same renderer and virtual settings filesystem.

Renderer tests cover retention, wrapping, resize, viewport movement and anchors.
Settings tests cover bounds, persistence and next-launch application. PTY tests
cover navigation, streaming while scrolled, mouse sequences and terminal
restoration; inference uses local mocks.
