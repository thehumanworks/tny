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
on `ensure_ascii`; its passes are partly an oracle artefact: the scorer parses
JSON, so its non-insertion-ordered dict output (key order changed) is not
penalised although user-visible JSON files would be reordered.

Static CPython 3.14.7 (`--disable-shared`, bootstrap modules only, `-Os
-ffunction-sections`, no LTO yet, encodings frozen via `_freeze_module`, no
stdlib directory at run time): 4,586,392-byte stripped probe with only
libc/libm; init ≈11.8 ms, empty run 0.14 ms, finalize ≈3.3 ms (warm). At
init `posix` and `_io` are loaded, so object-graph introspection
(`().__class__.__base__.__subclasses__()`, function `__globals__`) reaches
`_io.FileIO`/`posix`: restricted builtins cannot be the boundary. CPython
therefore needs an OS-confined interpreter process that holds no tool
authority.

### 2026-09-27 17:25 — Monty, controls, preregistration
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
