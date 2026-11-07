# 0217 — Codex images default to GPT Image 2.6 Sunburst

Date: 2026-11-07
Status: accepted

## Context

OpenAI released GPT Image 2.6 on 2026-11-04 as two API models,
`gpt-image-2.6-sunburst` (detailed work, edit consistency) and
`gpt-image-2.6-flare` (fast iteration). The image guide maps 2.5 Sunburst to
2.6 Sunburst and 2.5 Flare to 2.6 Flare, and documents the same six quality
values. ADR 0084 pinned the Codex image default to `gpt-image-2.5-sunburst`.

The Codex CLI itself does not yet select 2.6: as of 2026-11-06, a public
report (openai/codex#59890, no maintainer reply) finds no `gpt-image-2.6`
strings in the shipped CLI, says its built-in image tool sends `gpt-image-2`,
and observes that the ChatGPT-account image routes currently answer every
model string the same way. tny sends an explicit model, so it requests the
newest one and benefits once the account route honours the field.

## Decisions

1. Supersede ADR 0084 decision 1's model default only: Codex generation and
   editing select `gpt-image-2.6-sunburst` when no model is given. The
   default stays in the provider table (`src/core/image_codex.c`); the
   `high` quality default, quality validation and independence from the
   conversation model are unchanged.
2. Model strings remain free-form overrides. `gpt-image-2.6-flare` is the
   advertised alternative; `gpt-image-2.5-*` and `gpt-image-2` still pass
   through unchanged. No allowlist is added and no fallback retry is made on
   provider rejection.
3. 2.6-only parameters (`complexity`, markup edits, `svg`/`gif` output) are
   out of scope until the account route accepts them.
4. Wasm behavior is unchanged: same adapter and validation through the
   existing seams.

## Verification

HTTP fixtures in `tests/integration/test_image_service.py` and
`tests/integration/test_image_workflow.py` check the new default in
generation/editing payloads, manifests, schemas and help, both 2.6 models
plus an explicit 2.5 override with every quality level. No live generation
was run: the change environment had no ChatGPT login.

## Primary sources

- [OpenAI model list](https://developers.openai.com/api/docs/models/all), verified 2026-11-07
- [OpenAI image prompting guide](https://developers.openai.com/api/docs/guides/image-prompting), 2.6 models, quality values and 2.5→2.6 mapping, verified 2026-11-07
- [Codex issue #59890](https://github.com/openai/codex/issues/59890), user report on Codex CLI 2.6 support, read 2026-11-07
