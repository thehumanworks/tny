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

For `gui`, that registry lives in a detached per-user broker, not in the
AppKit process ([ADR 0007](adr/0007-durable-session-broker.md)). The broker
keeps ptys drained and VT state current with zero attached windows. The GUI
holds session IDs and renderer-side VT mirrors populated from versioned
snapshots; it never owns the child process.

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

The broker serves this same surface on a mode-0600, same-uid Unix socket.
`gui --listen` asks the broker to add a TCP listener to that same registry;
it does not start a proxy or a second session store.

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
human uses it); `tnytty gui` attaches a native window to broker sessions,
holding one session ID and VT mirror per split pane;
`tnytty serve` runs headless API-only;
`tnytty icat` encodes images to kitty graphics escapes. Flags in
[cli.md](cli.md).

## One event loop

Each process has one `poll(2)` loop and no shared VT mutator. The `run` and
`serve` loops multiplex their listener, HTTP connections, pty masters and
(in `run`) stdin + `SIGWINCH`. The detached GUI broker similarly owns every
broker pty plus its private and optional public HTTP listeners. The AppKit
frontend loop polls its broker snapshot request and pumps window events.

A session's pty master is polled for `POLLIN` always and for `POLLOUT`
while its input queue is non-empty; on `POLLOUT` the owner calls
`tt_session_flush`. Writers therefore never block the loop and never
lose bytes. The `run`/`serve` process or detached GUI broker does this,
including for headless sessions created through their HTTP listeners.
Each ready pty gets a bounded number of reads per turn, so a continuous
producer cannot starve signals, HTTP, window events, or sibling panes.

Foreground adapters mark their terminal or pane sessions as attached.
The public HTTP API can read their screen and write input, but cannot resize
or destroy them behind the frontend's ownership. Geometry stays with the tty
or GUI; explicit pane/tab close goes through the same-uid broker control path.

Explicit session teardown sends `SIGHUP` to the child process group. A child
that does not exit within 100 ms is sent `SIGKILL` and reaped. Cmd-W and
Cmd-Shift-W use that path. Cmd-Q, red-window close and frontend termination
save topology and detach instead, leaving broker-owned sessions running.

AppKit's event queue is not a pollable fd, so the GUI poll timeout is bounded
at 8 ms and each turn also drains the queue and presents dirty rows. The
broker, rather than AppKit, fairly drains every pane pty. The GUI main thread
is the only thread that touches its renderer-side VT mirrors
([ADR 0005](adr/0005-native-renderer-and-macos-window.md),
[ADR 0006](adr/0006-split-panes-and-the-layout-tree.md)).

## Vendored dependencies

From the shared root `third_party/` only: `picohttpparser` (HTTP
requests), `yyjson` (JSON in/out), `greatest` (tests). The VT core
depends on none of them.
