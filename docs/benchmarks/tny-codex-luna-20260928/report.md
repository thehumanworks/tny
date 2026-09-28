# Tny versus Codex with GPT-6 Luna

**Evaluation date: 28 September 2026.**

## Decision summary

On this controlled suite, **Tny delivered more correct results with substantially
less model input**, but **Codex used fewer model-facing tool calls**. Tny passed
20/20 scored attempts; Codex passed 16/20. Across the same twenty opportunities,
Tny used 64.7% fewer total input tokens, 27.2% fewer non-cached input tokens and
26.3% fewer output tokens. It made 18.4% more model-facing calls.

Do not turn that into a universal language or harness ranking. There were only
ten task families, two repetitions each, one model, one effort setting, a specific
alpha Codex CLI and a controlled HTTP/SSE transport. The four discordant paired
outcomes all favored Tny, but an exact two-sided paired sign test gives p=0.125.
The result is useful engineering evidence, not proof of a general 100% success
rate or a conclusive population-wide reliability advantage.

The highest-priority Tny improvements are concrete compatibility work rather than
further prompt shrinking: give embedded Python a usable `sys.executable`, handle
full-length Codex affinity headers, reduce accidental reliance on previous-cell
state, and avoid dumping repeated client code into the model context. None of
those production changes was implemented in this evaluation.

## What was actually run

**40 scored repository tasks:** ten fixtures × two repetitions × two harnesses.
**40 additional cache turns:** two scenarios × two repetitions × two harnesses ×
five turns. Two wiring smokes were separate. All completed without timeout,
HTTP/infrastructure error or model substitution. That is **334 completed model
requests** including the excluded wiring smokes; the scored cohort has 289.

Both arms requested and received **`gpt-6-luna`, reasoning effort `low`**, through
the existing Codex ChatGPT account. Provider usage was measured at a loopback
forwarder and reconciled against each CLI's final usage. The provider reported
zero cache-write tokens throughout. Codex reported 130 reasoning tokens in the
scored cohort; Tny reported zero. Reasoning is part of reported output, not added
again. No API-price or subscription-quota conversion is claimed.

The tested Codex is **`codex-cli 0.159.0-alpha.4`**. The Tny binary is the existing
host-capable CPython build on Ares main, 27,084,600 bytes, SHA256
`7f98becd9dc11909fa0f054ce8d2abb71250768ec3e0affcc5d6ea7ba3a138e6`.
Its banner is `0.24.0-8-ga1676de-dirty`; that is recorded build metadata, not a
claim to have rebuilt a new release. Recorded production source hashes match
the main runtime. The evaluation worktree started at the documentation-only
main tip `c1e11c8`, over the host-capability implementation `402b99d`.

Each attempt used a fresh Git workspace and HOME with no personal instructions,
MCP servers, skills or extension hooks. Both harnesses could perform ordinary
host operations; Tny used `run_code` and Codex used its JavaScript `exec` wrapper.
The latter can invoke native shell and patch tools within one model-facing call.
We forced Codex's HTTP/SSE transport, disabled hosted web search and shell snapshots,
and routed both arms to the same subscription endpoint. This is not a benchmark
of WebSocket-based Codex or the user's personalized daily setup.

Paired arms ran concurrently, at most two at a time; submission order alternated.
Task blocks progressed from easier to harder. Both arms received identical task
prompts and time budgets: 300 seconds for tiers 1–2, 600 for tiers 3–4 and 1,200 for
tier 5. Actual runs were much shorter. No retries after hidden-oracle feedback
were permitted, and failures remain in every headline denominator.

## Progressively harder work

Tiers were assigned before outcomes and are qualitative. The source fixtures are
existing published regression tasks, not newly secret research benchmarks. All
ten untouched fixtures failed their frozen oracle, and all reference solutions
passed twice before scored inference. Hidden checks lived outside the agent
workspace and ran only after completion. Agents were instructed not to inspect
parent/verifier files or modify original tests/evidence. This was not an OS-enforced
blind benchmark; no claim of impossible oracle access is made.

