# 0006 — A window holds many sessions in a binary split tree; panes share one framebuffer and one loop

Date: 2026-08-29
Status: accepted

## Context

[ADR 0005](0005-native-renderer-and-macos-window.md) gave tnytty a window
with exactly one session in it. Every terminal people actually use splits:
iTerm2, kitty, WezTerm and Ghostty all do, and the alternative — running
tmux inside tnytty — puts a second terminal emulator between the user and
the one we just wrote.

Two things had to be decided at once: what a window *contains* now that it
is more than one session, and what happens to the parts of ADR 0005 that
quietly assumed a single grid — the rasterizer's framebuffer, the caret,
the selection, the `poll(2)` set, and the status bar.

There was also a bug to fix first, and it turned out to belong here.

## The phantom caret

On launch the window sometimes showed two carets: the real one at the
prompt and a filled block stranded in the last cell of the last row. It
was not a caret at all.

AppKit delivers, when it activates an application, a synthetic
`NSEventTypeLeftMouseDown` whose `locationInWindow` is a sentinel far
outside the window — `(300000, -299382)` on macOS 27. `gui.c` mapped every
mouse-down to a cell with `tt_render_cell_at`, which **clamps** any point
into the grid, so the sentinel became `(cols - 1, rows - 1)`;
`tt_sel_begin` started a one-cell selection there; and the rasterizer
paints a selected cell by inverting it, which on a blank cell is
pixel-identical to a focused caret. No mouse-up ever arrived, so
`sel.dragging` stayed true — and the "the text under this selection
changed, drop it" watchdog in the loop deliberately skips a selection that
is still being dragged. The block therefore survived every frame until the
next full repaint, which is why it looked like a resize artefact.

Three changes, one per layer:

- **`window_macos.c`** converts the point and returns false when it does
  not land on the view. Out-of-window presses are dropped, not clamped.
  (This also replaces the old `px_y < 0` titlebar check, which never fired
  because a full-size content view has no negative coordinates.)
- **`render.c`** gains `tt_render_cell_hit`, which reports whether a point
  is on the grid at all. `gui.c` starts a selection only when it is; a
  press in the padding, under the traffic lights or past the last row is
  not a cell. Clamping stays available for extending a drag, which is the
  one case where it is the right behaviour.
- **`gui.c`** ends a live drag when the window loses key focus, so a
  mouse-up delivered to another application cannot leave a selection
  dragging forever.

The rasterizer also now forgets the caret's last position whenever the
grid is reallocated (`tt_render_set_area`, `tt_render_resize`,
`tt_render_configure`), so a caret recorded in one geometry can never be
reasoned about in another. `tests/test_render.c` pins all of it:
`resizing_clears_the_caret_the_old_grid_left_behind`,
`resizing_clears_an_unfocused_hollow_caret_too`,
`moving_the_caret_erases_the_row_it_left` and
`hit_testing_rejects_points_outside_the_grid` (which feeds it the real
sentinel).

## Decision

### A binary split tree, platform-free, payload-agnostic

`src/ui/layout.c` owns the geometry: a binary tree whose leaves are panes
and whose internal nodes carry a direction (`TT_SPLIT_VERT` side by side,
`TT_SPLIT_HORZ` stacked) and a ratio in thousandths, 500 on creation.
`tt_layout_apply` turns one area rectangle into a rectangle per leaf; a
`divider`-wide gap is left between siblings, so **no pane owns the rule
between them** and a click there belongs to nobody.

A leaf carries a `void *user` the caller owns. The tree therefore knows
nothing about sessions, fonts or pixels-per-cell, and `tests/test_layout.c`
exercises splitting, closing, nesting, odd pixel sizes, hit testing, focus
movement and cycling with tagged pointers for panes — no window, no pty.

Splitting happens **in place**: the leaf becomes the internal node and its
payload moves to the new first child. Closing is the inverse — the sibling
is spliced into the parent's slot, so it reclaims the whole rectangle in
one step and every pointer above stays valid.

### One framebuffer, one rasterizer per pane

`tt_render` used to own its pixels. It now paints into a `tt_fb` at a
rectangle it is given (`tt_render_new_in`, `tt_render_set_area`), and the
single-owner constructor `tt_render_new` is that case with the rectangle
equal to the surface. Every pane keeps its own shadow grid, dirty rows and
bleed flags, so **dirty-row painting stays per pane**: typing in one pane
repaints one row of one pane and presents that band.

