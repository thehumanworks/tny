# Tny versus Codex: progressive workload evaluation

Preregistered 2026-09-28 before scored inference. Requested model `gpt-6-luna`,
reasoning effort `low`, normal Codex-account subscription endpoint for both arms.
No model substitution. This evaluates whole harnesses, not isolated code cells.

## Frozen scope

Ten existing synthetic repository tasks, two independent attempts per task and
harness: 40 scored runs. Fresh Git workspaces/HOMEs per run, default harness
instructions/tools, host-authorized execution for both, no user skills/MCP/hooks.
A common task suffix forbids modifying original tests, reading parent/benchmark
files, internet research and delegation. The evaluator is outside the workspace
and runs only after the agent exits. This is not an OS-enforced blind benchmark;
these are published regression tasks, not newly held-out research problems.

Difficulty tiers are qualitative, selected before outcomes:
1. Localized C build repair and arena memory-lifetime repair.
2. A filtered CLI feature and a multi-table SQL aggregation fix.
3. Source-grounded Q&A and a 320,000-record trace analysis.
4. Recursive JSON-diff feature and workflow dependency semantics.
5. A repository-wide multi-bug triage and a multi-client API migration.

Time budgets: 300 s tiers 1–2, 600 s tiers 3–4, 1200 s tier 5. The same budget
applies to both arms. Each paired block starts both arms (concurrency two), with
submission order alternating by repetition/task. Blocks proceed from easier to
harder tasks. The two repetitions are retained, not selected or repaired after
oracle feedback. A wiring-only smoke is separate and never in headline scores.

## Measurement

A loopback recording proxy forwards each harness's normal Responses/SSE request
unchanged to the same Codex endpoint. Only the proxy reads the existing OAuth
store; real credentials/headers never enter the child HOME or retained evidence.
The adapters disable Codex hosted web search and websocket transport to make
request-level measurements comparable. This is a controlled SSE comparison, not
a claim to benchmark every default Codex transport or user's personalized setup.

Provider completion usage is authoritative: input, cached input, cache writes
(when reported), output and reasoning tokens. Record unknown fields as unknown;
no estimated usage replaces an absent provider completion. Check every request's
model and effort. Hash/cache metadata and canonical request sections support
prefix-stability diagnosis. Preserve all failures/timeouts in quality and total
resource denominators; measurement/infrastructure failures are explicitly separate.

Model-facing tool invocations and model request counts measure round trips, not
OS syscalls: one Tny Python call and one Codex shell call may each contain many
operations. Report tool-output bytes and actual command/result traces separately.
Do not assume a single Python call equals a single shell command.

Quality: frozen task oracle, pristine visible-test integrity, and a transparent
final-answer rubric (task-relevant summary, concrete validation evidence, accurate
completion claim, limitations when applicable). The rubric is supplementary, not
an LLM judge or a replacement for functional checks. Report failures individually.

Caching: token-weighted cached/input fractions, first model request versus later
requests, plus two dedicated scenarios on a shared synthetic catalog: resumed
conversation and fresh sessions in the same workspace. Two repetitions per
scenario/harness, five user turns each. First requests are not called guaranteed
cold; server caches cannot be manually cleared. No cache settings are forced in
the headline arms. Cache-write fields and stable/changed prefixes are separate.

No invented subscription dollar/quota conversion. Report raw token components and
input-minus-cached explicitly. Exploratory paired task-cluster bootstrap intervals
retain repetitions within tasks; ten tasks are too few to establish universal
harness superiority. Recommendations must distinguish observed causes from ideas
requiring a follow-up A/B experiment.

## Evidence and cleanup

Keep frozen binaries' SHA256, CLI versions, source hashes, task fixtures, every
run/turn/request receipt, final answers, patches, oracle logs, analysis and this
protocol. Do not alter production Tny or Codex. Evaluation code/evidence goes on
an evaluation-only branch. Remove only this evaluation's registered worktree and
its marker-identified temporary root after exporting evidence. Existing unrelated
worktrees, user files, caches, credentials and sessions remain untouched.
