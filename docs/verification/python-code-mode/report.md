# Tny Python code-mode migration

## Result and scope

The selected runtime is **CPython 3.14.7, statically embedded with frozen bootstrap
modules**. Tny's model-facing run_code now accepts Python. Code mode requires no
installed Python executable, shared libpython or filesystem standard-library tree.
The production Lua dependency is removed; a test-only pinned copy retains the
reproducibility of the older language benchmark. The user explicitly accepted
binary growth to preserve Python code-generation efficiency.

This report separates four kinds of evidence: equal-host interpreter probes,
unchanged-program runtime replays, real Codex-account generation trials, and
production implementation/verification. Probe binaries are not full product
releases, and subprocess latency without a model is not end-to-end agent latency.
Release status and final revision-bound artifact measurements are recorded in
[evidence.md](evidence.md); the architecture is [ADR 0179](../../adr/0179-python-code-mode-cpython.md).

## Python runtime selection

The same native virtual-tool host, GCC 16.2.1, size-oriented compilation and
section garbage collection were used for the following measured configurations.
All generated programs were executed unchanged, with strict typed output/effect
checking and private fixture variants. Failed attempts were retained.

| Runtime/configuration | Stripped probe bytes | Old Python programs passing | Semantic diagnostics matching CPython |
|---|---:|---:|---:|
| PocketPy 2.2.0 | 630,504 | 27/36 | 67/127 |
| MicroPython 1.29.0, native JSON | 371,200 | 33/36 | 88/127 |
| MicroPython plus the later JSON compatibility facade | 371,200 | 36/36 | Not reclassified as full CPython conformance |
| Monty 1.0.0, Rust static-library embedding | 6,165,632 | 36/36 | 103/127 |
| Minimal static CPython 3.14.7 | 4,668,312 | Production replay reported separately | Reference language implementation |
| Stock shared CPython, original narrow adapter | 117,568 plus separate libpython/stdlib | 36/36 | Reference |

The identical empty host was 96,568 bytes. Every old program had three private
fixture variants. Its scorer compares typed JSON contents, not object-key order;
new order requirements were not retroactively added to that published benchmark.

PocketPy's ordinary generator-expression syntax failures were relevant to actual
saved programs. MicroPython's original three Unicode failures were caused by the
prebound JSON API lacking ensure_ascii, **not an unavoidable language limitation**.
A later adapter-only control implemented that keyword, with no changed model
prompt or generated source, and all 36 original programs passed. This control
prevents a fixable wrapper difference from becoming the deciding argument.

The same MicroPython adapter was then tested on the newer saved programs. It
passed 12/36 programs generated for the proposed CPython wording, 10/36 for the
old narrow Python wording, and 14/36 for the disclosed Monty subset wording.
All 111 attempts and 333 variant observations are preserved, including failures
and apparently successful done messages whose effects were wrong. The failures
include the required insertion-order behavior and missing string operations.
These are targeted compatibility replays, **not new MicroPython-specific model
generations**. They do not prove that future prompts or implementation changes
could never improve MicroPython; they show that this small adapter does not
preserve the ordinary Python programs being considered for production.

Monty preserved the old corpus and was a serious generation candidate. Its
measured embedding was nevertheless about 1.32 times the minimal static CPython
probe, with documented/observed Python-subset differences. It is not labelled
inferior in all respects: its empty-cell runtime was much faster, and it solved
one additional held-out task after repair. It failed the declared selection rule
for a *smaller* replacement preserving the required semantics and generation
cost. No comparison here is a universal size lower bound or future-version claim.

