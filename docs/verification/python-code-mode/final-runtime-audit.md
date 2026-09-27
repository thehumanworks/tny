# Final runtime-selection / delivery audit — Python code mode

- Audited revision: `7c40a20704256c50480f3707841601c17528b56d` (HEAD, worktree clean; this report is under git-ignored `.agent/`, `.git/info/exclude:10`).
- Mode: read-only. No tracked file, commit, push, tag or publication. No model calls, no network, no delegation.
- Only new execution: 52 semantic probes run through the existing local production code-cell harness `build/python-runtime-bench/production` (sha256 `11a0ff98…`, identical to the binary hash recorded in `docs/verification/python-code-mode/data/unicode-production-original-replay.json`). `git diff 63502cb 7c40a20 -- src include scripts third_party` is empty (the Makefile delta only adds a bench target), so the probes reflect final production `code_python.c`.

## Verdict

**No release-blocking bug found in this narrow scope.** Points 1, 2 and 4 are confirmed. Point 3 holds, with one claim-precision issue about codecs that is not documented (non-blocking, and there is no measured agent cost). One remaining release gate is outside what local evidence can show: hosted CI at exactly 7c40a20.

## 1. Production code mode is genuine CPython

- `src/core/code_python.c:11` includes `<Python.h>`. Cells run through `Py_CompileStringExFlags(code, "<code>", Py_file_input, …)` and `PyEval_EvalCode` (`:1004`, `:1009`), which are CPython's own parser, compiler and evaluator. There is no transpiler and no dialect layer.
- Build rules: `Makefile:212-231` pins `third_party/cpython/VERSION` = 3.14.7 and `SHA256`, and `Makefile:555-560` builds `$(CPYTHON_LIB)` with `scripts/cpython_runtime.sh`. That script rejects any digest mismatch before extraction, then runs CPython's own `configure`/`make libpython3.14.a`. Nix passes `CPYTHON_TARBALL` with `CPYTHON_FETCH=0` (`nix/package.nix:72-73`, `nix/tests.nix:252-253`, `nix/cpython-source.nix`).
- The local artifact matches the pin: `build/deps/Python-3.14.7.tar.xz` sha256 = `third_party/cpython/SHA256` = `build/cpython-3.14.7-x86_64-linux-gnu-1395944601/stamp` = `3b48dac8…7f81`. Whether that digest matches python.org's metadata can't be checked offline (evidence boundary, not a gap).
- `git grep -iE 'micropython|pocketpy|monty'` over `src include nix Makefile scripts .github sdk site` returns nothing.
- Reduced packaging affects the stdlib, not the language. Only these things are removed: stdlib extension modules, docs, ensurepip, tests, mimalloc and remote debugging, plus the loading/interactive builtins (`code_python.c:872-874`). Probes passed for:
  - match, `except*`/ExceptionGroup, PEP 695 `type`/generic syntax, and `add_note`
  - f-string `=`, generators with `send`, coroutine `send`, `__slots__` classes and finalizers
  - bigints, float repr, `divmod`/`round`, insertion-ordered dicts, `zip(strict=)`
  - str `casefold`/`upper` (ß→SS), `int('١٢')`, Greek and NFKC identifiers, and `\N{…}` escapes (the `unicodedata` fix, `63502cb`)
  - utf-8, latin-1, ascii, utf-16 and utf-32 encode, plus the `errors=` handlers

## 2. MicroPython is only a benchmark candidate, reported honestly

- It exists only under `tests/bench/python_runtime/` (`micropython.c`, `micropython_json_compat.h`, `micropython/mpconfigport.h`) and `build/python-runtime-bench/`. It is absent from production and packaging.
- Original-corpus fix: `data/replay-micropython-json-compat.json` records `micropython_compat` at 36/36 programs and 108/108 variants. The report and ADR state plainly that the earlier 33/36 was a fixable `ensure_ascii` gap in the wrapper, "not an unavoidable language limitation".
- Weaker targeted replay: `data/heldout-replay-micropython-json-compat.json` has `by_arm` = 12/36 for CPython wording, 10/36 for PR197 wording and 14/36 for Monty wording, with 186/333 variant matches. `report.md` quotes those same figures and labels them "targeted compatibility replays, not new MicroPython-specific model generations". The report doesn't claim MicroPython can't be improved.
- The production CPython replays match the report: 36/36 and 108/108 original (`unicode-production-original-replay.json`), and 333/333 same outcomes over 111 held-out attempts (`unicode-production-heldout-replay.json`, `by_arm` 35/35/36 final). `evidence.md` correctly says this does not mean 333 successes.

## 3. Language vs. restricted authority vs. json facade

- The distinction is stated consistently:
  - ADR 0179 §"Bounded execution and Python surface" says full language, stdlib deliberately not exposed, json is a native facade, and `cls=`/decoder hooks are unsupported.
  - `report.md` §Production behavior says the same.
  - The model-facing prompt (`src/core/tools.c:631-653`) says "There is no import statement, open, eval, exec, filesystem, network or process access" and that json is prebound.
