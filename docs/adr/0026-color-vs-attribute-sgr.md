# 0026 — Color is not structure: NO_COLOR gates SGR colors, never attributes

Date: 2026-08-24
Status: accepted

## Context

A sandboxed run (pty allocated, `NO_COLOR` set in the image — common in
cloud/CI environments) rendered the TUI with the block layout intact but
zero SGR: the status row printed as ordinary text instead of a reverse-video
bar, the banner lost its bold, the composer prompt lost its green.

The root cause was a single gate: `t.color = tty && !NO_COLOR` fed
`tui_c()`, which suppressed *every* SGR sequence. But the NO_COLOR
convention disables *color*; bold, dim, and reverse video are attributes,
and the status bar's reverse video is structural — it is the only thing
separating the bar from transcript text. Three adjacent gaps made the
failure mode worse:

- There was no way to force color back on when `NO_COLOR` is set.
- `ctx->no_color` was declared but never assigned; `ask --no-color` was a
  no-op.
- Dumb (no-pty) mode dropped the status row and composer silently; the only
  hint was approvals answering "not a terminal: denied".

## Decision

**Two gates, resolved once at startup by
`tny_color_resolve(ctx, tty, &color, &attr)`:**

- `tui_c()` gates SGR *colors* on `t->color`.
- `tui_attr()` gates non-color SGR (bold, dim, reverse, reset) on `t->attr`.
  Mixed sequences split at the call site (`\x1b[1;36m` becomes
  `tui_attr("\x1b[1m")` + `tui_c("\x1b[36m")`), so the bold selection in the
  popover and the bold prompt survive colorless setups.

Precedence, strongest first:

1. `--color=never` / `--no-color` (the flag wires up `ctx->no_color`): no
   SGR at all — the zero-escape hatch for terminals and recorders that
   mangle SGR. The status row falls back to `── … ──` delimiters.
2. `--color=always` / `CLICOLOR_FORCE` (non-empty, not `"0"`): full styling,
   even when piped (`grep --color=always` semantics), beating `NO_COLOR`.
3. `NO_COLOR` (presence-based; even an empty value counts, as before):
   colors off, attributes on.
4. Default: both on when stdout is a tty, both off when piped.

When multiple CLI color flags are supplied, the last one selects the explicit
mode before this environment and tty precedence is applied.

**Dumb mode surfaces itself:** the banner adds `not a terminal: status bar
disabled, approvals auto-deny`, and each turn ends with a plain
`── provider  model  in/out tok ──` transcript line so piped logs keep some
status. The same verification pass caught a pre-existing livelock: macOS
polls `/dev/null` stdin back as `POLLNVAL`, which the run loop's
`POLLIN|POLLHUP|POLLERR` mask excluded — `poll` returned instantly forever
and `read` never ran. `POLLNVAL` is now treated as stdin-is-gone (EOF).

## Consequences

- Sandboxes and CI images that ship `NO_COLOR` keep the status bar's visual
  identity exactly; only hues disappear.
- `NO_COLOR=1 tny --color=always` (or `CLICOLOR_FORCE=1`) recovers full
  styling without touching the environment.
- Dim reasoning (`tui_write_dim`) is an attribute: it survives `NO_COLOR`
  and disappears only with `--color=never` or no tty. ADR 0012's per-line
  SGR invariant is unchanged.
- `ask` output was already plain; its `--no-color` flag now sets
  `ctx->no_color` for real instead of being a no-op.
- Unit tests pin the resolution matrix (`color_resolve_matrix`), the gate
  split, and both status-row renderings in `tests/test_tui.c`;
  `tests/test_core.c` pins flag parsing and validation.
