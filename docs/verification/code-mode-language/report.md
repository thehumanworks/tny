# Tny code-mode language evaluation

## Decision

**Keep production Lua.** JavaScript and Python were more output-token-efficient
for `gpt-6-luna` in this experiment; Lua was the smallest and fastest measured
embedded runtime. The observed alternative-language advantage is not sufficient
to warrant a production migration under the preregistered rule and Tny's measured
embedding costs. Python clears the model-efficiency threshold, but is rejected on
the separate runtime/containment trade-off, not on a claim of worse model quality.

This work does not change production code or agent prompts. All alternative
interpreters live in an optional development benchmark.

## Live experiment

The actual requested model was `gpt-6-luna`, low reasoning effort, via Codex CLI
0.159.0-alpha.4 using the existing **ChatGPT login**. No API-key provider, model
substitution, custom credential-reading client, or manual code correction was
used. The CLI request arguments and completion/usage events are retained. The
CLI does not independently attest the identity of backend model weights; the
claim is the configured, accepted model request, not a cryptographic identity proof.

The twelve families cover filtering, grouping, joins, pagination, conditional
batch edits, nested JSON shapes, NDJSON, stable sorting, bounded retries, literal
replacement, Unicode scalar handling and dependency ordering. Three generations
per family/language give **36 programs per language, 108 total**. Every program
runs against three hidden fixture variants. A pass requires exact final virtual
file effects, required reads/calls, no undeclared/schema-invalid capability,
successful execution and the requested completion marker on **all three** variants.

The models see equal synchronous typed capabilities. Each `tools.call` takes a
name and JSON-encoded arguments and returns a string. Language-specific help
explains the real available APIs, including Lua's null sentinel and explicit empty
array constructor. Models receive no fixture values, oracle answers or handwritten
reference solutions. Language order is rotated within task/repetition blocks;
concurrency is three. Fresh generation directories are outside the repository,
with user configuration ignored and no Codex tool use allowed in a valid trial.

A failed program gets one fresh-session repair with its code and observed failure,
not the expected answer. Every first attempt is retained. Simulated state resets
between attempts; this does **not** authorize replay of real production effects.

### Agent results

| Measure | Lua 5.4.9 | QuickJS 2026-06-04 | CPython 3.14.7 |
|---|---:|---:|---:|
| First-attempt success | 35/36 (97.22%) | 36/36 (100%) | 36/36 (100%) |
| Success after at most one repair | 36/36 | 36/36 | 36/36 |
| Repairs needed | 1 | 0 | 0 |
| Total model generations | 37 | 36 | 36 |
| Total reported output tokens, repairs included | 6,221 | 5,025 | 4,545 |
| Output tokens per solved task | 172.81 | 139.58 | 126.25 |
| First-attempt generated source bytes, mean | 526.86 | 476.78 | 369.58 |
| Median generation wall time per task, repairs included | 8.349 s | 7.707 s | 7.517 s |
| Nested calls, all variants/attempts | 345 | 342 | 342 |

Generation time includes Codex CLI startup/network/model time. It is not Tny
end-to-end latency. Runtime work happened in actual embedded interpreters, not
Node or an external Python-script substitute. The Lua adapter links the exact
production runtime source.

Lua's single failure was an unnecessary `json.decode(raw)` on a complete NDJSON
stream before an otherwise correct per-line loop. The repair removed that whole-
stream decode. This is not evidence of a Lua syntax defect, an empty-array failure,
or a widespread inability to program in Lua. Every Lua JSON-shape, Unicode and
literal-pattern case passed first attempt.

### Uncertainty and the predeclared rule

The decision rule was committed **before live inference** in `47972c0`. It asks
for no observed success regression and task-clustered evidence of either a
reliability improvement or at least **15% lower output tokens per solved task**.
It then requires acceptable production compatibility and runtime trade-offs.

A paired percentile bootstrap resamples twelve task families, preserving all
three repetitions within each selected family, with 10,000 draws and fixed seed
20260927. It does not pretend the 108 executions or the three fixture variants
are independent task families.

| Candidate versus Lua | Observed output-token saving | Task-clustered 95% interval | Clears the model-efficiency gate? |
|---|---:|---:|---|
| JavaScript | 19.23% | 11.88%–25.11% | No: lower saving bound is below 15% |
| Python | 26.94% | 18.70%–33.96% | Yes |

Both candidates' first-pass improvement is 2.78 percentage points, with interval
0–8.33 points. This small sample does not establish a reliable success-rate
advantage. JavaScript's token benefit is real in the observed corpus; the decision
is not a claim of “no difference.” The threshold is an engineering criterion,
not a statistical law. Intervals remain exploratory given the handcrafted corpus
and only twelve independent task clusters.

### Input usage and caching

| Reported usage, all attempts | Lua | JavaScript | Python |
|---|---:|---:|---:|
| Input tokens | 548,869 | 522,820 | 523,497 |
| Cached input tokens | 411,648 | 365,568 | 365,568 |
| Uncached input tokens | 137,221 | 157,252 | 157,929 |
| Reasoning output tokens | 0 | 0 | 0 |

The normal Codex harness contributes substantial common input overhead. Cached
input was not equal across arms, and Lua had one extra generation. These numbers
are not converted into API prices, subscription quota credits, or a claim of
19%/27% lower total workflow cost. The output-token metric is narrower and explicitly
includes failed first attempts and repairs.

## Embedding footprint and startup