- Removed builtins are explicitly "not the security boundary"; seccomp and the macOS profile are the boundary. The probes confirm `import x`, `from __future__ import annotations` and `import warnings` all fail with `ImportError: __import__ not found`, as documented. The json facade probes behave as claimed: ensure_ascii, indent/sort_keys, bigint round-trip, and `JSONDecodeError` lineno/colno.
- **Finding (claim precision, non-blocking): the codec gap is undocumented.**
  - Only `encodings`, `encodings.aliases` and `encodings.utf_8` are frozen (`scripts/cpython_runtime.sh`, `code_python.c:28-33`).
  - So builtin method calls that need any other codec raise `LookupError: unknown encoding`. Probed: `b'\x80'.decode('cp1252')`, `.decode('utf-8-sig')`, `.decode('unicode_escape')`, `.encode('idna')`.
  - The prompt says "str/list/dict/set/tuple methods … behave as in CPython", and the ADR says Unicode strings "are CPython's". That overclaims for codec lookups inside builtin methods.
- **Evidence on agent efficiency:**
  - Across all 147 committed Python programs (36 original plus 111 held-out generations), there are zero `import`/`from` statements and zero `.encode`/`.decode` calls. Nested tools return `str`. So neither the import refusal nor the codec gap has any measured cost.
  - Follow-up (not a blocker): either document the frozen codec set, or freeze more `encodings.*` modules. That is cheap and fits the user's stance on size.
- Measured costs that are honestly reported:
  - Local cell latency is +11.3 to +35.7 ms per task (`cell-latency.log`, `evidence.md`).
  - Stripped binary goes from 1,401,904 to 6,283,208 B.
  - `ask --help` median is 5.56 ms, above the 5 ms target; this is reported, not suppressed.
  - Output tokens per solved task: 269.37 (CPython wording) vs 276.89 (old wording). The bootstrap interval spans zero, as the report says.
- Live smoke: the first attempt failed the effect oracle because of a model filtering error, even though every run_code/tool call succeeded. The one repair passed. Both are retained (`data/live-production-smoke*.json`, `evidence.md`), with no success-by-final-message shortcut.

## 4. Test and Lean receipts at the real revision

| Gate | Revision | Exit | Notes |
|---|---|---|---|
| `make -j3 test` (`run-full-test.sh`) | 7c40a20 | 0 | 612 unit tests / 47,455 assertions; 101 integration groups; `test_manifest_permissions` passes all pending rows |
| `make verify-code-policy` (Lean, corrected) | 7c40a20 | 0 | "All 38 mutants rejected at the expected stage" |
| `make verify-code-policy` (first attempt) | 092f7b5 | 2 | Toolchain setup failure: missing Clang 21 `libclang_rt.ubsan_standalone.a`. Retained, and disclosed in `evidence.md` |
| `make quality` (local) | 092f7b5 | 2 | GCC `-fanalyzer` fd-leak in `src/core/jobs.cpp:6206`. `jobs.cpp` is unchanged versus merge-base `88d343d`, so this predates the branch. Disclosed, not relabelled as a pass |

- Receipts are written atomically (temp file then rename) by the run scripts, and each records `git rev-parse HEAD` at start. The archive checks out: `sha256sum -c python-code-mode-local-verification.sha256` → OK, and the archived `full-test.log` is byte-identical to the local one (`3c1f73ec…`) with exit/revision 0 / 7c40a20.
- `receipt-snapshot.json` holds an earlier, still-growing 165,851-byte log hash. It says it is a snapshot, so the mismatch with the final 287,837-byte log is expected, not tampering.
- Log noise, not hidden failures:
  - The three tracebacks in `full-test.log` are BrokenPipe/ConnectionReset from `test_codex_chatgpt`'s mock server when clients disconnect; the group passed.
  - `test_execution_code_mode` skipped=1 is the wasm-only test (`skipUnless(WASM)`).
  - `build/tny --version` shows `-dirty` because the suite regenerated site metadata. `generated-site.diff` shows only version and size strings, which were restored.
- **Evidence boundaries:**
  - (a) Hosted CI at exactly 7c40a20 can't be checked locally. `.agent/ci-pr198/*` logs are from 18:04–18:54, before 7c40a20. The green hosted SDK/quality/platform/Lean status and "hosted Linux full job still completing" come only from `local-verification-comment.md`. That hosted Linux full job is the outstanding release gate.
  - (b) Local quality was not rerun at 7c40a20. The delta from 092f7b5 is one Python test plus docs/JSON, so Ruff formatting of `tests/integration/test_manifest_permissions.py` at the final revision is covered only by hosted quality.
  - (c) The committed `evidence.md` says final gate receipts "are appended during completion". At 7c40a20 they exist only in the ignored `.agent/completion/`.

## 5. Release-blocking bugs

None found in scope. Non-blocking items for the release owner:

1. Codec-lookup overclaim (§3). Document it, or freeze more `encodings.*` modules.
2. Stale doc path: ADR 0179 line 117 names `tests/bench/legacy_lua`, but the pinned test-only Lua is at `tests/bench/code_mode/lua_runtime/`.
3. Confirm the hosted Linux full suite at 7c40a20 before merge or release (§4a).

## Scope limits

This audit didn't re-run the suite, Lean, quality, replays or live inference. It didn't audit OS confinement, the rest of the parent/child protocol, or anything outside runtime selection and delivery evidence.

## Supervisor reconciliation after the read-only audit

The outstanding hosted status was checked through GitHub, not inferred from local
logs. CI run 36352172108, SDK run 36352172133 and language-proof run 36352172122
all completed successfully on exact revision 7c40a20 before it was fast-forwarded
onto main. PR #198 is merged and annotated tag v0.24.0 points to that same commit.
The tag release workflow is separately observed; creation of the tag alone is
not represented as publication of its release assets.

The codec limitation and stale historical-Lua path are corrected in the current
documentation. This closeout changes documentation/evidence only, not the tested
CPython implementation or the immutable v0.24.0 source revision.
