# 0171 — Start formal verification with SMT-LIB 2 and Z3

Date: 2026-09-24
Status: accepted

## Context

An old `claude-agent-acp` advertised "Opus (1M context)" (described as Opus 5)
even when Claude Code showed Opus 5.5. tny was faithfully printing the adapter's
session catalog. We want
an executable statement of the trust boundary, not a hard-coded list of model
releases. The project needs a formalism reusable for catalog selection,
permissions, session lifecycle and retry invariants across C, C++ and wasm.

## Decision

Use standard SMT-LIB 2 as the specification language and Z3 as a test-only
solver. `make verify-formal` fails on `sat`, `unknown`, solver errors or missing
Z3; `make test` and the Nix test derivation include it. Proof obligations live
in `tests/formal/*.smt2`, each with an explicit negated invariant (`unsat`
means proved). An unbounded integer-indexed array models the parsed catalog;
there is no model-count cutoff. The first proof establishes that choosing the
ACP select config (or the legacy catalog when absent) cannot introduce or
rename an entry, and that a nonadvertised ID cannot become selectable.

The proof is an **abstract protocol contract**, not a proof of the C parser,
JSON escaping, the external adapter or account eligibility. The corresponding
fixture integration test runs the actual `tny models --json` binary twice with
stale and updated catalogs and checks that `opus[1m]` keeps its ID while its
advertised display name changes. Future formal obligations should state their
assumptions, use counterexample queries and pair with concrete integration
checks at the abstraction boundary; use source-level bounded verification
separately where memory safety of a C implementation is the question.

No provider connection or real account is needed. Updating the adapter is
still required to learn new Claude models; formal checks cannot manufacture
knowledge absent from its response.
