# 0009 — Canonical reasoning-effort levels, mapped per provider

Date: 2026-08-22
Status: accepted

## Context

Every backend has grown a reasoning-effort knob, but no two agree on the
vocabulary or the wire location:

- **Codex app-server** — `turn/start.effort` ("for this turn and subsequent
  turns"); each model advertises its levels in `model/list` →
  `supportedReasoningEfforts[].reasoningEffort` (commonly
  `none|minimal|low|medium|high|xhigh`, some models add `max`/`ultra`).
- **Cursor bridge** — `ModelSelection.params` `[{id,value}]` on
  `CreateAgent`/`ResumeAgent` and `SendOptions.model`. Both the parameter id
  and its values are model-specific and discoverable only through
  `ListModels` (`SdkModel.parameters`). Top-level keys and unknown param ids
  are **silently dropped** by the bridge.
- **OpenAI-compatible** — `reasoning_effort` in the request body
  (`none|minimal|low|medium|high|xhigh` depending on model; no `max`).
  Sending the field to a model without it is a 400.
- **ACP** — nothing portable at protocolVersion 1; newer agents expose a
  thought-level session config option tny's client does not consume yet.

Users want one flag that works everywhere and can change mid-conversation.

## Decision

**tny defines six canonical levels — `off light medium high xhigh max` —
and maps them onto each provider's vocabulary at the wire
(`tny_effort_wire`, `src/core/config.c`). Anything outside the canonical set
passes through verbatim so users can pick whatever the provider's catalog
advertises. Where a catalog exists, tny pulls the real levels from it.**

- Mapping: `off`→`none`, `light`→`low` everywhere; `medium|high|xhigh` are
  already shared spellings; `max` stays `max` on codex/cursor and clamps to
  `xhigh` on the OpenAI API, which has no `max`.
- Discovery: `tny models` (and `--json`) surfaces per-model levels —
  codex `supportedReasoningEfforts` (+ `default_effort`), cursor parameter
  definitions. openai has no catalog for this; the canonical set is the
  documented contract. The cursor backend goes further and **resolves** the
  requested level against the catalog before sending, because the bridge
  drops unverified params silently instead of erroring.
- Surfaces: `--effort` / `--reasoning-effort` (leading global flag), env
  `TNY_REASONING_EFFORT`, TUI `/effort` — changeable at any time.
  `default` clears the setting (provider default, field omitted).
- Mid-conversation changes need **no backend rebind**: the value is read at
  send time on every backend (codex `turn/start`, cursor
  `SendOptions.model.params`, openai request body). `/effort` still calls
  `tui_prewarm_drop` before mutating ctx because the cursor
  `create_or_resume` on the warm-up thread reads the effort too (the
  ADR 0002 contract), then re-warms only when no backend is bound.
- Like `/fast`, the setting is process-memory only — never persisted to
  `settings.json`. A scripted `tny ask --effort X` should not change what
  tomorrow's interactive session does.
- ACP degrades loudly-but-gently: one status line per process, agent default
  applies.

## Consequences

- One more ctx string every backend may read at send time; no new
  dependency. Stripped Linux release: 445640 → 449744 bytes (+4.0 KiB),
  still far under the 1.5 MiB budget.
- The canonical names are a tny contract; if a provider renames its levels,
  the mapping table in `src/core/config.c` is the single place to touch.
- The cursor path spends one extra `ListModels` round trip the first time an
  effort is set (cached per value afterwards), usually on the pre-warm
  thread where it is invisible.
- Integration mocks assert the exact wire shape per backend
  (`turn/start.effort`, `ModelSelection.params`, `reasoning_effort`), so a
  regression fails loudly rather than running silently at default effort —
  the exact failure mode the cursor bridge makes easy.
