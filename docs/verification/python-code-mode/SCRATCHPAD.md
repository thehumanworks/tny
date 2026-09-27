# Python code-mode migration scratchpad

Date: 2026-09-27
Branch: `feat/python-code-mode-runtime-20260927`
Parent: `175092fdac88825b65ad10f8f9b0f1ddc630e177`

## Goal

Migrate Tny's embedded code-mode agent runtime from Lua to Python. Benchmark
lighter Python-compatible runtimes first; prefer a lighter runtime only if it
does not reduce the already measured Python code-generation efficiency. Binary
growth is acceptable when justified by the agent-efficiency benefit.

## Required process

- Work only in this worktree.
- Preserve the prior Lua/QuickJS/CPython benchmark as immutable evidence.
- Keep benchmark protocol/results and runtime-selection rationale reproducible.
- Use real `gpt-6-luna` Codex-account trials when comparing prompt/runtime
  compatibility; do not infer generation efficiency from popularity.
- Formally verify changed deterministic policy/runtime gates with Lean.
- Run production regressions and explicit migration tests.
- Commit frequently; do not force-push.
- Claude Opus is the implementation/research agent. Its durable transcript is
  written outside Git under `.agent/`.

## Candidate runtime questions

At minimum investigate CPython, MicroPython, PyPy, RustPython, and other credible
maintained embeddable Python runtimes. A candidate must run the Python benchmark
task corpus without rewriting generated programs. If its Python subset/API
requires model prompt changes, run paired Luna generation trials against that
actual runtime before claiming equal efficiency.

## Initial evidence inherited from PR #197

CPython generation: 36/36 first-pass, 126.25 output tokens/solved task.
Lua generation: 35/36 first-pass, 172.81 output tokens/solved task.
Installed CPython probe: 31,867,976-byte stripped shared runtime plus host/stdlib.
Lua probe: 277,248 bytes.
Python passed the preregistered model-efficiency threshold; migration was rejected
only on footprint/containment trade-offs. User has now explicitly accepted those
costs if lighter alternatives lose efficiency.

## Journal

### 2026-09-27 — worktree created
Migration starts from the completed/reconciled benchmark revision. No runtime
selection has been made yet.

### 2026-09-27 16:20–16:40 — orientation and candidate acquisition
Read ADRs 0174/0178, the prior report/protocol/evidence, production
`src/core/code_runtime.c`, `src/core/execution.c` call site, `tools.c` prompt,
unit tests and integration fixtures (`code_mode_fixture.lua_string` wraps every
mock provider tool call as Lua). Supervisor guidance: Python decision is made;
compare Python implementations; keep null/false/zero and empty-container
shapes; report runtime metrics separately from generation metrics.

Pinned candidate sources (digests checked against publisher metadata):

| Candidate | Version | Artifact | SHA256 |
|---|---|---|---|
| PocketPy | v2.2.0 (2026-09-04) | `pocketpy.c`/`pocketpy.h` release assets | `0f0c1907…6e56` / `43864cfa…3ad` (GitHub asset digests match) |
| MicroPython | v1.29.0 (2026-08-24) | release tarball | `d925a7c6…ea0` (GitHub asset digest matches) |
| Monty | v1.0.0 (2026-09-25) | git tag `85c5d1f6` | Rust workspace, rust-version 1.96 |
| CPython | 3.14.7 | `Python-3.14.7.tar.xz` | `3b48dac8…f81` (python.org release-file API matches) |

Eliminated from primary sources without building (architecture, not taste):
- **PyPy**: no supported C embedding of an RPython-translated interpreter
  inside another executable beyond cffi embedding of a separate libpypy; JIT
  runtime is tens of MB and translation takes hours. Heavier than CPython.
- **RustPython**: full-language goal, larger and slower than CPython in its
  own published benchmarks; brings a Rust toolchain without a sandbox design.
  Monty is the Rust candidate that was actually designed for this job.
- **GraalPy/Jython/IronPython**: JVM/.NET hosts. **Brython/Skulpt/Pyodide**:
  browser JS/wasm hosts. **Starlark**: no `try/except`, no `while` by default,
  different dialect. **Codon/Nuitka/Cython**: compilers, not cell runtimes.
  **PikaPython/snek/tinypy**: microcontroller subsets smaller than MicroPython.