PyPy and RustPython were screened but not built in this experiment; **no timing,
size or task-pass figures are claimed for them**. PyPy does support embedding,
with its old interface deprecated in favor of CFFI embedding; absence of any
embedding support would be an incorrect reason to dismiss it. RustPython offers
Rust embedding and explicitly describes its production-readiness limitations.
The measured shortlist focused on small direct embedders and Monty's agent-oriented
subset rather than adding unverified size estimates for unbuilt runtimes.
Relevant primary references are the PyPy embedding documentation
(https://doc.pypy.org/en/latest/embedding.html), the RustPython repository
(https://github.com/RustPython/RustPython), and Monty's limitations reference
(https://pydantic.dev/docs/monty/limitations/). Candidate versions and source digests
in the checked-in build receipt, rather than a moving web page, identify what ran.

## Real model generations

The decision protocol and held-out tasks were committed in **77564e5 before live
inference**. Generation used the normal Codex CLI 0.159.0-alpha.4 with the existing
ChatGPT login, explicitly requesting **gpt-6-luna, low effort**. No credentials
were read/copied by the benchmark and no model or API-key provider was substituted.
The accepted CLI configuration and completion receipts identify the requested
model; this is not a cryptographic attestation of backend model weights.

Twelve new families × three repetitions × three arms produced **108 first
programs**. Each program was evaluated on three private variants, with at most
one repair using the previous code and observed errors, not expected answers.
There were three repairs, hence **111 generations and 333 attempt/variant
observations**. Valid trials used no generation-time tools. Both prompts and raw
completion events are retained in heldout-raw-generations.tar.gz.

The arms test both the runtime and the prompt change: proposed production Python
wording; the exact narrow Python wording inherited from PR #197; and the proposed
wording with an honest description of Monty's subset. Arm order rotated within
family/repetition blocks; concurrency was three.

| Measure | Proposed CPython wording | Old narrow Python wording | Monty subset wording |
|---|---:|---:|---:|
| First-pass success | 35/36 | 35/36 | 35/36 |
| Final success after one permitted repair | 35/36 | 35/36 | 36/36 |
| Generations, failures included | 37 | 37 | 37 |
| Output tokens | 9,428 | 9,691 | 10,421 |
| Output tokens per solved task | 269.37 | 276.89 | 289.47 |
| Median generation time per task | 9.675 s | 9.647 s | 10.178 s |
| Input tokens | 546,160 | 545,242 | 543,234 |
| Cached input tokens | 467,968 | 441,856 | 484,096 |
| Uncached input tokens | 78,192 | 103,386 | 59,138 |

All failures occurred in the log-extraction family. Neither failed CPython repair
is hidden or converted into a successful runtime outcome. Monty repaired its one
failure. The proposed wording did not show an observed success regression against
the old wording; the small study does not establish statistical equivalence.

A paired task-cluster bootstrap uses 10,000 draws with seed 20260927, preserving
all repetitions of each sampled family. Relative to the proposed CPython arm,
the old wording's observed output-token saving is -2.79%, with interval
[-7.76%, +1.93%]; Monty's is -7.46%, with interval [-15.38%, +1.54%]. Negative
saving means more output tokens per solved task. Both intervals span zero.
Input caching is different across arms, so output-token changes are not converted
into subscription quota, billing or total workflow-cost savings.

These tasks are held out from the original corpus but were authored **after**
the runtime semantic diagnostics. Six of twelve explicitly require key order.
They deliberately test compatibility, not an unbiased distribution of coding
workloads. Three fixture variants are not three independent tasks. The ambiguity
in the config-merge prompt and its non-effect on these actual fixtures are retained
in protocol.md. No task, repair budget, prompt or threshold was silently tuned
after seeing this cohort's failures.

The old experiment remains a separate result: Python used 126.25 output tokens
per solved task versus Lua's 172.81, with 36/36 and 35/36 first-pass success.
Those approximately 26.9% savings motivated the user's migration decision, but
must not be numerically compared against the larger, different held-out tasks
as though their difficulty or context were equal.

## Production behavior

The trusted execution server retains tools, MCP, workspace policy, permissions
and state settlement. A further fresh, empty-environment Python child has no
ambient host file/network/process authority. CPython isolated configuration and
removed builtins are not treated as a sandbox; Linux seccomp or the macOS
pure-computation profile is the actual operating-system boundary. Unsupported
hosts, wasm and shared-library model-tool execution fail explicitly rather than
falling back to unconfined or in-process Python.

The metered Python/codec heap is 64 MiB, output is 64 KiB, source and arguments
are bounded, and nested calls are limited to 64. A parent wall deadline bounds
native operations too. Fatal quotas terminate the process before more Python
except/finally/finalizer code can run. A completed message is not accepted unless
the child also closes its stream and exits successfully. Completed effects are
retained, not rolled back or automatically replayed.

Full CPython collection, exception, class, generator, integer and Unicode semantics
are preserved. There is no general standard-library import surface. The prebound
native JSON facade preserves None/false/zero, empty shapes, order and arbitrary
integers; its unsupported cls/decoder-hook/encoding extensions are documented in
ADR 0179. A callback can mutate its input containers without invalidating native
borrowed references. Exact-length tool-name framing prevents delimiter confusion.

Building CPython once per compatible build/ABI/flag combination produces a static
archive and frozen encodings. The package has no code-mode Python installation
step. Shared-library and wasm builds use an explicit stub, while native lifecycle
fault executables link the actual interpreter. Offline source pins and licensing
are included in Nix and release packaging. Saved Lua cells are not silently
translated; new run_code must be Python, and existing syntax failures are explicit.

## Proofs and checks

The **actual C gate definitions** are translated from Clang's typed AST into Lean
fixed-width bitvectors. Lean 4.30.0 checked 36 specification theorems, one helper
and seventeen generated overflow obligations; 6,070 separately compiled GCC/Clang
UBSan vectors agreed with Lean kernel evaluation. The runtime-selection function
is translated from its actual Python AST, with thirteen specification theorems
and 371 replay vectors. All **38 negative mutations** were rejected at their
expected proof or unsupported-syntax stage. Standard Lean axioms are reported;
there are no admitted proofs, shadowed gate definitions or native_decide shortcuts.

This proves the stated deterministic gates, not all C/Python memory safety,
CPython, the JSON parser, operating-system containment or model competence.
Concrete regressions cover quota termination, ordinary exceptions, byte-exact
names, callback ownership, JSON types, malformed inputs and successful reaping.
The independent evidence auditor reconciles all 333 typed effects and 111 raw
code/usage receipts and rejects missing/duplicate cohorts or fabricated zero usage.
Final test/build/CI status and production binary/latency receipts remain separately
revision-bound in evidence.md rather than being inferred from a successful proof.

## Reproduce

See tests/bench/python_runtime/README.md for exact opt-in build, replay and live
trial commands. Normal verification never calls a provider. Data files and raw
receipts are synthetic and source-bound. The chronological scratchpad preserves
old measurements, corrected probes, failed gates and subsequent fixes instead of
rewriting history into an all-green narrative.

### Unicode parser bootstrap
The builtin unicodedata module is statically linked and preloaded for the
CPython parser's non-ASCII identifier normalization and named Unicode escapes.
It is not a general import permission or an external stdlib dependency. A
production regression verifies Greek identifiers, NFKC normalization and named
Unicode escapes while user imports remain refused. Earlier minimal-probe sizes
remain historical; the final production artifact includes this required module.
