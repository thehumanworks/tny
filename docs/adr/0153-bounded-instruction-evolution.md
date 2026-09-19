# ADR 0153: Bounded, evidence-gated instruction evolution

- Status: accepted
- Date: 2026-09-19

## Context and research

The harness should learn from verified outcomes without treating model prose as
proof or granting a candidate control over its judge. Recent work gives useful,
but distinct, precedents:

| Primary source | Mechanism we borrow | What we do not claim |
| --- | --- | --- |
| [SoL-Pi](https://github.com/NVlabs/SoL-Pi/tree/bd005888b9b8a3fcdb511feb91fc27d3dfa8f2b1), [paper v1](https://arxiv.org/html/2609.20519v1) | Explicit opt-in; constrained harness experiments; preserve source evidence; separate search from final evaluation | Its four shipped efficiency mechanisms are not themselves an unrestricted self-rewriting controller. Its reported savings are not tny measurements. |
| [GEPA, v1](https://arxiv.org/html/2507.19457v1) | Revise instructions using execution feedback; evaluate outside the proposer; separate train, validation and test | Our single-incumbent Pareto gate is not GEPA's population/per-example frontier or full algorithm. |
| [Darwin Gödel Machine, v1](https://arxiv.org/html/2505.22954v1) | Descendants become parents; keep lineage and rejected experiments | No code-level self-modification, model-weight training, open-ended search or proof of general improvement. |
| [ACE, v1](https://arxiv.org/html/2510.04618v1) | Durable, revisable context learned from outcomes rather than one-off reflection | No automatic global memory updates or unreviewed accumulation of instructions. |

These are dated primary-source snapshots, not a systematic literature review.
SoL-Pi's v1 full-stack result reports lower cost with about 94% score retention;
that is a tradeoff, not proof of zero capability loss. Genuine quotes establish
provenance, not completeness. Fixed test cases establish finite-sample behavior,
not correctness on all future work.

## Decision

Add an optional **task-instruction evolution workflow** and a `self-improve`
built-in task preset. Use the existing native CLI and detached session machinery
for model proposals. An external standard-library Python controller owns the
experiment; it does not implement another agent loop, provider transport, tool
runner, or scheduler. Like the existing shell workflows, it runs only when
explicitly invoked. Ship its two modules beside the optional extension host.

The editable artifact is a plain task body. The operator fixes the evaluator,
cases, cost unit, budgets, provider choice and promotion target. Training
feedback informs proposals. Every candidate is evaluated independently on
training and validation cases. Accept only a strict training improvement with
no pass-to-fail regressions or aggregate cost increase in either split. Rejected
candidates leave the parent unchanged. Accepted children become subsequent
parents. Keep the baseline, commands, outputs, failures and lineage in a private
run directory. After search, compare baseline and winner once on held-out tests.
Do not send holdout results back into the same search.

Activation is a separate, explicit compare-and-replace operation against the
unchanged baseline file. A completed experiment is not permission to install a
preset. Existing task snapshots and resolution precedence do not change.

See [the workflow contract](../instruction-improvement.md) for the CLI, protocol,
trust boundary, rollback and platform support.

## Verification and claim boundary

- Fault tests cover invalid evidence, evaluator errors/timeouts, regression,
  cost inflation, ties, held-out isolation and unsafe/stale promotion.
- An offline benchmark performs real local file reads through a deterministic
  instruction interpreter, with an independent byte-exact answer check. It
  demonstrates recursive selection and cost accounting, **not LLM learning**.
- Keep search cost separate from final operating cost. Compare a frozen winner
  to the original baseline on identical held-out cases. Publish per-case data,
  source hashes and reproducible commands.
- Live-model gains need explicitly authorized, paired, repeated live trials,
  fixed provider/model/effort, independent checks and uncertainty estimates.
  They cannot be inferred from the offline replay benchmark.

## Consequences

The C11 runtime, public ABI, startup, permission defaults and model weights stay
unchanged. The preset can be inspected on every CLI build. The workflow needs
native Python and subprocesses and is unavailable in wasm. It does not support
SSH workspaces or remote promotion. It is not a security sandbox: the trusted
operator controls commands, files and run storage. Hashes detect accidental
changes, not a malicious party that can rewrite both evidence and hashes.

A single incumbent and finite budget are easier to audit than an evolving
population. They can miss useful stepping stones. Repeated validation selection
can overfit; final held-out evaluation reports this risk but is not a universal
safety certificate. A future live study can justify broader search mechanisms;
this change does not pre-authorize them.
