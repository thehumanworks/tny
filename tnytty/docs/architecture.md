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

### `src/api/` — the HTTP adapter

A minimal HTTP/1.1 server on the shared `picohttpparser`, serving the
REST surface in [http-api.md](http-api.md). Request handling is a pure
function from (method, path, body, auth) to a response buffer, so the
router is unit-testable without sockets. Auth per
[ADR 0002](adr/0002-http-api-and-auth.md).

### `src/cli/` + `src/main.c` — the CLI adapter

`tnytty run` attaches the controlling tty raw to a session (passthrough
bytes both ways, mirror into the vt so the session is scriptable while a
human uses it); `tnytty serve` runs headless API-only;
`tnytty icat` encodes images to kitty graphics escapes. Flags in
[cli.md](cli.md).

## One event loop

A single `poll(2)` loop multiplexes: the listening socket, HTTP
connections, every session's pty master, and (in `run`) stdin +
`SIGWINCH` via a self-pipe. No threads; no busy waits. Blocking waits
never bypass the loop.

## Vendored dependencies

From the shared root `third_party/` only: `picohttpparser` (HTTP
requests), `yyjson` (JSON in/out), `greatest` (tests). The VT core
depends on none of them.
