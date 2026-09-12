# 0090 — Canonical foreground ask events through the existing engine

Date: 2026-09-11
Status: proposed; implementation and verification pending
Requirement: R124.8 / I124.8; canonical contract ../verification/open-issues-2026-09-11/contract.md, A7.

## Decision

Add foreground --events=jsonl via the existing in-process ask engine path, not a second agent loop or a reinterpretation of the runner's private wire events. Serialize the engine-owned canonical envelope verbatim and use frozen public ABI numeric values/public-reader payload semantics. Keep legacy isolated/Markdown/final-json behavior intact; explicitly reject competing stdout modes and detached background event streams. Keep ephemeral foreground turns with present empty envelope values where the engine supplies them. --progress=none suppresses successful human diagnostics without hiding failures.

JSONL writes are an explicit checked boundary. A slow reader creates bounded backpressure without event loss; cancellation and closed-pipe failure must clean owned work and return non-success, not fall through the default DONE stop value. A missing actual terminal event is an error, never an invented terminal. Machine startup failure has stable stderr JSON and no fake accepted-turn event. The stdout delivery outcome must not destroy real provider output or claim successful delivery after I/O failure.

## Alternatives and consequences

The runner's private replay/durability protocol lacks the exact public envelope, so extending it would add unnecessary cross-process protocol scope. A second API-backed agent loop would duplicate tools/permissions/context. The existing in-process engine already owns event ordering, envelope, cancellation and bounded event queues. The additive foreground mode selects that path deliberately. A simple unchecked fwrite loop would be shorter but fails broken-pipe/cancellation guarantees. Public ABI records and canonical schema are not revised to fit an easier implementation; conformance tests detect any duplicated mapping drift.

## Validation

Fresh independent design review 6407aa95-b471-4b79-9adb-380b709dcb9d approved the bounded direction with explicit write-failure, missing-terminal and ephemeral-envelope corrections now fixed in A7 before code. Required exact fixture checks include public-reader parity, full tool/permission/usage/error/cancel events, startup/legacy/ephemeral behavior, slow/closed stdout, bounded memory, actual owned cleanup and absence of fabricated terminal success. Critical mutations target mapping, terminal accounting and I/O/cancel honesty. Actual code needs a new independent review before jobs reuse. No job implementation or runtime proof is established by this proposed decision.
