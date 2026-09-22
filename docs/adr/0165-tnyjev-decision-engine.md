# ADR 0165: tnyjev, an isolated typed decision engine

Date: 2026-09-22
Status: Accepted

## Context

The user requests TypeSafe AI Jev as a modular decision engine, first exposed
through `tny score` (no/yes probability) and `tny choose` (JSON-defined routing
against state). They explicitly name the module **tnyjev**. Later reasoning,
model-routing and memory/reaction uses motivate the boundary but are not yet
authorized implementation scope.

Jev is not a chat-completions model. Its documented `/v1/systemone` endpoint
accepts typed questions and returns typed answers. Its **Noul** primitive is
the requested `[0,1]` probability; its distinct **Score** primitive represents
ordinal rubric levels and must not be confused with this CLI name.

## Decision

1. Add the private C11 `core/tnyjev` module with a C/C++-compatible typed API.
   Use explicit configuration, borrowed requests, tagged values, typed status,
   and an allocation-free result. Do not add a conversational backend, SDK
   dependency, or additional C++ ownership area.
2. Implement Noul and Choice only. Validate finite/ranged probabilities,
   complete distributions, chosen-key membership, model and usage fields.
   Fail closed instead of returning guessed values or default routes.
3. Expose `score` and `choose` as standalone CLI toolkit adapters. Read
   `TYPESAFE_API_KEY` only in the adapter. Resolve no conversation credentials,
   create no agent sessions, and perform no provider I/O for help.
4. Share JSON and HTTP/TLS/wasm seams. Bound inputs, request and response to
   1 MiB. Use `tny_poll`, explicit cancellation and a response deadline.
   Keep the existing transport's separate connect/write deadlines explicit.
5. Do not retry, follow redirects, execute routes, or inject Jev into existing
   harness decisions. Keep confidence thresholds, fallback and retry policies
   with future callers. Do not log credentials or untrusted response bodies.
6. Support remote wasm use via fetch/Asyncify; run the mock CLI suite in wasm
   CI. Browser CORS remains the endpoint's responsibility, not an assumed
   live-provider guarantee.

## Consequences

The module can be called by later harness components without parsing CLI
output or loading chat/session state. Choice descriptions and state retain
structured JSON. The initial synchronous entry point is suitable for these
one-shot commands; future event-loop integration may need a stepped API and
must preserve bounded cancellation/ownership. No stable libtny SDK exports
are added now. No claim about latency, model quality or live entitlement is
made from local mock tests.

The shared source wildcard includes the module on native/wasm builds. Tests
cover typed boundaries, HTTP failures, split bodies, limits and interruption;
mutation targets check decision validation. Documentation and exact CLI
contracts live in [tnyjev](../tnyjev.md), including primary API references.