Same Linux x86-64 host, GCC 16.2.1, `-Os`, LTO, section garbage collection and stripped
probe executables. The host transports the same virtual capability fixture and
records effects. No QuickJS standard OS/I/O library is linked. These are comparable
native embedding probes, **not** complete migrated Tny release binaries.

| Artifact | Stripped bytes | Interpretation |
|---|---:|---|
| Empty host | 96,568 | Common fixture host, JSON parser and recording machinery |
| Lua probe | 277,248 | Current Lua runtime and actual Tny binding, self-contained except system libraries |
| QuickJS probe | 748,088 | JS engine and experimental binding, self-contained except system libraries |
| CPython host | 117,568 | Misleading alone: dynamically loads a separate Python runtime |
| CPython shared runtime, separately stripped | 31,867,976 | Installed CPython distribution, before separate stdlib files |

QuickJS is **2.70×** the Lua probe's total size, an additional **470,840 bytes**.
Subtracting the common host gives 180,680 versus 651,520 bytes, but that subtraction
is a diagnostic rather than a guaranteed full-product link delta. The measured
base Tny release before this work was 1,373,232 bytes; its runtime is unchanged.

The installed CPython shared library before separate stripping was 33,163,040
bytes; its stdlib directory excluding site-packages and bytecode caches was
16,857,219 bytes. Not all of that stdlib is needed by this experiment. The observed
loaded-module source-file list is also retained, but is not a minimal closure:
modules can be frozen, and missing import-time files can matter later. No optimized
static/frozen CPython build was attempted. These measurements do not prove a
universal minimum Python footprint. Likewise, Python's prebuilt shared library
was not rebuilt under the probes' compiler flags.

A separate serial native microbenchmark ran 50 empty-cell processes per arm after
five warmup blocks, rotating order. It executes only trusted empty source and
excludes bubblewrap. OS pages are warm; this is not cold-machine latency.

| Median | Empty host | Lua | QuickJS | CPython |
|---|---:|---:|---:|---:|
| Interpreter initialize + evaluate + destroy | 0.00013 ms | 0.114 ms | 0.457 ms | 23.663 ms |
| Whole fresh native process | 1.695 ms | 1.911 ms | 2.184 ms | 29.058 ms |

These runtime differences are small compared with a model generation. Python's
faster observed generation more than offsets its initialization overhead in this
small corpus; startup alone is not the migration decision. Footprint, hard-budget
compatibility, containment work and confidence in a reusable benefit also matter.

## Candidate parity diagnostics

These are **separate post-trial adapter probes**, never additional model failures.
Lua, QuickJS and CPython all rejected output beyond 64 KiB. However, native QuickJS
JSON round-tripping changed `9007199254740993` to `9007199254740992`; the current
Lua JSON bridge and CPython preserved it. That is a compatibility gap for the
chosen native JSON interface, not an impossibility claim about lossless JS APIs.

A 20 MiB cell allocation was rejected by the Lua and QuickJS 16 MiB allocator
limits, but admitted by this CPython probe. CPython's isolated initialization and
restricted globals are not a general sandbox. All generated programs therefore
ran inside mandatory bubblewrap without network, host HOME/workspace or inherited
secrets. The candidates are prototypes, not audited production replacements.

The initial live run had a six-second outer watchdog, while its advertised cell
budget and Lua/QuickJS interpreter deadlines were two seconds. A later audit
identified the mismatch and tightened the common outer limit to two seconds.
**All 327 frozen attempt/variant executions were then replayed**, with identical
pass/fail outcomes and no new inference or source edits. The original measured
latencies remain original, not replaced by selected replay times. The common
process-memory ceiling was 512 MiB; Lua/QuickJS additionally enforced 16 MiB on
the interpreter heap. Do not interpret this as an equal-heap Python experiment.

## Reproduction and evidence

See `tests/bench/code_mode/README.md` for exact commands. Live inference is always
opt-in; normal tests need neither an account nor a downloaded interpreter.
`data/samples.json` includes every generated program, usage receipt and observed
variant outcome. `data/raw-generations.tar.gz` retains synthetic prompts, CLI
events and generation receipts. No credential file is read or copied by the
benchmark. `data/experiment.json` binds model/configuration, source hashes, exact
build commands and executable hashes. `data/SHA256.json` binds exported artifacts.
The original harness source is reachable in commit `7ed5441`; later formatting
and audit additions do not replace the original live manifest.

Before live trials, all **108 handwritten reference executions** passed. An
independent canonicalization audit reconciled all **327** attempt/variant outcomes
and actual generated-code/usage events; all **327** frozen cells were replayed.
One successful JSON answer is never treated as proof that its virtual effects
were correct. Remaining validation and host-specific limitations are recorded in
the evidence ledger rather than silently omitted.

## Formal scope

Lean 4.12.0 verifies eighteen theorems about the **actual source-linked** acceptance
and promotion gates, plus the two concrete observed retention decisions. A whitelist translator reads the Python AST and generates
Lean `Bool` and unbounded `Int` definitions; unsupported syntax fails closed.
Proofs establish exact acceptance conditions, non-vacuity, mandatory completeness,
confirmed gain, parity and first/final non-regression. Four altered gate programs
must fail the actual Lean proof. Standard propositional extensionality (`propext`)
is reported; there are no admitted proofs or new axioms.

This is not a proof of model competence, bootstrap coverage, all Python/C memory
safety, interpreters, operating-system containment, or the entire Tny application.
Production execution code is unchanged. Statistical analysis, parser/schema
boundaries, raw-event integrity and runtime behavior have separate executable
evidence. Retaining Lua is a measured engineering decision, not a theorem that
Lua will always be the best language.