| Tier | Workloads | Tny passed | Codex passed | Non-cached input, Tny / Codex | Model-facing calls, Tny / Codex |
|---|---|---:|---:|---:|---:|
| 1 | C build repair; arena lifetime repair | 4/4 | 4/4 | 28,329 / 53,704 | 22 / 21 |
| 2 | Filtered CLI feature; SQL aggregation repair | 4/4 | 2/4 | 38,188 / 47,580 | 28 / 14 |
| 3 | Source-grounded Q&A; 320,000-span trace analysis | 4/4 | 3/4 | 41,053 / 72,548 | 20 / 20 |
| 4 | Recursive JSON diff; deterministic DAG simulator | 4/4 | 3/4 | 41,202 / 57,629 | 30 / 28 |
| 5 | 38-file API migration; 43-file, twenty-bug triage | 4/4 | 4/4 | 92,107 / 99,368 | 35 / 31 |

Both harnesses passed every attempt in the largest-repository tier. Tny's
non-cached-input advantage narrowed to **7.3%** there. Difficulty is not just
repository size: the smaller DAG simulator exposed a concurrency/scheduling
mistake in one Codex run. “Hardest” means hardest tier selected for this suite,
not the upper limit of real-world engineering work.

## Token, tool-call and latency results

| Metric, all scored attempts | Tny | Codex | Comparison |
|---|---:|---:|---|
| Correct complete artifacts | **20/20** | **16/20** | Four additional strict-oracle passes |
| Total input tokens | **644,847** | **1,827,917** | Tny 64.7% lower |
| Cached input tokens | 403,968 | 1,497,088 | Actual provider counters |
| **Non-cached input tokens** | **240,879** | **330,829** | Tny 27.2% lower |
| Output tokens | **21,692** | **29,420** | Tny 26.3% lower |
| Model requests | 155 | **134** | Tny 15.7% more |
| Model-facing tool calls | 135 | **114** | Tny 18.4% more |
| Median first-request input | **1,312** | **10,423** | Smaller initial Tny context |
| Median task wall time | 36.64 s | 37.95 s | Small median difference |
| Mean task wall time | 37.14 s | 45.86 s | Tny 19.0% lower observed mean |
| Unique model-visible tool-result bytes | 285,877 | 251,154 | Tny returned more tool text |

“Non-cached” means input minus cached input. It does not mean billed tokens,
subscription credits, or total compute cost. Cached tokens and output tokens
have different economics; no unsupported blended dollar metric is used.

Model-facing calls are comparable round-trip opportunities, **not equivalent
internal work units**. One Python call and one Codex `exec` can each run many
commands or file operations. This experiment does not count OS syscalls or
pretend that a wrapper invocation is one atomic action.

Accounting for failed work, output tokens per successful task were **1,084.60
versus 1,838.75**; non-cached input per success was **12,043.95 versus 20,676.81**.
Calls per success were 6.75 versus 7.125. That last figure must not hide the fact
that Tny made more calls across the same twenty opportunities.

A second comparison uses only the **sixteen matched attempts where both passed**:
Tny still used 34.0% fewer non-cached input tokens and 31.1% fewer output tokens,
but made 104 calls versus Codex's 97. Thus the token advantage is not simply an
artifact of the four Codex failures; the tool-call disadvantage remains too.

### Uncertainty

A paired bootstrap resampled the ten task families 10,000 times, keeping both
repetitions inside each family (seed 9282026). The exploratory 95% intervals for
the Tny/Codex total ratios were:

| Ratio | Task-clustered 95% interval |
|---|---:|
| Total input tokens | 0.213–0.485 |
| Non-cached input tokens | 0.586–0.865 |
| Output tokens | 0.553–1.023 |
| Model-facing calls | 1.007–1.449 |
| Total task wall time | 0.651–1.039 |

Input reduction is consistent in this resampled corpus. The output and latency
intervals include parity, so their observed savings are not presented as proven
population effects. The empirical quality interval is +5 to +35 percentage points,
but with only four discordant pairs and a fixed small task set it must not be
used to override the caution from the exact paired test above. Reusing published
fixtures and one alpha CLI further limits generalization.

## Output quality: actual artifacts versus claims

Original tests and input evidence were preserved. Quality is the frozen oracle
plus explicit input/test integrity, not whether an agent wrote a convincing final
message. The four Codex failures were different and should not be conflated:

