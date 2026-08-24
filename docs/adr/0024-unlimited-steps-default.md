# 0024 — The native agent loop is unlimited by default; step caps are explicit opt-ins

## Status

Accepted.

## Context

The native OpenAI-compatible loop counted model calls per turn against
`ctx->max_steps`, hardcoded to 24 (overridable only by the repo's `.tny.json`
`"steps"` limit). Real tasks routinely need more than 24 tool rounds; hitting
the ceiling ended the turn mid-work with `TNY_STOP_STEP_LIMIT` and there was
no flag, slash command, or setting to raise it. Host providers (cursor,
codex, acp) run their own loops and never consulted the value.

libtny ABI 0 mirrored the same default (`max_steps = 24`) and rejected both
`0` and anything above 256 as "outside the supported range".

## Decision

- **Default: no step limit.** `ctx->max_steps` defaults to `0`, meaning the
  native loop runs for as long as the turn needs. The step counter and
  `TNY_STOP_STEP_LIMIT` machinery stay; the checks simply do not fire when
  the cap is 0.
- **Explicit caps only.** A cap comes from one of:
  - the global CLI flag `--max-steps N` (`unlimited`/`none`/`0` clear it),
  - the TUI command `/max-steps set N` / `/max-steps clear` (bare
    `/max-steps N` also works; no argument shows the current value),
  - the repo's `.tny.json` `"steps"` limit (unchanged semantics; still a
    limit, never authority).
  The explicit flag beats the repo limit — `--max-steps unlimited` is the
  documented way to override a repo cap for one run. `tny_parse_max_steps`
  in `core/config.c` is the single parser for both surfaces.
- **libtny follows.** `tny_runtime_options_init` now sets `max_steps = 0`
  (unlimited) and validation accepts any value; a positive value still caps
  the loop and ends the turn with `TNY_STOP_REASON_STEP_LIMIT`. The 256
  upper bound is gone — an embedder that wants a cap picks its own number.
- **Reporting.** `tny status --json` keeps the `agent_step_limit` field;
  `0` now means unlimited.

## Consequences

- Long agentic turns finish instead of dying at an arbitrary count; runaway
  loops are bounded by the user's interrupt (Esc/Ctrl-C, `cancel`), token
  budgets, and permission gates rather than a silent default.
- Scripts that relied on the implicit 24-step ceiling as a safety net must
  pass `--max-steps 24` (or set `.tny.json` `"steps"`) to keep it.
- Host providers are unaffected either way: the knob only governs tny's
  native loop, and the TUI/CLI help says so.
- `/max-steps` mutates a plain `int` read at step boundaries, so it applies
  immediately — even to the running turn — with no backend rebind and no
  pre-warm interaction.
