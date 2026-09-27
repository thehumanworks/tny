# 0178 — Retain Lua after the Codex code-mode language benchmark

Date: 2026-09-27
Status: accepted

## Context

Compare the current embedded Lua code mode with QuickJS JavaScript and CPython,
using `gpt-6-luna` through the owner's normal Codex ChatGPT login. Do not infer
model familiarity or interpreter footprint from language popularity. The request
also requires Lean verification of changed deterministic decision rules.

## Decision

**Retain the current production Lua 5.4.9 implementation.** No provider prompt,
execution authority, wire protocol, public ABI, shipped dependency, runtime budget,
or wasm behavior changes. QuickJS and CPython remain experiment-only probes.

The preregistered experiment used twelve capability-composition task families,
three independent generations per language/family, and three private fixture
variants per generated program. At low reasoning effort, Lua passed 35/36 on
first attempt, JavaScript and Python 36/36. All reached 36/36 after allowing one
repair; only Lua needed one. Across all attempts, output tokens per solved task
were 172.81 for Lua, 139.58 for JavaScript, and 126.25 for Python.

JavaScript's observed output-token reduction was **19.23%**, with a task-clustered
95% bootstrap interval of **11.88%–25.11%**. This is evidence of reduced output,
not evidence of no benefit. It does not clear the *predeclared* requirement that
the interval support at least a 15% reduction. The first-pass difference interval
includes zero. This conclusion holds even assuming full production compatibility.

Python's reduction was **26.94%**, interval **18.70%–33.96%**: it **does** clear
the model-efficiency threshold. We nevertheless reject the measured CPython
embedding for this product: its stripped shared interpreter alone is 31,867,976
bytes, separate from its 117,568-byte host and stdlib. Its empty-cell initialization
and teardown median was 23.66 ms versus Lua's 0.114 ms. Saving an average 46.56
output tokens per solved cell, with no observed eventual-success improvement,
does not justify that dependency/initialization cost and additional containment
work in Tny's small embedded composition layer. This is a product trade-off,
not a claim that Python is less familiar to the model or intrinsically incapable
of safe embedding. A minimized/frozen/static CPython distribution was not built;
the measured distribution is not a universal Python size lower bound.

Under the same host and `-Os`/LTO/stripping settings, the standalone Lua probe
was **277,248 bytes**, QuickJS **748,088 bytes**, and the empty host **96,568 bytes**.
QuickJS was 2.70 times the Lua probe size, not smaller. The incremental difference
is 470,840 bytes, not an estimate from marketing figures. These are isolated
embedding probes, not full migrated Tny release artifacts or a new fixed size cap.

Separate compatibility probes found that native QuickJS JSON parsing/stringifying
changes signed integer `9007199254740993`, whereas the current Lua bridge preserves
it. The CPython probe admitted a 20 MiB cell allocation that Lua's 16 MiB allocator
rejects. Neither prototype is a verified drop-in replacement. These observations
were not counted as extra model failures, and do not establish that a completed
alternative implementation could never address the gaps.

## Implementation and verification

Add an opt-in reproducible benchmark, synthetic raw evidence, independent effect
and usage reconciliation, a machine-checked decision policy and a dedicated CI
job. Normal tests never call a provider or download QuickJS. The benchmark's
Lua arm links the actual `src/core/code_runtime.c`; generated programs run with
no network, no host workspace/HOME, no inherited credentials, and a mandatory
Linux bubblewrap boundary. No user's actual file is a simulated capability target.

Lean checks definitions generated from the actual Python policy AST: acceptance
requires execution, output and trace success; promotion requires a complete
cohort, a confirmed gain, verified parity, and no observed first/final success
regression. Mutation tests must fail when those gates are weakened. The proof
does not certify the stochastic experiment, the entire Python/C implementation,
the interpreters, or OS containment. Existing production tests and the existing
source-linked SMT check retain their separate scopes.

See the [report](../verification/code-mode-language/report.md),
[preregistered protocol and execution notes](../verification/code-mode-language/protocol.md),
and [evidence ledger](../verification/code-mode-language/evidence.md).

## Consequences and limits

The result is deliberately narrow: short synchronous typed-capability programs,
one model, one effort level, one Linux host and a small handcrafted task set.
The Codex CLI generates cells for all arms; this is not a full Tny agent-loop,
large-repository, multi-turn, hosted billing or cold-machine benchmark. Token cache
behavior differs across arms and is reported rather than converted into invented
subscription pricing. Twelve task clusters do not establish broad language
superiority or equivalence. Reconsideration should use new held-out task families,
not repeat selection or tuning against this published corpus.
