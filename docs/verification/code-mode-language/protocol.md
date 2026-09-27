# Code-mode language evaluation — preregistered protocol

Base revision: `23852ca`. Requested model: `gpt-6-luna`, Codex ChatGPT login,
low reasoning effort. No substitute model and no API-key provider. The normal
Codex CLI owns authentication; the benchmark never reads or copies credentials.

## Question and experiment

Compare writing bounded code over the same synchronous, typed capabilities in
production Lua 5.4.9, embedded QuickJS 2026-06-04, and embedded CPython 3.14.7.
This measures code-cell generation/composition, not whole-repository coding or
end-to-end Tny provider latency. All three use the same Codex CLI generation
harness; only the language/codec instructions differ. The Lua executor links the
actual production `src/core/code_runtime.c`, not a reimplementation.

Twelve task families: filtering, aggregation, joins, pagination, batch editing,
JSON shape preservation, NDJSON, stable top-k, bounded retries, literal string
replacement, Unicode scalar handling, and dependency ordering. Three independent
generations per language/family (36 per language); every program is evaluated
against three private deterministic fixture variants. Fixture values and oracle
answers are not in generation prompts. Tasks use deterministic virtual files and
API replies, never the real workspace, network tools, or account data.

Every first attempt is retained. A failed program gets at most one fresh-session
repair with its original code and observed failure, but no oracle answer. Each
attempt is executed in fresh isolated processes; partial simulated writes never
carry into a repair. Real-world effects cannot in general be safely replayed;
this repair policy is an evaluation fixture, not a production retry policy.

Language order is rotated within paired family/repetition blocks. Generation
concurrency is bounded; all arms receive identical effort, schema, code/source,
execution wall-time, output and tool-call ceilings. CPython's isolated config is
not a security sandbox. Linux bubblewrap with no network, no host HOME/workspace,
no inherited secrets and read-only interpreter dependencies is mandatory for all
model-generated programs. There is no unsandboxed fallback.

## Measures and acceptance

A program passes only when **all three variants** execute successfully, produce
the exact typed final virtual-file contents, use only declared/schema-correct
capabilities, and actually read required inputs. JSON comparisons distinguish
null/false/zero, arrays/objects and string/number types. A success statement is not
an oracle. Invalid/missing provider completion, tool-using generation, absent
usage or wrong requested model cannot be counted as a completed sample.

Report first-pass success, success after repair, repairs, model input/cached/
uncached/output/reasoning tokens, generated-source bytes, nested calls, wall time,
and output tokens per solved task. Retain failures in denominators. Report task-
clustered paired bootstrap intervals (all repetitions of a family resample
together), and clearly distinguish sample observations from general model claims.
The sample is small, handcrafted and not independent of the author's design.

Measure stripped native executors built with the same `-Os -flto` flags, an empty
host control, runtime initialization+evaluation time, and fresh-process time.
Account for CPython's separately loaded libpython and stdlib; a tiny dynamically
linked executable is not a tiny bundled interpreter. These are native embedding
probes, not a claim that a complete production migration has already been built.

## Decision rule, before observing results

Prefer the existing runtime unless a candidate has no first-pass or eventual
success regression, and task-clustered uncertainty supports either a reliability
improvement or at least 15% lower output tokens per solved task. An apparent gain
must also survive production capability/budget/JSON compatibility verification
before migration. Footprint, startup and implementation complexity determine
whether the gain is worth the change; size alone is not a fixed product ceiling.
If the evidence is inconclusive, retain Lua and publish the result rather than
inventing a winner or an equivalence claim. Any protocol amendment is recorded
before the affected evaluation, with its reason; do not silently alter thresholds.

## Formal verification scope

Lean must prove the actual source-linked acceptance and promotion gates, including
rejection on any failed boundary, incompleteness, or observed success regression.
Independent recomputation must reconcile the raw sample cohort and reported
aggregates. Proofs certify those deterministic rules, not stochastic model
competence, the C interpreters, the OS sandbox, or Python memory safety. Existing
production source-linked SMT checks and executable regressions remain separate.

## Execution notes and deviations (recorded after the first cohort)

The live cohort used a six-second *outer process watchdog*, although every prompt
advertised two seconds and Lua/QuickJS had two-second interpreter deadlines.
CPython did not have an internal wall-clock hook. The common outer watchdog was
then tightened to two seconds, and **all 327 frozen attempt/variant executions
were replayed without model calls or code edits**. Every pass/fail result was
unchanged; `data/audit.json` records that replay. Original live timings remain
unchanged and are not mixed with replay timings. CPython has no matching 16 MiB
interpreter heap limit: the common process limit was 512 MiB, whereas Lua and
QuickJS additionally used their 16 MiB heap limit. This is an explicit candidate
parity gap, not an equal-memory benchmark claim.

A separate serial empty-cell timing probe uses only trusted, handwritten input,
not generated programs, and excludes bubblewrap overhead. It measures warm-page-
cache startup, not a cold host. Compatibility probes were added after model
trials and are reported separately; they never alter model scores or denominators.
The only model error was decoding an entire NDJSON stream as one JSON document
before a correct per-line loop. No fixture, task, success oracle, language prompt,
repair budget, or decision threshold was changed in response to that error.
