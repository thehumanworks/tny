# 0084 — Codex images default to Sunburst with high quality

Date: 2026-09-09
Status: accepted

## Context

The image adapter defaults from ADR 0074 select `gpt-image-2` and `auto`
quality. GPT Image 2.5 provides Sunburst and Flare, with `xhigh` and `max`
quality levels in addition to the existing four. tny should default to
Sunburst with explicit `high` quality while preserving per-request overrides.

## Decisions

1. Supersede ADR 0074's model and quality defaults: Codex generation and
   editing select `gpt-image-2.5-sunburst` and `high` when omitted. Keep the
   model default in the provider table and the quality default in the Codex
   adapter; conversation model selection does not affect either setting.
2. Preserve explicit image model strings, including `gpt-image-2.5-flare`.
   Accept `auto`, `low`, `medium`, `high`, `xhigh`, and `max` quality values.
   Model and quality overrides are independent; explicit `auto` reaches the
   provider unchanged. Invalid quality values fail before a request. Never
   retry with a different model or quality on provider rejection.
3. Keep standalone CLI, typed image tools, and intercepted shell commands
   on the shared service. Update help, schemas, and user documentation to
   advertise the same defaults and overrides. The release-pinned account
   transport, credentials, size/background hints, and atomic output behavior
   from ADRs 0074/0075 remain in effect.
4. Wasm uses the same validation and adapter through its existing transport
   and filesystem seams. Browser access still depends on CORS and account
   credentials; no new platform behavior or public embedding API is added.

## Verification

HTTP fixtures check default generation/editing payloads, model metadata,
both GPT Image 2.5 models with omitted quality and every explicit quality
level, independence from the global chat model, and typed/shell tool calls.
Unit tests cover valid and invalid quality values; agent fixtures inspect
the advertised schemas, and CLI fixtures inspect help without credentials.
The unchanged baseline failed the new default/override regressions on the
old model, omitted quality, and `xhigh`/`max` rejection.

These fixtures verify request construction and response handling, not live
account entitlement or output quality for either model. No live generation
or performance comparison is part of this change.

## Primary source

- [OpenAI image prompting guide](https://developers.openai.com/api/docs/guides/image-prompting), model parameters verified 2026-09-09. The public API documents both models and six quality values; tny deliberately chooses `high` rather than the API's `auto` default. The Codex account routes retain ADR 0074's transport pin.