The window paints what belongs to no pane: it fills the grid area with the
`divider` colour before a relayout — each pane then clears its own
rectangle to the terminal background, and what is left showing is exactly
the rules — and it draws the status bar, which spans the window rather
than any pane (`tt_render_status_bar` is exported for it). A pane's config
therefore carries `status_h = 0`.

The transparent titlebar's inset is folded into `pad_top` by the window
seam (ADR 0005). Only panes whose rectangle touches the top of the grid
area sit under the traffic lights, so every other pane gets that inset
back as usable rows.

### One loop, still

`poll(2)` gets every pane's pty master in the one set it already had, with
`POLLOUT` while `tt_session_pending` is non-zero, exactly as the single
session did. No threads, no second event source, no per-pane loop. A pane
whose child exits is closed on the spot and its sibling takes the space;
the last pane closes the window and its exit code is the process's.

### Keys are iTerm2's, decoded once

| Chord | Action |
| --- | --- |
| Cmd-D | split vertically — a new pane to the right |
| Cmd-Shift-D | split horizontally — a new pane below |
| Cmd-W | close the focused pane (the window, when it is the last) |
| Cmd-Opt-Arrow | move focus to the neighbour in that direction |
| Cmd-\[ / Cmd-] | focus the previous / next pane, wrapping |
| Cmd-C / Cmd-V | copy the selection / paste into the focused pane |
| Cmd-Q | quit |

Decoding lives in `src/ui/keys.c` (`tt_key_chord`), with the rest of the
key translation, and is therefore unit-tested on every host.
`window_macos.c` **asks that function** whether a Command press is one of
ours before consuming it, so there is no second copy of the key map in the
platform file and every Command press we do not bind still reaches AppKit
— Cmd-M, Cmd-H and the rest keep working.

The two ways to move focus are deliberate. `Cmd-Opt-Arrow` is directional
and can legitimately find nothing (the pane is against that edge).
`Cmd-[` / `Cmd-]` walk the leaves in reading order with wrap-around, so
they always land somewhere no matter how the tree is shaped — the escape
hatch when the geometry has no neighbour that way.

### Focus is per pane, and so is everything focus implies

The focused pane draws the solid caret; the others draw the hollow box
ADR 0005 already used for an unfocused window. Key input, paste and
DSR/DA answers go to the focused pane's pty only — each pane has its own
`tt_reply` routing its VT's answers into its own session, which is what
makes two `vim`s in two panes both get their cursor-position replies.

Selection is per pane and **only one pane may hold one**: beginning a
selection clears the others, so Cmd-C and copy-on-select never have to
guess. Clicking in a pane focuses it.

## Alternatives rejected

- **Recommend tmux instead.** A second terminal emulator inside ours, with
  its own key prefix, its own resize model and its own idea of what a
  cell is. tnytty is the terminal; splitting is table stakes.
- **A flat list of panes with fractional rects.** Simpler to compute and
  wrong the first time you close a pane: with no tree there is no sibling
  to give the space back to, and every close needs a re-pack heuristic.
  The tree makes "the sibling takes the space" a pointer splice.
- **A framebuffer per pane, composited at present time.** More memory and
  an extra copy per frame, and `tt_window_present` would have to learn
  about multiple bitmaps. One bitmap with a rectangle per rasterizer keeps
  the window seam exactly as ADR 0005 defined it.
- **A thread per pane.** The invariant ADR 0001 exists to prevent: one
  mutator of the VT core. `poll(2)` already scales to the fd count.
- **Draggable dividers.** The ratio is in the tree and resizing it is a
  few lines, but it needs a hit-tested drag region, a cursor change and a
  minimum-size policy. 50/50 splits plus window resizing cover the case;
  this is additive behind `tt_node.ratio` when it is wanted.

## Consequences

- `tnytty gui` is now a multi-session client of the registry, so the HTTP
  API (ADR 0002) lists every pane of the window as a session. That is the
  behaviour we want — each pane is scriptable and shareable — and it means
  a `--listen` window's session list grows and shrinks as panes come and
  go.
- A window is capped at `MAX_PANES` (32) panes, which bounds the `poll`
  set and the leaf arrays the loop walks without allocating per frame.
- **Kitty graphics are still not drawn in the window** (ADR 0005). Panes
  do not change that; placements will be per pane when they land, because
  the geometry is now per pane.
- `divider` is a new config key. Config keys are additive; removing or
  renaming one needs an ADR ([config.md](../config.md)).
- Splitting spawns the same command the window was started with (or
  `$SHELL`), inheriting the launch working directory — not the focused
  pane's current directory, which tnytty cannot know without OSC 7. That
  becomes possible, additively, when the core records OSC 7.
