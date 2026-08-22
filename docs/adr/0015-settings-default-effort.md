# 0015 — settings.json can carry a default reasoning effort

Date: 2026-08-22
Status: accepted (amends the "never persisted" note in ADR 0009)

## Context

ADR 0009 made reasoning effort process-memory only: `--effort`, env
`TNY_REASONING_EFFORT`, TUI `/effort` — never persisted, so a scripted
`tny ask --effort X` cannot change what tomorrow's interactive session
does. That rationale is about tny **writing** the setting behind the
user's back. It accidentally also ruled out the user writing their own
default, forcing an env export or a flag on every invocation when someone
always wants, say, `high` on codex. Provider and model already have lean
settings defaults (`last_provider`, `models.{provider}`); effort was the
odd one out.

## Decision

`~/.tny/settings.json` accepts a user-authored `"effort"` default:

- a string applies to every provider: `{"effort": "high"}`
- an object is per-provider, same shape as `"models"`:
  `{"effort": {"codex": "xhigh", "openai": "medium"}}` (named profiles
  count; unlisted providers stay at provider default)
- `"default"` or empty entries mean provider default (unset).

Precedence, strongest first: `--effort` / `/effort` (an explicit
`default` included) → `TNY_REASONING_EFFORT` → settings `"effort"` →
provider default. Implementation (`apply_provider_effort`,
`src/core/config.c`) runs when the provider resolves, because the
per-provider lookup needs the resolved name; two ctx markers keep the
chain honest:

- `effort_explicit` — set by the flag and by `/effort` (any value): a
  settings default must never override an explicit choice, including
  across a TUI `/provider` re-resolve.
- `effort_from_settings` — the current value came from settings, so a
  `/provider` switch recomputes it for the new provider instead of
  leaking one provider's default into another. An env value is not
  marked and therefore survives provider switches, as before.

The half of ADR 0009 that survives unchanged: **tny never writes the
effort to settings.json.** `/effort` remains process-memory; the settings
entry is edited by the user (or `tny setup`, later), not by turns.

## Consequences

- Lean invocations: `tny ask "…"` picks up provider, model, and now
  effort from settings; flags stay available for one-off overrides.
- Values pass through the existing ADR 0009 mapping at send time
  (canonical `light` → openai wire `low`, etc.), so catalogs and
  provider-advertised tokens behave identically to the flag path.
- Tests: unit — `effort_settings_global_default`,
  `effort_settings_per_provider` (including the `/provider` recompute),
  `effort_env_beats_settings`, `effort_flag_beats_settings` (including
  `--effort default` clearing a settings value); integration —
  `test_openai.py` proves a settings-only effort rides the request mapped
  to the wire vocabulary and that `--effort default` suppresses the field.
