# ADR 0203: Explicit current-route benchmark profiles

Status: accepted. Date: 2026-10-19.
Renumbered from ADR 0192 (serial collision with the budgeted-recoverable-working-context ADR) on 2026-10-19.

## Decision

Extend the existing recording benchmark rather than introduce another evaluation
framework. Default to the production Sign in with ChatGPT public Responses route;
retain legacy Codex only by explicit selection. Fixed public Responses and Chat
Completions profiles use their own environment-key sources. The proxy never
creates or refreshes credentials, follows redirects, or changes destinations in
response to a credential or an error. Synthetic harness credentials are distinct
from the real credentials read only by the proxy.

Pair existing task fixtures by content hash and task/repetition on one route,
model and effort. Keep provider usage unknown when absent and separate task
success from measurement completeness. Keep failed task costs and timings in
the existing per-success comparison. Do not pool routes in reports/comparisons.
Do not infer quality or cache savings from mock streams or character counts.

## Verification and limits

The offline route tests join the existing benchmark-policy Make prerequisite;
they use standard-library fixtures and no provider credentials. They exercise
wrong wire/model rejection before auth, expired/wrong SIWC grants, redirects,
credential isolation, error redaction, split tool names and missing usage.
Native tny adapter smoke checks can use a local synthetic upstream; paid/live
runs remain separately authorized. No new model-price claims are introduced.
Current-route competitors are rejected until an adapter is verified; the
legacy adapters remain explicit compatibility, not a current-route comparison.

See [benchmark instructions](../../tests/bench/harness_bench/README.md) for the
paired-run manifest, evidence limits and primary provider documentation.
