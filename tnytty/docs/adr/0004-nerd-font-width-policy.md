# 0004 — PUA codepoints (nerd fonts) are width 1; width comes from built-in tables

Date: 2026-08-29
Status: accepted

## Context

Nerd-font glyphs live in the Unicode Private Use Area (U+E000–U+F8FF and
plane-15/16 PUA). `wcwidth(3)` is locale-dependent and returns -1 or
inconsistent widths for PUA and for recent emoji, which is exactly how
prompt icons and file-type glyphs get misaligned in lesser terminals.
Powerline/nerd-font conventions assume the glyphs are single-cell.

## Decision

The core never calls `wcwidth`. `vt_cp_width()` uses built-in tables:

- combining marks and zero-width characters → 0;
- East-Asian Wide/Fullwidth blocks, CJK plane-2/3, Hangul syllables,
  wide emoji blocks → 2;
- **all PUA codepoints → 1**, deliberately including plane 15/16;
- everything else printable → 1.

Renderers may draw a nominally-1-cell nerd glyph with overhang (as kitty
does) but the *model* is one cell, so cursor math, wraps, and the HTTP
screen dump agree with what nerd-font-aware terminals do.

## Consequences

- Deterministic layout across platforms and locales; the split-boundary
  test suite can assert exact cells for icon-bearing prompts.
- A future variation-selector/grapheme upgrade (emoji ZWJ sequences)
  extends the tables and gets its own ADR; it does not reintroduce
  locale dependence.
