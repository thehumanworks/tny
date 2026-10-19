# ADR 0189: agent-first modernization through existing seams

Status: accepted, 2026-10-19.

## Context

The goal is a simpler, extensible, persistent and efficient harness, not a new
framework or an unmeasured claim to be the fastest. The existing implementation
already has native Sign in with ChatGPT, typed Python extension hooks, detached
session runners, host-authorized CPython cells, and bounded workflow learning.
Replacing these wholesale would increase migration risk and maintenance cost.

## Decisions

1. Keep the native C11/scoped private C++20 core and public C ABI. Extract the
   provider's pure error policy from its large transport module, with explicit
   inputs and independent tests. Add an outcome-oriented development map rather
   than moving every source file and breaking navigation history.
2. Keep Python as the agent-friendly plugin language. Extract one shared loader
   for the runtime and explicit-path authoring checker, derive the plugin event
   catalog from runtime types, and catch invalid callbacks before a model turn.
   Catalog inspection executes no user code; checking a plugin explicitly
   executes trusted import/setup code and is not a sandbox or a static proof.
3. Add bounded progressive `tools.search` discovery to existing code mode.
   Preserve `list`, `describe` and `call`; search is a projection of the already
   filtered catalog, not another tool registry or authority channel. Short
   summaries avoid printing every parameter schema just to find one tool.
4. Harden cancellation at worker launch/publication boundaries and retain
   completed worker results. A cancel acknowledgment must reflect observed worker
   cleanup, not just a recorded intent. No promise to roll back prior effects.
5. Recover learner persistence despite abandoned temporary files and sync
   directory entries where supported. Rename is the publication point: directory
   sync failure must not replay an already-published counter delta. Learning is
   best-effort advice, not a transactional exactly-once effects engine.
6. Prefer the supported public OpenAI SIWC route for new implicit provider
   selection. Never reinterpret or transfer legacy Codex credentials. Preserve
   explicit legacy selection and saved context until a user deliberately moves.
   Retain the verified Claude ACP adapter, which already delegates to the official
   SDK/Claude Code; a second direct SDK bridge is not a demonstrated net benefit.

## Persistence and improvement boundaries

The existing detached runner remains the session writer and reconnect endpoint.
The checkpoint, durable-job and runner ownership modules are retained; this is
not a storage migration. Search adds no provider request or process and does not
persist transient catalogs. Python plugin validation is on demand, never added
to ordinary startup.

The automatic learner still stores only bounded counters and source session IDs
with fixed advice templates (at most 1 KiB), not arbitrary model-written policy.
Promotion requires observed successful recovery and can be reversed by failures.
The optional broader instruction experiment keeps independent evaluation,
held-out cases and explicit promotion. No weight training, silent code rewrite,
credential change or permission expansion is introduced.

## Verification and limits

Use compact executable invariants at the owning seam, then real process/wire
fixtures. Measure full-catalog versus progressive-discovery output against the
same selected schema and unchanged baseline. Bytes are not model tokens; fewer
discovery bytes are not evidence of better task quality or lower inference
latency. Preserve help/startup latency with paired same-host measurements.

[Evidence and remaining limits](../verification/agent-first-modernization.md)
records exact commands and outcomes. Platform, leak, live-account and paid-model
checks not run remain explicitly unverified. No competitor-best claim is made.
