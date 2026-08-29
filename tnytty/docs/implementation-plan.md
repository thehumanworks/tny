# tnytty — implementation plan

Ordered phases; each has an acceptance gate. Do not skip ahead. Phase 1
ships in the monorepo-creation change; later phases are the contract for
follow-ups.

## Phase 1 — headless core + POSIX terminal + API (this change)

- VT engine (`src/vt/`): incremental parser (C0, ESC, CSI, OSC, APC),
  cell grid, SGR 16/256/truecolor, UTF-8 with width tables, wrap,
  scroll regions, alternate screen (1049), scrollback, title, DSR/DA
  responses, kitty graphics APC capture + passthrough.
- POSIX pty seam (`src/term/pty_posix.c`), session registry.
- HTTP API (`src/api/`): health, session CRUD, screen (text/JSON),
  input, resize; bearer-token auth for non-loopback binds.
- CLI: `run` (raw passthrough + optional `--listen`), `serve`, `icat`
  (PNG via kitty `f=100`, chunked base64).
- Tests: `tests/test_vt.c` (including every-split-boundary),
  `tests/test_icat.c`, `tests/test_http.c`; ASan/UBSan debug builds.

**Gate:** `make -C tnytty test` green on Linux + macOS CI; stripped size
reported by `make -C tnytty size`; root `make quality` passes with
tnytty sources included.

## Phase 2 — protocol depth

- Mouse reporting (SGR 1006), focus events, OSC 52 clipboard (gated),
  DECSCUSR cursor styles, rectangular ops as needed by real TUIs
  (vim, htop, tny itself as the canonical workloads).
- Kitty keyboard protocol passthrough; kitty graphics placements
  tracked with cell geometry (delete-by-id, z-index) instead of a flat
  record list.
- `icat`: JPEG/GIF via decode-to-RGBA (`f=32`), `--place`, tmux
  passthrough wrapping.

**Gate:** vttest core screens pass; vim/htop sessions render byte-exact
against recorded fixtures.

## Phase 3 — sharing and streaming

- `GET /v1/sessions/{id}/events`: SSE stream of screen deltas and
  graphics events (the polling API stays; streaming is additive).
- Read-only share tokens vs read-write tokens; `tnytty attach` over
  HTTP to join a remote session from another tnytty.
- Multi-writer input ordering and a viewer count on the session object.

**Gate:** two tnytty processes on different machines share one live
session; a script drives a TUI over SSE + POST with no polling loop.

## Phase 3.5 — the first native renderer (shipped)

- Window seam (`src/ui/window.h`) with `window_macos.c` (AppKit through
  the Objective-C runtime, from C) and `window_stub.c` elsewhere; CPU
  cell rasterizer (`src/ui/render.c`); key encoder (`src/ui/keys.c`);
  `tnytty gui` (`src/ui/gui.c`); config file (`src/util/config.c`).
- Titlebar transparent by default, opaque on request
  ([ADR 0005](adr/0005-native-renderer-and-macos-window.md),
  [config.md](config.md)).

**Gate:** `make -C tnytty test` covers config parsing, dirty rows,
cell-to-pixel mapping and key encoding on every host; `gui` is a clean
error off macOS. Kitty graphics in the window are explicitly deferred to
phase 2's placement geometry.

## Phase 4 — platforms

- Windows: `pty_win.c` on ConPTY; CI job on MSYS2/native as the root
  harness does; `window_win.c` (DirectWrite glyph masks) behind the same
  seam as the macOS window.
- iOS: no fork/pty on-device — tnytty ships as the renderer + HTTP
  client attaching to remote sessions ([platforms.md](platforms.md)).
- wasm: evaluate reusing the harness's `tny_poll`/net seams (root ADR
  0017) for a browser-hosted renderer; needs its own ADR before code.

**Gate:** per-platform behavior stated in [platforms.md](platforms.md)
holds — works / remote-only / clean error, enforced in CI.

## Phase 5 — packaging

- Add tnytty to the nix flake (extend `nix/source.nix`/`nix/tests.nix`
  at the root — the sandbox has only what those files name).
- Release artifacts alongside the harness's in `release.yml`.

**Gate:** `nix flake check` covers tnytty; release publishes stripped
binaries for the CI platform matrix.