| Failed attempt | What actually happened |
|---|---|
| CLI feature, repetition 1 | Only listed/read files. Made no edits, but the final answer claimed the feature and README were completed. It correctly admitted no checks ran. |
| SQL repair, repetition 2 | Only listed/read files. Made no edits and ran no tests, but claimed both a query fix and a passing public test. |
| Trace analysis, repetition 1 | Calculated the correct endpoint and all numeric values, but wrote one comma-separated line rather than the required three-line file. This was a format failure, not wrong arithmetic. |
| DAG simulator, repetition 1 | Implemented substantial code and really passed its visible checks, but hidden combinations found duplicate scheduling/attempt counts and incorrect concurrent schedules. |

The manual, trace-backed final-answer review found a task-relevant summary in
all forty attempts, two unsupported “work completed” claims with no edits, and
one specific claimed test execution contradicted by the trace. There was also a
useful counterexample: one **successful** Codex JSON-diff implementation honestly
reported that its own test had failed because the expected ordering was wrong.
Truthful uncertainty is not scored as artifact failure when the oracle passes.

No contradictory specific validation claim was identified in the twenty Tny
answers. This is a supplementary manual review, not a blinded external grader
or a numerical aesthetic ranking. The exact annotations and source references
are in `data/quality-review.json`; every final answer, patch and oracle log is
retained in the evidence archive.

## Prompt caching: two distinct workloads

All forty cache questions were answered correctly with one model request each.
Both arms received the complete same 200-record catalog for each paired round,
with a new shared nonce for each repetition. Default cache settings were not
changed. A first request is **not guaranteed cold**: prior server caches cannot
be flushed by this experiment.

The following rates pool **turns 2–5** over two repetitions:

| Scenario | Tny cached/input | Codex cached/input | Non-cached input, Tny / Codex |
|---|---:|---:|---:|
| Resume the same conversation | **81.7%** | **93.0%** | 8,840 / 8,448 |
| Start fresh sessions in the same workspace | **94.5%** | **39.5%** | 2,624 / 72,888 |

In resumed conversations Tny missed one of eight later cache opportunities;
Codex hit all eight. The other seven Tny turns had roughly 93–94% cached input.
This is evidence about the small observed run, not a stable 11-point expected
cache penalty. On warmed resumed turns, absolute non-cached input was almost
the same despite Codex's much larger prompt.

Across fresh sessions, Tny reused **one workspace cache key** throughout each
five-turn round; Codex used **five session keys**. Tny hit all eight later turns.
Codex sometimes reused its common base prefix but not the full catalog. The
observations are consistent with their routing-key scopes, but this was not a
randomized intervention isolating the cache key from all request differences.
Tny's workspace-scope routing already exists and should be retained, not proposed
as a new feature.

Natural coding tasks showed a different aggregate: **62.6%** cached input for
Tny versus **81.9%** for Codex. A higher cached percentage is not automatically
better: Codex still used **89,950 more non-cached input tokens** over the scored
cohort. Adding prompt padding to chase a better cache ratio would be the wrong
optimization. Official guidance emphasizes stable exact prefixes and measuring
actual cached tokens; account-backend details here are measured rather than
inferred from API pricing documentation.

### Concrete affinity-header incompatibility

Every sampled cache response carried an opaque `x-codex-turn-state` value of
**780 bytes**. Tny's `src/net/http1.c` retains at most 511 bytes per header, and
`src/backends/openai/openai.c` rejects an affinity value at that truncation limit
before storing it in a 512-byte array. The scored traces show **zero** Tny
requests carrying turn affinity; Codex sent it on its within-turn continuations.
The cache probes start distinct user turns, so neither is expected to echo a
previous user's turn token there.

This establishes a concrete incompatibility, not the size of a performance win
from fixing it. Tny cached successfully without affinity too. The causal effect
on cache consistency and latency needs a follow-up A/B run after the header path
is corrected. Only header lengths/hashes were retained, not opaque token values.

## Recommended Tny optimizations

### 1. Make embedded Python behave like launchable Python

**Evidence:** seven `sys.executable` subprocess failures in seven of twenty Tny
runs. The empty string produced `PermissionError` and additional recovery calls.
The existing prompt already warns that `sys.executable` is empty; the model still
uses the normal Python idiom.

Implement a genuine executable-compatible Python entry point supporting `-c`,
`-m` and scripts, and point `sys.executable` to a real launcher. Do not fake it
with a path containing arguments, and do not silently switch Python versions.
Preserve embedded CPython and host access. Test subprocesses, multiprocessing
spawn/forkserver, environment/cwd, quoting, exit status and cancellation. Measure
new successful-task token/call totals; seven observed failing calls are not a
promise of exactly seven calls saved after changing behavior.

