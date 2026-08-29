# tnytty — architecture

## Shape

```text
            keystrokes / HTTP POST input          bytes from child
                     │                                   ▲
                     ▼                                   │
   ┌──────────┐   write   ┌─────────┐   read   ┌─────────────────┐
   │ adapters │ ────────► │   pty   │ ───────► │  vt core (lib)  │
   │ cli/api  │           │  seam   │          │ grid+parser+gfx │
   └──────────┘           └─────────┘          └─────────────────┘
        ▲                                               │
        └── screen dumps (text/JSON), passthrough bytes ┘
```

Three layers, one rule: **the core is headless and I/O-free**
([ADR 0001](adr/0001-headless-core-and-renderers.md)).

### `src/vt/` — the VT core

A pure state machine: `vt_feed(vt, bytes, len)` consumes output from the
child program and updates a cell grid. It performs no I/O; when the
emulated program asks a question (DSR cursor report, DA attributes), the
core emits the answer through a caller-provided `respond` callback, and
kitty graphics APC payloads go through a `graphics` callback plus a
per-session record. Everything the API serves — text screen, JSON screen,
title, cursor, graphics list — is a read of this state.

Cells store a codepoint, one combining mark, packed attributes, and
tagged fg/bg colors (default / 256-indexed / truecolor). Width comes from
built-in tables ([ADR 0004](adr/0004-nerd-font-width-policy.md)):
East-Asian wide blocks and emoji are 2, combining marks are 0, PUA is 1.
Wide glyphs occupy a lead cell plus a continuation cell.

The parser is incremental: UTF-8 sequences, CSI parameters, and OSC/APC
strings all survive arbitrary read-boundary splits. This is enforced by
`vt_every_split_boundary` in `tests/test_vt.c`, mirroring the harness's
`chunked_survives_every_split_boundary` rule from the root `AGENTS.md`.

### `src/term/` — the pty seam

`pty.h` is the platform seam; `pty_posix.c` implements it with
`posix_openpt`/`fork`/`TIOCSCTTY` and `TIOCSWINSZ` for resize. Other
platforms implement the same header (ConPTY on Windows, none on iOS —
see [platforms.md](platforms.md)); platform code lives only behind this
seam, never in `#ifdef`s sprinkled through the core.

### `src/session/` — the registry

A session is `pty + vt + metadata` (id, argv, size, creation time,
exit status). The registry owns lifecycle and is the single source both
adapters address sessions through. IDs are short random hex, never
guessable-sequential, because they appear in shared URLs.

**Input is queued, never dropped.** A pty master takes only a kernel
buffer's worth of input before it returns `EAGAIN`; discarding the rest
truncates input mid-sequence (lose the `ESC` of `ESC[201~` and a literal
`[201~` lands at the prompt). `tt_session_write` writes what the fd will
take and appends the remainder to a per-session pending queue, so it is
all-or-nothing from the caller's side: every byte is accepted, in order,
and later writes queue behind earlier ones. `tt_session_pending` reports
the backlog; `tt_session_flush` drains it.

The queue is capped at 4 MiB per session (`TT_INPUT_QUEUE_MAX`). Past
the cap a write is rejected whole — nothing queued — with `ENOBUFS`,
which the HTTP adapter reports as `503 {"error":"input queue full"}`
([http-api.md](http-api.md)). Adapters that own their input source apply
back-pressure instead: `tnytty run` stops polling the attached tty above
`TT_INPUT_HIGH_WATER` (1 MiB) and resumes when the child catches up, so
the cap is reachable only through the API.

### `src/api/` — the HTTP adapter

A minimal HTTP/1.1 server on the shared `picohttpparser`, serving the
REST surface in [http-api.md](http-api.md). Request handling is a pure
function from (method, path, body, auth) to a response buffer, so the
router is unit-testable without sockets. Auth per
[ADR 0002](adr/0002-http-api-and-auth.md).

### `src/ui/` — the native renderer

The window seam (`window.h`) sits beside the pty seam: one
implementation per platform (`window_macos.c`; `window_stub.c` for the
rest), and nothing above it names AppKit. `render.c` is a CPU cell
rasterizer that reads the same getters the HTTP screen endpoint reads
and paints an RGBA framebuffer, repainting only rows whose cells
changed; glyph coverage masks come from the platform through one
callback, so cell geometry, dirty rows and clipping unit-test with a
stub. `keys.c` turns a classified key press into pty bytes (and decodes
the Command chords the window binds), `layout.c` is the binary split tree
that gives each pane its rectangle, and `gui.c` is the `tnytty gui`
adapter. A window's panes share one framebuffer, one rasterizer each.
Decided in [ADR 0005](adr/0005-native-renderer-and-macos-window.md) and
[ADR 0006](adr/0006-split-panes-and-the-layout-tree.md).

### `src/cli/` + `src/main.c` — the CLI adapter

`tnytty run` attaches the controlling tty raw to a session (passthrough
bytes both ways, mirror into the vt so the session is scriptable while a
human uses it); `tnytty gui` attaches a native window instead, holding
one session per split pane;
`tnytty serve` runs headless API-only;
`tnytty icat` encodes images to kitty graphics escapes. Flags in
[cli.md](cli.md).

## One event loop

A single `poll(2)` loop multiplexes: the listening socket, HTTP
connections, every session's pty master, and (in `run`) stdin +
`SIGWINCH` via a self-pipe. No threads; no busy waits. Blocking waits
never bypass the loop.

A session's pty master is polled for `POLLIN` always and for `POLLOUT`
while its input queue is non-empty; on `POLLOUT` the loop calls
`tt_session_flush`. Writers therefore never block the loop and never
lose bytes. `run`, `serve`, and `gui` all do this.

`tnytty gui` keeps that loop and adds the window: AppKit's event queue is
not a pollable fd, so the poll timeout is bounded (8 ms) and each turn
also drains the queue and presents the dirty rows. A split window puts
**every pane's** pty master in that same set, with the same `POLLOUT`
drain — panes are more fds in one loop, not more loops and not threads.
The main thread stays the only thread that touches a VT core
([ADR 0005](adr/0005-native-renderer-and-macos-window.md),
[ADR 0006](adr/0006-split-panes-and-the-layout-tree.md)).

## Vendored dependencies

From the shared root `third_party/` only: `picohttpparser` (HTTP
requests), `yyjson` (JSON in/out), `greatest` (tests). The VT core
depends on none of them.
