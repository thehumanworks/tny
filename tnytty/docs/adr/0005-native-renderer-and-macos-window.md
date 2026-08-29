# 0005 — The native renderer is a CPU cell rasterizer behind a window seam; macOS drives AppKit from C

Date: 2026-08-29
Status: accepted

## Context

[ADR 0001](0001-headless-core-and-renderers.md) promised that renderers
are additive adapters over the headless VT core, and
[platforms.md](../platforms.md) says fonts are rasterized by renderers,
never by the core. Nothing had claimed that promise yet: `tnytty run` is
a passthrough into somebody else's terminal, so tnytty had no pixels of
its own.

The first native window is macOS. That forces four decisions at once:
what draws the cells, what talks to the platform, what the window looks
like, and how a GUI's event queue coexists with the one `poll(2)` loop
that already owns the pty and the HTTP fds. The size budget (< 500 KiB
stripped, [product.md](../product.md)) and the C11-only invariant (root
`AGENTS.md`) constrain all four.

## Decision

### A platform-free seam, one implementation per platform

`src/ui/window.h` is to windows what `src/term/pty.h` is to ptys: open a
window, report its backing size and font metrics, drain queued events
(key, resize, focus, close), blit a bitmap. `window_macos.c` implements
it; `window_stub.c` implements it everywhere else and returns
`gui: not supported on this platform yet`, the clean-error contract
[platforms.md](../platforms.md) requires. No platform `#ifdef` appears in
`render.c`, `keys.c`, or `gui.c`, and the unit suite links neither
backend.

### Cells are rasterized on the CPU; the platform only supplies glyph masks

`src/ui/render.c` reads the VT grid through the same public getters the
HTTP screen endpoint uses and paints an RGBA framebuffer: per-cell
background, glyph, underline/strike, reverse video, faint, and the
caret (filled block when focused, hollow box when not). It repaints only
rows whose cells changed, plus the caret's row when the caret actually
moved — an idle terminal produces zero dirty rows and the window
presents nothing.

Glyphs arrive through `tt_glyph_fn`, which fills an 8-bit coverage mask
with cell-relative `left`/`top`. That one callback is the entire
platform dependency of the rasterizer, so cell-to-pixel mapping, dirty
rows, palette resolution and clipping are unit-tested on Linux with a
stub mask. A mask may be wider than its cell: nerd-font glyphs stay one
cell in the model ([ADR 0004](0004-nerd-font-width-policy.md)) and are
allowed to overhang when drawn, exactly as kitty does.

On macOS the masks come from **CoreText**, cached by (codepoint, bold,
italic) in a fixed 2048-slot open-addressed table. Each miss builds a
one-character `CTLine`, which gets font fallback and non-BMP surrogate
pairs for free — that is what makes PUA/nerd and emoji codepoints render
without a font-matching layer of our own. The blit is a `CGImage` into a
layer-hosted `CALayer`'s `contents`. No Metal, no OpenGL.

### AppKit is called from C through the Objective-C runtime

`window_macos.c` is a `.c` file. It reaches AppKit, CoreText and
CoreAnimation with `objc_getClass` / `sel_registerName` / typed casts of
`objc_msgSend` (with the `objc_msgSend_stret` and `objc_msgSend_fpret`
entry points guarded for x86_64). Frameworks are linked only into that
object: `-framework AppKit -framework CoreText -framework CoreGraphics
-framework QuartzCore -framework Foundation -lobjc`.

There is no `class_addMethod` delegate and no `NSView` subclass. Key
presses are read straight off `nextEventMatchingMask:` before they are
dispatched; resize, focus and close are polled from `bounds`,
`isKeyWindow` and `isVisible` once per loop turn. That removes the only
part of the design that would have needed synthesized Objective-C
classes, and with them the runtime's method-encoding strings.

### The titlebar is transparent by default

