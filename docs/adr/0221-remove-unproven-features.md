# 0221 — Remove features without evidence of value

Date: 2026-11-12
Status: accepted

## Context

The owner's goal is a small, old-school coding agent judged on feature set and
simplicity. A read-only audit on `eebc162` ranked code by cost (complexity,
fragility, binary size) against evidence of value found in the repository:
callers, user paths, tests against real models, and the project's own
verification reports. The owner then delegated the calls to the maintainer.

## Decision

Remove each feature below. Each removal is one commit, so it can be reverted on
its own. A removed feature can return through a new ADR that brings evidence.

### Jev decisions (`tny score`, `tny choose`)

Supersedes [ADR 0165](0165-tnyjev-decision-engine.md). The client needed a
separate paid TypeSafe key, had no caller inside tny, and was only tested with
loopback fixtures. Removed: `src/core/tnyjev*`, `src/cli/cmd_jev.c`, their tests
and docs (about 1.5k lines including tests). Open issues #180 to #187, which
would route reasoning and models through Jev, are out of scope.
