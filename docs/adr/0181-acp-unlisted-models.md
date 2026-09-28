# 0181 — Unlisted ACP model IDs are requested, not rejected

Date: 2026-09-28
Status: accepted
Supersedes: ADR 0029's requirement that a requested ACP model "must be
advertised". Selection through the agent's model option, confirmation, and the
unset-model default remain in force.

## Context

ACP adapters publish a model catalog at session start. That catalog lags model
releases: `claude-sonnet-5-5` can be served by the account an hour after release
while the installed adapter still lists only older IDs. tny rejected any model
absent from the catalog before contacting the agent, so users could not use a
model they knew to be available until the adapter shipped an update.

## Decision

The catalog informs `models` output and pickers; it never constrains the user.
A nonempty requested model is always sent to the agent through the model select
option (`session/set_config_option`) or the legacy `session/set_model` request,
listed or not. The agent is the sole authority:

- If it confirms the exact requested value (legacy: acknowledges), the turn
  proceeds with that model.
- If it rejects the request or confirms a different value, setup fails before
  the prompt, as for any rejected model. When the model was unlisted the error
  adds "not in the agent's catalog; see `models`" so the user can see why. The
  failure is the user's responsibility; tny does not retry with another model or
  fall back to the default.

Structural requirements are unchanged: an agent with no model option, or a model
select without an `options` array, still fails an explicit model request.
Reasoning effort and permission mode stay limited to advertised values; this
decision applies only to the model, whose catalog is the one that lags releases.

## Verification

- `tests/formal/acp/Model.lean` (Lean 4, no `sorry`; axioms: `propext` only)
  proves that the catalog never changes whether a turn proceeds or with which
  model, that a prompt uses only the requested model, that config selection
  requires the agent's confirmation, that rejection always fails, and that the
  catalog note appears exactly when an unlisted model was sent. `Export.lean`
  writes `golden/models.tsv`, replayed against the real client by
  `test_model_selection_matches_lean` in `tests/integration/test_acp_client.py`.
- ast-grep rules `acp-model-catalog-not-a-gate` and
  `acp-legacy-model-request-first` (`make lint-ast`, part of `make quality`)
  fail if a client-side catalog rejection returns.

## Consequences

- New models work as soon as the agent serves them, without adapter updates.
- A typo in `--model` now reaches the agent and fails with the agent's error
  (annotated as unlisted) instead of tny's own message. No prompt is sent either
  way.
