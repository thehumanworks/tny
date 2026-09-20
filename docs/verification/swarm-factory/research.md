# Research inputs: software factories and effective multi-agent systems

Reviewed 2026-09-20. Sources are primary research or authors' implementation reports.
Reported benchmark results are not measurements of tny and are not assumed to transfer.

| Source | Evidence and limitation | Engineering consequence for tny |
|---|---|---|
| StrongDM Software Factory, https://factory.strongdm.ai/ | Practitioner account: specifications plus external scenarios, behavioral service twins, and outcome-oriented validation rather than self-reported completion. Not a controlled comparison of arbitrary coding harnesses. | Keep acceptance scenarios outside generated workspaces; distinguish task termination, peer evidence and independently accepted output. |
| StrongDM Attractor spec, https://github.com/strongdm/attractor/blob/main/attractor-spec.md | Executable graph specification with outcome routing, bounded retries and goal gates. A design specification, not measured proof that this exact design wins. | Reuse the existing durable DAG instead of adding a broker or recursive scheduler. Make dependency and output expectations executable or explicitly advisory. |
| Cursor, Scaling long-running autonomous coding (2026-01-14), https://cursor.com/blog/scaling-agents | Reports flat shared-file coordination contention and unclear ownership; role separation improved long-running work. Its worker-only/no-peer topology conflicts with this user's collective-participation objective. | Adopt clear ownership and bounded responsibility, not the removal of peer communication. Avoid a centralized integration bottleneck and ambiguous shared writes. |
| Cursor/NVIDIA, multi-agent kernels (2026-04-14), https://cursor.com/blog/multi-agent-kernels | Reports 235 kernel problems and measured optimization against correctness/performance evaluators, with a concise protocol specifying outputs and tests. Domain-specific author report, not general coding superiority. | Optimize quality-adjusted outcomes and useful experiments; additional compute can be valuable. Include clear contribution contracts and independent task oracles. |
| Google Research/MIT, Towards a Science of Scaling Agent Systems, https://arxiv.org/html/2512.08296v2 and https://research.google/blog/towards-a-science-of-scaling-agent-systems-when-and-why-agent-systems-work/ (2026-01-28) | Controlled 180-configuration study; benefits depend on task structure, tools, coordination and budget. Benchmarks are not production repository changes. | Preserve useful independent investigations; reduce duplicate work and waiting, not agent count or tokens in isolation. Measure per-task outcomes and actual participation. |
| Cemri et al., Why Do Multi-Agent LLM Systems Fail?, https://arxiv.org/html/2503.13657v3 | MAST taxonomy from 1,642 traces, 14 failure modes in specification, inter-agent alignment and verification/termination. Taxonomy is not exhaustive; historical models/frameworks. | Explicit scope, deliverables, causal dependencies, stable context and bounded termination; test missing, corrupt and failed handoffs. |
| Anthropic, Effective harnesses for long-running agents, https://www.anthropic.com/engineering/effective-harnesses-for-long-running-agents | Practitioner experiments emphasize persistent feature state, incremental work, commits and end-to-end tests. | Durable contracts and immutable handoff inputs; no success from model prose alone. |
| OpenAI, Harness engineering (2026-02-11), https://openai.com/index/harness-engineering/ | In-repository knowledge, per-worktree tools, observable outcomes and mechanically enforced boundaries. Internal case study, not a causal benchmark. | Reuse tny worktree authority and record-backed state; make capabilities legible and keep tests outside agent self-assessment. |
| Lin et al., Agentic Harness Engineering (2026-04-28), https://arxiv.org/html/2604.25850v1 | File-level observable changes with falsifiable predictions; ablations place gains in tools/middleware/memory rather than prompt text alone. Preprint; benchmark/generalization claims belong to that study. | Pair each change with a prediction, deterministic regression and comparative evaluation. Do not merely enlarge the system prompt. |

## Baseline findings and falsifiable hypotheses

Baseline: `8f77e71`, existing draft PR #173. Version-1 membership has names/purposes
but every worker is forced into a review-only role, the manifest cannot express
causal dependencies or an expected deliverable, and generic mailbox send requires
run/task/id bookkeeping. The last evaluation retained four recoverable mailbox
errors. Existing jobs already provide dependency readiness, task workspaces,
attempt fences and immutable results: extend these, do not duplicate them.

H1: Versioned contribution contracts and native dependency readiness reduce
premature synthesis/irrelevant reviews; explicit isolated workspaces permit useful
implementation experiments without shared write races. Validate launch ordering,
failed predecessor blocking, immutable recovery and exact policy inheritance.
H2: A typed named-recipient message adapter removes accidental transport bookkeeping
and produces organized evidence while retaining explicit acknowledgment, replay and
attempt-scoped idempotency. Validate name resolution, authority, retry identity and
no implicit permission widening.
H3: Compare externally verified task outcomes, elapsed time, errors, peer participation,
context/cache telemetry and total usage. Raw tokens are a cost measurement, never
an automatic failure or the optimization objective. Preserve unfavorable trials.
