# 0001 — The VT engine is a headless, I/O-free library; everything else is an adapter

Date: 2026-08-29
Status: accepted

## Context

tnytty must run on macOS, Linux, Windows, and iOS, expose sessions over
HTTP, and stay tiny. iOS forbids fork/exec entirely; Windows ptys are
ConPTY, not `/dev/ptmx`; a shared session may have zero local display. A
terminal emulator whose screen model calls `read`/`write`/`ioctl`
directly cannot satisfy any of that.

## Decision

`src/vt/` is a pure state machine: `vt_feed()` consumes bytes, queries
read state, and the only outward paths are caller-provided callbacks
(`respond` for DSR/DA answers, `graphics` for kitty APC passthrough).
It allocates through the caller-visible init/free pair and touches no
fd, no signal, no global. The pty (`src/term/`), the HTTP API
(`src/api/`), and the CLI (`src/main.c`) are adapters that own all I/O,
multiplexed by one `poll(2)` loop.

## Consequences

- The core compiles unchanged for iOS, wasm, and unit tests; the test
  suite drives it byte-by-byte with no mocks.
- Terminal questions (cursor position report) answer through the
  adapter, which decides where the answer goes (the pty), keeping the
  core testable and the data flow one-directional.
- Renderers are additive: a platform view consumes the same read API the
  HTTP screen endpoint does.
