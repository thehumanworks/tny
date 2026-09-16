# Evidence: C++ phase 1

Contract: [contract.md](contract.md)
State: INCOMPLETE. No implementation or migration check has passed yet.
Baseline: 1d8ad71d66c06c726b3c5b35e367fec678031e85; clean, recorded before contract creation.
Active invariants: P1-I1 through P1-I6 and C0-C6. Owner: root pending bounded delegation.
Initial contract was written before production changes. Initial bytes retained in contract.initial.md; digest and ADR manifest recorded in series artifacts before first implementation.
Native goal: unavailable by authorization (ordinary task, no explicit goal request); get_goal returned null. Higher-priority tool rule applied.

## Execution records

Pending.

## Reviews

Pending.

## Mutation results

All planned mutations unrun.

## Final reconciliation

Every invariant pending. No scope reduction authorized.


## Integrated implementation checkpoint — 2026-09-16

- Parser implementation `c47e568b40eb724868f3e452a16bd2d651a46683`
  integrated as `1af4c7b`; build implementation
  `af5663b5932c325797ebca765e42294d1b7f80e7` integrated next.
- Parser slice final focused checks: 56 net/OpenAI/Cursor tests, instrumented
  fuzz corpus, deterministic ownership fault sweeps, four behaviorally killed
  mutants and zero leaks. Those precede final integrated acceptance.
- Unchanged baseline `make quality` passed (exit 0). Its sole clean full-suite
  failure was reproduced below the parser with a real TCP client: the abort
  mock could remain open/silent. Explicit transport shutdown fixes the fixture
  without weakening assertions or altering production transport code. The
  unchanged baseline executable then passed the complete OpenAI integration
  suite with that fixture (50.2 seconds, exit 0). A six-socket regression test
  independently exercises abort and unterminated framing.
- ADR 0113 documents actual ownership/borrowed-view/failure decisions. Wire
  corpus bytes are Git binary inputs, not source whitespace exemptions.
- Integrated full safety, ABI/SDK, quality, platform and performance gates
  remain pending at this checkpoint. No independent review yet invoked.
