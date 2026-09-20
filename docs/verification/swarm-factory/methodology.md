# Effectiveness-first matched evaluation

## Objective

Maximize verified useful outcomes for the requested task, then reduce avoidable
coordination delay, invalid tool attempts, duplicate context and unnecessary
synchronization. Additional reasoning and independent review are expected to cost
tokens. Token usage is telemetry, not a failure condition or the optimization target.

## Controlled comparison

`tests/bench/bench_swarm_effectiveness.py` compares a frozen baseline tny binary
at historical `8f77e71` and its version-1 review roster against a frozen candidate
binary and the version-2
`examples/swarm/benchmark-contract-review.json` roster. Both rosters have the same
root and three named participants. Both use `gpt-5.6-sol` with medium effort,
fresh state/workspaces, the same task prompts and independent correctness oracle,
60 steps and a 360-second per-trial deadline. Order alternates across cases and
repetitions; failed, timed-out and infrastructure-error trials remain recorded.
An infrastructure failure aborts the experiment after persisting its result,
rather than starting another trial with uncertain previous-job settlement.

The candidate has the same independent specification/testing reviewers, with
explicit deliverables and evidence criteria. Its nested coordinator begins after
both reviewer results are durably available, rather than spending turns waiting
for them. Direct typed peer communication remains available; prerequisite edges
control execution readiness, not a prohibition on collective discussion. These
contracts and the binary change together, so this is not an isolated causal
estimate for a single mechanism.

The task ladder is unchanged: Unicode normalization (37 external checks), durable
concurrent ledger plus CLI (21), and deterministic retrying dependency scheduler
(121). Oracles are run outside generated workspaces, and modifying public tests
fails acceptance. Actual model identity and every declared participant's launch
are also checked. A successful root answer is insufficient if peers failed or
cleanup had to terminate unfinished work.

## Measurements and interpretation

Record external correctness and orchestration completion separately, end-to-end
elapsed time including cleanup, observed launched participants, complete available
root/child input/output/cache usage, typed message categories/topics, attempted
tools and failed tools. Report medians over all observed runs and accepted runs
separately. Do not divide only successes into a cost estimate that drops failures.
Unknown/incomplete usage is null, never zero. Input already includes cached input.
Do not interpret more messages, acknowledgments or agent exits as greater quality.

This small synthetic ladder supports diagnostic hypotheses, not universal speed,
reliability or token-efficiency claims. Provider caching is uncontrolled. No coding
agents, code builds or local quality gates should overlap timed inference. Keep
original runs when iterating; publish sanitized result records, not raw sessions,
login references or generated private workspaces.

## Beyond the review roster

Isolated writer execution, causal launch ordering, failed/missing/corrupt
predecessor handling, workspace/attempt integrity, typed peer retries, permissions
and cancellation require deterministic runtime tests. Passing a review-roster live
benchmark does not establish multi-writer integration effectiveness. Version-2
workspace options remain explicit and isolated changes never auto-merge. Future
larger repository evaluations should include competing implementation experiments,
integration conflict rates, external acceptance coverage and verified defects
caught—not merely number of agents or lines of generated code.

## Workspace matching

The baseline predates the writable-agent-default commit `ca7eb71`. To isolate the
review workflow comparison from that policy change, all three candidate peers,
including the delayed coordinator, explicitly choose `shared_read_only`. The root
is writable in both arms. These are intentional benchmark settings, not new
read-only defaults. Main's current writable baseline is not measured in this
experiment, and differences cannot be attributed to one mechanism alone.