Default (`macos-titlebar = transparent`): `titlebarAppearsTransparent`,
`NSWindowStyleMaskFullSizeContentView`, `titleVisibility` hidden, window
background set to the terminal background, dark appearance. The content
therefore runs edge to edge under the traffic lights, and the grid's top
padding is increased by the standard titlebar height (read from
`+[NSWindow contentRectForFrameRect:styleMask:]`, not hardcoded) so no
row is hidden behind them.

`macos-titlebar = opaque` restores the system titlebar and shows the
session title (OSC 0/2). `--titlebar transparent|opaque` overrides the
config file.

### The default palette is contrast-measured, not inherited

The stock xterm palette is a liability on a dark background: SGR 34 is
`#0000ee`, and a bold blue path segment — what most shell prompts draw —
comes out as saturated navy on near-black. tnytty ships a dark palette in
which **every entry except index 0 clears WCAG AA (4.5:1) against the
default background**, and `tests/test_config.c` recomputes the ratios and
fails if a future palette change drops one below the line. Index 0 is
exempt because it is a *background* color; giving it 4.5:1 against the
background would defeat its purpose.

`foreground`, `background` and `palette0`..`palette15` are config keys,
so the measurement is a floor for the shipped defaults, not a cage.
Indices 16–255 stay the fixed xterm cube and grayscale ramp.

Bold is a **heavier face** first: the glyph lookup sees `VT_ATTR_BOLD`
and picks the bold CoreText variant. `bold-brightens` (default on) also
maps an indexed 0–7 foreground to 8–15 — safe precisely because every
bright entry is in the measured table. Faint blends 69 % toward the
foreground rather than the 50 % that would sink the dimmest entries into
the background.

### Selection is platform-free; only the pasteboard is not

`src/ui/selection.c` owns what a click means: character / word / line
mode from the click count, a pivot the drag extends around, wide-glyph
rounding (a continuation cell pulls in its lead, a lead pulls in its
continuation), per-row trailing-blank trimming, and `\n` between rows.
It reads the grid through the same public getters as everything else, so
its whole surface is unit-tested with no window in sight. The rasterizer
draws a selected cell by inverting it — the same primitive as the caret,
and one that needs no theme color of its own.

`window_macos.c` contributes only the parts that must be AppKit: the
mouse events (with their click count and a point converted to device
pixels), and `NSPasteboard` reads and writes of `public.utf8-plain-text`.
Copy-on-select (default on) fires on mouse-up; Cmd-C copies on demand;
Cmd-V writes the pasteboard into the pty, bracketed when the program
enabled mode 2004. A selection is dropped when the text under it changes,
detected by comparing the extracted text against a snapshot rather than
by guessing from scroll events.

### The window answers terminal queries itself

Under `tnytty run` there is an outer terminal to forward DSR/DA answers
to. In the window there is not: tnytty *is* the terminal, and a program
that asks for the cursor position and never hears back hangs. ADR 0001
keeps the core I/O-free, so `src/ui/reply.c` is the adapter half — it
routes `vt_set_respond` output straight into the pty and is platform-free
so the round trip (`ESC[6n` → `ESC[r;cR`) has a unit test.

### A status bar, on the same loop

One line along the bottom edge, drawn by the same rasterizer in the same
font: transient messages ("Copied 42 characters") that clear after two
seconds. `src/ui/status.c` holds the formatting and the deadline and
takes "now" as a parameter, so the expiry is exact in tests and needs no
timer thread — the existing 8 ms poll tick is the clock. The bar's height
comes out of the grid area, so the session's rows are computed from the
pixels that remain; `status-bar = false` returns that row.

### A config file, because there was none

`$XDG_CONFIG_HOME/tnytty/config`, else `~/.config/tnytty/config`:
`key = value` lines, `#` comments, no sections
([config.md](../config.md)). Parsing lives in `src/util/config.c` and is
a pure function over a text buffer, so it unit-tests without a
filesystem. Unknown keys and malformed lines warn and are skipped; a bad
*value* is a clean error, because the user meant that key. The same
`tt_config_set` backs the CLI flags, where an unknown key is an error
rather than a warning — the user typed it just now.

### One loop, main thread, no second mutator

