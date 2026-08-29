# tnytty — platforms

The core is platform-free C11; platform code lives only behind the pty
seam (`src/term/pty.h`) and the event loop. Every platform states its
behavior here — works / remote-only / clean error — and CI enforces the
stated behavior, mirroring the root wasm-parity rule.

| Platform | Phase 1 | Target end-state |
| --- | --- | --- |
| Linux (glibc, musl) | **works** — `run`, `serve`, `icat`, full API; CI-built and tested | first-class, static musl publish builds |
| macOS (arm64) | **works** — same as Linux; CI-built and tested | first-class |
| Windows | **clean error** from `run`/`serve` ("pty: not supported on this platform yet"); `icat` works (it is pure stdout) | `pty_win.c` on ConPTY (phase 4); MSYS2 first like the harness, native Win32 later |
| iOS | **remote-only** by design: iOS forbids fork/exec, so no local pty ever | tnytty as renderer + HTTP client attaching to remote sessions over the [HTTP API](http-api.md); the VT core compiles unchanged (it is I/O-free) and renders shared sessions on-device (phase 4) |

## Rules

- New platform support = a new `pty_*.c` behind the existing header plus
  a row update here plus CI coverage. Never `#ifdef` platform branches
  into `src/vt/` or `src/api/`.
- The VT core must always compile on every target above, including iOS
  and wasm — it takes bytes and returns state, nothing else. Anything
  that stops that is a core bug.
- Renderers (Metal/UIKit on Apple platforms, a Win32/DirectWrite view, a
  browser view) are adapters over the core + API and live outside this
  tree until they exist; fonts (nerd fonts included) are rasterized by
  renderers, never by the core ([ADR 0004](adr/0004-nerd-font-width-policy.md)).
