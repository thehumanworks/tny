# tnytty — product

## What it is

tnytty is **the tiny terminal**: a terminal emulator whose core is a
headless C11 library, wrapped by a CLI. It runs real programs in a real
pty, models the screen faithfully (UTF-8, wide glyphs, 256/truecolor SGR,
scrollback, alternate screen, kitty graphics), and exposes every session
over a REST HTTP API so terminals become scriptable objects: read the
screen, type keys, resize, share a live session with another person or
program.

Same principles as the tny harness:

- **Smallest binary.** One static-friendly C11 executable; vendored
  dependencies only (`yyjson`, `picohttpparser`, `greatest` from the
  shared `third_party/`). No ncurses, no GUI toolkit in the core.
- **Fast interactions and startup.** `--help`/`--version` never touch a
  pty or socket; `tnytty run` reaches a live shell in milliseconds; one
  `poll(2)` event loop, no threads in the hot path.
- **Cross-platform.** POSIX (macOS, Linux) first; Windows via ConPTY and
  iOS via remote attach are contract-level targets ([platforms.md](platforms.md)).
- **Docs are the contract.** Behavior changes update this tree; decisions
  get ADRs.

## Who it is for

- **Humans** who want a lean terminal: `tnytty run` is a passthrough
  terminal-in-a-terminal today and the engine for platform renderers
  tomorrow.
- **Agents and scripts** that need a terminal as a service: create a
  session over HTTP, run a TUI inside it, read the rendered screen as
  text or JSON, send keys, and tear it down — no scraping, no expect.
- **Session sharing**: expose the API (with a token) and a collaborator
  or a bot can watch and drive the same live session over plain REST.

## Feature pillars

1. **Faithful VT engine** — xterm-compatible subset: cursor addressing,
   erase/insert/delete, scroll regions, alternate screen, SGR with 16/256/
   truecolor, UTF-8 with correct wide/combining handling, scrollback,
   title, bracketed paste, cursor reports (DSR/DA).
2. **Nerd fonts render right** — PUA glyphs are single-width by policy
   ([ADR 0004](adr/0004-nerd-font-width-policy.md)); the core never
   depends on locale tables.
3. **Kitty graphics protocol** — APC `G` sequences are parsed, recorded
   per session, and passed through to attached renderers; `tnytty icat`
   is bundled ([ADR 0003](adr/0003-kitty-graphics-and-icat.md)).
4. **Scriptable, shareable** — the REST API ([http-api.md](http-api.md))
   is a first-class product surface, not a debug port.

## Non-goals

- Not an IDE, not a multiplexer replacing tmux (no panes/windows in the
  core; sessions are the unit).
- No font rasterization in the core: rendering glyphs is the platform
  renderer's job; the core deals in codepoints, widths, and attributes.
- No web frontend in this tree (a browser renderer over the HTTP API is a
  natural later app, but it is not phase work here).
- No shell: tnytty spawns `$SHELL` (or an explicit command); it does not
  implement one.

## Success metrics

- Stripped release binary **< 500 KiB** on Linux x86_64 (`wc -c`), well
  under the harness's 2 MiB ceiling; growth needs an ADR.
- `tnytty --help` and `--version` complete in **< 5 ms**.
- `tnytty run` first prompt visibly interactive in **< 50 ms** over the
  spawned shell's own startup.
- The VT suite passes with every input split at every byte boundary
  (streaming parsers must not care where reads split).
- `make -C tnytty test` green with ASan/UBSan on Linux and macOS.
