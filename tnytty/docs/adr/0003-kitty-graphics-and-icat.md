# 0003 — Kitty graphics passes through and is recorded; `icat` is a built-in subcommand

Date: 2026-08-29
Status: accepted

## Context

Images in terminals have three live protocols: kitty graphics (APC `G`),
iTerm2 (OSC 1337), and sixel. Kitty's is the one modern terminals and
TUIs converge on (kitty, ghostty, WezTerm, konsole), supports PNG
passthrough without decoding, and is what tny's own image plans assume.
kitty ships `icat` as a separate kitten; tnytty's size budget makes a
second binary pointless.

## Decision

- The VT core parses APC strings and recognizes `G` payloads: control
  keys are parsed (`a`, `f`, `m`, `i`, `q`), a bounded per-session
  record of graphics commands is kept for API visibility, and the whole
  raw sequence is forwarded through the `graphics` callback so an
  attached renderer or passthrough tty shows the image live.
- Phase 1 records commands; cell-anchored placements (position, z-index,
  delete-by-id) are phase 2.
- `tnytty icat` is a subcommand of the one binary. PNG files transmit
  as-is (`f=100`, `a=T`), base64 in 4096-byte chunks with `m=1`/`m=0`
  continuation — no image decoding linked into the binary. Non-PNG input
  is a clean error naming the phase-2 plan (decode to RGBA `f=32`).
- Sixel and iTerm2 protocols are out of scope until an ADR says
  otherwise.

## Consequences

- `tnytty icat x.png` works in any kitty-protocol terminal today,
  independent of the emulator core.
- The binary carries a base64 encoder and a PNG magic check, not libpng:
  the size budget holds.
- API consumers can see that (and how many) graphics landed in a session
  even before phase-2 placement geometry exists.