Semantic conformance harness: `tests/bench/python_runtime/semantics.py`
(127 isolated cases, each a fresh interpreter, stdout+status must equal
CPython 3.14.7) and `conformance.py`.

First results (probe runners, full-feature configs, each runtime's own `json`):
- PocketPy 2.2.0: 67/127. Fails nested comprehensions, standalone generator
  expressions and genexpr-as-argument (`sorted(x for x in …)` is used by the
  frozen corpus), `try/except/else`, `nonlocal`, `**=`, `max(default=)`,
  stable `reverse=True`, f-string specs, bigints beyond i64, JSON (eval-based,
  accepts `[1,]`, rejects ints > i64).
- MicroPython 1.29.0 (EXTRA_FEATURES, MPZ ints, unicode): 78/127. **Dicts are
  not insertion ordered** (JSON object key order changes on round trip), no
  `{**d}` literal, no `__cause__`, no stepped Unicode slices, unstable
  `reverse=True`, float `round`/`format` and `sum` precision differ, no
  `\N{}`. Its json module was not in the embed build; production would use a
  native facade anyway, but dict ordering cannot be fixed by a facade.
Both are eliminated on semantics; frozen-corpus replay is still run for the
record.

### 2026-09-27 16:45 — corrected probes and first corpus replays
Bug found in my MicroPython runner/adapter: the embed example compiles with
`is_repl=true`, which echoes expression-statement values. Fixed to `false`
and re-ran everything; earlier MicroPython numbers above are superseded.

Frozen corpus replay (`replay.py`: 36 published Python programs, unmodified,
three hidden variants each, same bwrap/prlimit envelope and scorer):

| Runtime | Stripped probe bytes | Conformance (127) | Corpus programs | Variant executions |
|---|---:|---:|---:|---:|
| empty host control | 96,568 (identical to PR #197) | — | 0/36 | 0/108 |
| PocketPy 2.2.0 | 630,504 | 67 | 27/36 | 83/108 |
| MicroPython 1.29.0 | 371,200 | 88 | 33/36 | 99/108 |
| CPython 3.14.7 (PR #197) | 117,568 + 31.9 MB libpython | 127 (reference) | 36/36 | 108/108 |

PocketPy fails `sorted(x for x in …)` / `all(... for ...)` (syntax), and
`json.dumps(ensure_ascii=False)`. MicroPython fails the three Unicode programs
on `ensure_ascii`. Its 33 passes are real passes under the published contract,
which compares JSON objects by keys and typed values, not serialized key order.
Separately, MicroPython dict iteration/serialization order differs from
CPython's insertion order; that matters for tasks that explicitly require an
ordered result or byte-stable rewrites of existing JSON, and is tested
separately rather than retroactively added to the published corpus.

Static CPython 3.14.7 (`--disable-shared`, bootstrap modules only, `-Os
-ffunction-sections`, no LTO yet, encodings frozen via `_freeze_module`, no
stdlib directory at run time): 4,586,392-byte stripped probe with only
libc/libm; init ≈11.8 ms, empty run 0.14 ms, finalize ≈3.3 ms (warm). At
init `posix` and `_io` are loaded, so object-graph introspection
(`().__class__.__base__.__subclasses__()`, function `__globals__`) reaches
`_io.FileIO`/`posix`: restricted builtins cannot be the boundary. CPython
therefore needs an OS-confined interpreter process that holds no tool
authority.

### 2026-09-27 17:01 — Monty, controls, preregistration (commit `77564e5`, before the live run)
Monty 1.0.0 built from tag `85c5d1f6` (CLI 27.3 MB incl. type checker; the
embedding probe is a Rust staticlib shim `monty_probe/` with fat LTO).
Conformance 103/127; frozen corpus **36/36 (108/108)**; probe 6,165,632 B.
Genuine gaps for generated code: `yield`, `del`, inheritance/custom
exceptions, `match`, eager genexprs (`range(10**9)` genexpr times out),
`True + 1` TypeError (`sum(bools)` idiom), `key=str.lower`, no
`callable/issubclass/ascii`, multi-arg exception constructors error.

The preserved PR #197 CPython adapter's narrow builtin whitelist lacks
`type`, `KeyError`, `RuntimeError`, and its `print` has no `sep/end`; it does
not represent the intended production runtime. Added `cpython_prod.c` with
the proposed production policy. Both CPython adapters reproduce 36/36.

Equal-host static CPython size probe: 4,668,312 B (libpython objects not
LTO-compiled yet). Empty-cell medians (50 rotated rounds): static CPython
13.79 ms, Monty 0.79 ms, MicroPython 0.22 ms, PocketPy 2.39 ms, stock shared
CPython with stdlib json/types imports 29.40 ms.

Held-out controls 108/108 on the three arms. Protocol and `policy.py`
committed before inference.

### Held-out Luna trial result and runtime decision
Live run `build/python-runtime-bench/live-heldout` (manifest source revision
`77564e5`, codex-cli 0.159.0-alpha.4, ChatGPT login, gpt-6-luna, low effort,
3 workers): 108 first generations + 3 repairs, all valid, exit 0.

| Arm | First pass | Final | Repairs | Output tokens (all attempts) | Tokens / solved | Uncached input |
|---|---:|---:|---:|---:|---:|---:|
| cpython (production wording, full builtins) | 35/36 | 35/36 | 1 | 9,428 | 269.37 | 78,192 |
| cpython_pr197 (PR #197 wording, narrow builtins) | 35/36 | 35/36 | 1 | 9,691 | 276.89 | 103,386 |
| monty (subset disclosed) | 35/36 | 36/36 | 1 | 10,421 | 289.47 | 59,138 |

Task-clustered bootstrap (10,000 draws, seed 20260927), saving vs `cpython`:
PR #197 wording −2.79% [−7.76%, +1.93%]; Monty −7.46% [−15.38%, +1.54%].
First-pass differences 0 [0, 0]. Failures are genuine model parsing errors
in `log_errors` (unpacking a 4-way split; `partition(":")` on the component
field leaving an empty message), not executor artefacts.

`policy.select_lighter` = **false** for Monty: it used more output tokens per
solved task (10,421×35 > 9,428×36) and its probe is larger than static
CPython (6,165,632 > 4,668,312 B); its final 36 vs 35 is one task. Decision:
**CPython 3.14.7, pinned and statically embedded**, isolated in an
OS-confined cell process. The production wording is not worse than PR #197's
on held-out tasks (observed 2.8% fewer output tokens, interval spans zero).

### 2026-09-27T18:20:05.979102+01:00 — integrated review checkpoints
Merged codec ownership/metadata corrections and process-terminal quota handling;
child DONE+EOF now also requires successful exit. Replaced newline name framing
with a fixed three-digit byte count, preserving exact names and bounded arguments.
Added persistent production regressions: 14/14 runtime cases, 125 assertions;
3/3 transport/process cases, 383 assertions. No assertion or sanitizer suppressed.

The independent Opus/high proof worker completed normally (exit 0). Integrated
its typed Clang-AST → Lean BitVec translation, source mutations and runtime-choice
proof; the superseded simpler checker is retained only in history. On the amended
frame bounds, Lean 4.12.0 proved 36 specifications, one helper, 17 generated
no-wrap obligations; 6,070 compiled GCC/Clang UBSan vectors matched Lean kernel
evaluation. Thirteen selection theorems plus 371 replay vectors passed. This does
not prove interpreter/OS or all call-site implementation correctness. Full mutation
suite remains pending a durable completed run (one tool waiter timed out).

### 2026-09-27T18:29:07.740414+01:00 — fair MicroPython JSON control
A benchmark-only prebound json facade now implements ensure_ascii without
editing any generated Python code or prompt. The original 36 frozen programs
all pass (108/108 variants); the old 33/36 unadapted measurement remains separate.
The 371,200-byte stripped compatibility probe is genuinely much smaller.

On the newer saved programs, the same adapter passes 12/36 CPython-wording
programs, 10/36 old-wording programs and 14/36 Monty-wording programs. All 111
attempts/333 variants were retained. This is runtime replay evidence, not fresh
MicroPython-specific generation. The held-out tasks target known order/semantic
requirements; it is not a generic model ranking. Detailed outputs remain in
replay-micropython-json-compat.json and heldout-replay-micropython-json-compat.json.
The original JSON-wrapper gap alone therefore does not justify excluding
MicroPython, but this follow-up does not preserve ordinary generated-program
behavior on the targeted compatibility corpus. Full CPython remains selected.