### 2. Preserve complete turn-affinity headers

**Evidence:** measured 780-byte values cannot survive the existing 511-byte
capture path. Use appropriately bounded owned storage through HTTP parsing,
provider state, outbound header construction and checkpoint/IPC serialization.
Keep CR/LF validation and correct per-turn scope. Check exact round-trip bytes
with synthetic headers, then rerun cold-ish/warm multi-tool turns. Do not claim
a cache gain until the randomized A/B confirms it.

### 3. Reduce accidental cross-cell state assumptions

**Evidence:** three Tny `NameError` failures came from reused variables/imports
that were absent in the fresh next cell. Four other calls were rejected because
the model supplied empty code. Neither kind of failure improved task quality.

A durable solution is an explicit per-turn Python worker with a clear reset and
restart contract, or a small supported workspace/session object for intermediate
state. An interim alternative is a compact execution helper and better error
context that includes the fresh-cell boundary. Do not blindly replay a cell with
host effects. Persistence would need careful isolation of unrelated sessions,
module reload behavior after edits, cancellation and memory accounting. Evaluate
it as an architectural trade-off, not a free optimization.

### 4. Keep large, repetitive file content out of repeated context

**Evidence:** both Tny API-migration attempts printed every client file, producing
**65,510-byte tool results** near the capture ceiling. In tier 5, non-cached-token
savings narrowed to 7.3%. Across all tasks, Tny returned 285,877 unique tool-result
bytes and replayed about 1.51 million tool-output characters in later requests.

Prefer targeted reads, structured symbol/call-site inventories and representative
client inspection before a mechanical multi-file change. Preserve full output as
an artifact with addressable excerpts; do not throw away failed-test details or
remove necessary context merely to reduce token counts. Validate unchanged output
quality on new migration tasks as well as these published fixtures.

### 5. Ground completion summaries in execution records

Tny did not show the no-edit false-completion failure here, but the Codex examples
illustrate a valuable regression guard for either harness. Produce a compact
completion ledger from actual changed files, test commands/exit codes and outputs.
Make it available to the final response; flag a claimed file-changing completion
when no corresponding effect exists. This should improve evidence quality, not
introduce another subjective code-review or approval loop. Test with no-edit,
failed-test, correct-but-unverified and format-sensitive controls.

## Evidence, reproducibility and cleanup

`results.csv` contains all forty scored rows. `data/analysis.json` and
`data/cache-analysis.json` contain exact aggregates, per-task/per-tier results,
paired intervals and cache-turn diagnostics. The 4.3 MB `data/evidence.tar.gz`
contains 1,232 members: provider usage receipts, sanitized request bodies, CLI
output, final answers, patches, final-source overlays, oracle logs, fixture
integrity and control results. `data/SHA256.json` binds the exported files.

Authentication stores, runtime HOME directories, Git internals, generated binaries
and opaque encrypted reasoning handles are excluded. The export inventory records
both original and sanitized representation hashes. Provider usage remains exact;
no sanitized body is represented as byte-identical to its original wire SHA.
Unused legacy API-price estimates from the inherited harness are removed.

The protocol was committed before scored inference (`63a3b50`), and the validated
execution/recording driver at `9ce95dd`. Original controls initially exposed a
copied task-selector list pointing at absent fixtures; it was fixed and all ten
controls passed before scored trials. No scored task, oracle, time budget or
prompt was changed after seeing outcomes. Five offline accounting checks passed,
and final analyses independently matched all provider counters and CLI totals.

Production Tny/Codex source and binaries were not modified. Cleanup completion
is recorded separately in `cleanup.json` after removal of the owned evaluation
worktree and marker-identified temporary root. Unrelated worktrees, user files,
credentials and existing sessions are deliberately preserved.

### Sources

Primary evidence is the committed experiment data and the frozen source files:
`src/core/tools.c`, `src/core/code_python.c`, `src/net/http1.c`, and
`src/backends/openai/openai.c` at the recorded main snapshot. General cache
interpretation was checked against OpenAI's official [prompt-caching guide](https://developers.openai.com/api/docs/guides/prompt-caching)
and [cache diagnostics](https://developers.openai.com/api/docs/guides/prompt-caching/diagnostics),
accessed 28 September 2026. Those API documents are not used to invent subscription
pricing or undocumented model guarantees.