`tnytty gui` runs the same shape of loop as `tnytty run`: `poll(2)` over
the pty master, the signal self-pipe and the HTTP fds, and each turn it
also drains AppKit's queue and presents any dirty rows. AppKit's queue is
not a pollable fd, so the poll timeout is bounded at 8 ms; that is the
worst-case latency a keystroke can sit in the queue, and the cost of the
idle wakeups is one `tt_render_frame` scan (≈20 µs on a 240×80 grid,
measured below).

The rejected alternative was a `CFFileDescriptor` source per fd driving
`CFRunLoopRun`. It is the more native integration, but the HTTP server's
fd set changes on every accept, and the run loop would then own the
program's lifetime instead of the loop tnytty already documents in
[architecture.md](../architecture.md). Bounded polling keeps one loop
that reads the same in `run`, `serve` and `gui`. **No thread but the main
one touches the VT core**, which is the invariant that actually matters.

## Measurements

Apple M-series, macOS 27, `cc -Os`, stripped (`make -C tnytty size`):

| Binary | Bytes | KiB |
| --- | --- | --- |
| before (no `gui`) | 203,232 | 198.5 |
| after (window, renderer, config) | 222,656 | 217.4 |
| after (+ selection, palette, status bar) | 239,760 | 234.1 |

+36,528 bytes (+18 %) in total, well inside the 500 KiB budget; the
CoreText and AppKit code is in the OS, not in the binary.

Rasterizer, 240×80 grid at 2× scale (2192×1596 px), every cell holding a
glyph — the worst case a real screen never reaches:

| Frame | Cost |
| --- | --- |
| full redraw (resize / invalidate) | 5.9 ms |
| one dirty row (typing) | 18 µs |
| idle scan, 0 dirty rows | 18 µs |

The idle scan is what the 8 ms poll tick pays to learn that nothing
changed: 18 µs per tick is 0.2 % of one core.

## Alternatives rejected

- **`.m` files / Objective-C**: the smallest change, and it breaks the
  C11-only invariant in the root `AGENTS.md`. The runtime is a C API;
  using it costs a few typed casts and keeps one language.
- **sokol / SDL / GLFW**: a vendored windowing library is tens of
  thousands of lines for a seam that is ~15 functions wide, and none of
  them gives a transparent macOS titlebar without dropping to AppKit
  anyway.
- **Metal or OpenGL glyph atlas**: the right answer for a 4K terminal at
  120 Hz, and premature here. A dirty-row CPU blit costs 22 µs per
  keystroke and pulls in no shaders, no pipeline state, and no second
  render path to keep in sync with the CPU one. Revisit with an ADR when
  measurements demand it.
- **A second thread pumping AppKit**: two mutators of the VT core, which
  ADR 0001 exists to prevent.
- **Rendering over the HTTP API from a separate GUI process**: doubles
  the latency of every keystroke and makes the local case pay for the
  remote one. The API stays the *sharing* surface (ADR 0002); the local
  window reads the core directly.

## Consequences

- Renderers on other platforms are now a `window_*.c` plus a
  [platforms.md](../platforms.md) row; the rasterizer and key encoding
  are already shared and already tested.
- Adding a config file creates a compatibility surface: new keys are
  additive, and removing or renaming one needs an ADR.
- **Kitty graphics are not drawn in the window yet.** The core still
  parses, records and forwards APC `G` payloads unchanged (ADR 0003), so
  passthrough and the API are untouched; `tnytty gui` simply ignores the
  `graphics` callback. Cell-anchored placements are phase-2 work and land
  with the placement geometry, not before it.
- Overhanging glyphs made the dirty-row optimization subtler than it
  looks: repainting a row clears its whole band, which would slice a
  neighbour's overhang in half. The renderer records, per row, whether
  its last paint put ink above or below its own band, and pulls the
  affected neighbour into the repaint.
- Mouse *reporting* to the child (SGR 1006), scrollback scrolling, a menu
  bar beyond Cmd-Q/Cmd-W/Cmd-C/Cmd-V, and IME/marked text are not in this
  phase; each is additive behind the same seam.
