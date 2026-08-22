# 0010 — `--fast` is a provider capability, not a universal knob

Date: 2026-08-22
Status: accepted

## Context

Several providers sell a paid low-latency tier, each under its own name and
in its own place on the wire:

- **OpenAI-compatible** — `service_tier` on the chat-completions request.
  OpenAI renamed "priority processing" to "fast mode"; the API accepts both
  `priority` and `fast`, and every compatible gateway that knows the field
  accepts `priority`.
- **Codex app-server** — `thread/start.serviceTier` (`priority`), fixed for
  the thread's lifetime, so changing it means a new thread.
- **Cursor bridge** — not a request field at all: a per-model
  `ModelSelection.params` entry `{"id":"fast","value":"true"|"false"}`. A
  model's *default* variant may already be the fast one; omitting the param
  keeps that default.
- **ACP** — `session/new` has no tier field.

Before #9, `/fast` existed only as the codex service tier. #7 (ADR 0009)
added reasoning effort, which on cursor also rides `ModelSelection.params`.
The two landed in parallel and had to be merged.

## Decision

**A fast tier is an advertised capability (`TNY_CAP_FAST` in
`tny_backend_caps`, `src/core/backend.c`), selected once in
`ctx->service_tier`, and mapped to each provider's wire by the backend.
Providers without the bit reject `--fast` at startup (exit 1, listing the
capable providers) rather than silently dropping it.**

- `ctx->service_tier` is `NULL` (provider default), `fast`/`priority`
  (the paid tier — both spellings, `tny_tier_is_fast`), or `default`
  (explicitly the standard tier). It is process memory only, like effort.
- Wire: openai `"service_tier":"priority"`; codex `serviceTier:"priority"`;
  cursor `{"id":"fast","value":"true"}` (`"false"` for `default`).
- **Cursor composes effort and fast into one `params` array**
  (`append_model_selection`, `src/backends/cursor/cursor.c`). The bridge
  drops unknown ids silently, so the integration mock
  (`tests/integration/mock_bridge.py`) asserts each param independently and
  rejects any other id; `test_cursor.sh` run 5 sends both flags together.
- `SendOptions.model` is sent only when effort or tier is set, so runs
  without either keep the model's own default variant.
- TUI `/fast [fast|priority|default]` mirrors the flag. On codex it rebinds
  (the tier lives on `thread/start`); on openai and cursor it applies from
  the next turn.

## Consequences

- Adding a tier to a new backend = set the capability bit + map the value;
  the CLI, TUI, help text, and the startup error need no change.
- Users see an error instead of a silent no-op when a provider has no tier
  (ACP, and any future provider until it opts in).
- The "priority" spelling is what goes on the wire everywhere, so older
  gateways keep working even though the user-facing word is "fast".
- Mutation targets in `tests/mutation/mutate.py` cover `tny_backend_caps`,
  `tny_tier_is_fast`, and the tier lines in each backend.
