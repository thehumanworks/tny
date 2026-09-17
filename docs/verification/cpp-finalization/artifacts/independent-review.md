# Independent read-only review: C++ ownership continuation

Branch `fix/cpp-ownership-finalization` (staged tree) vs `origin/main` fdd5aa7.
Reviewer: fresh session, not the author. No source edits, no commits, no
branch changes, no live provider calls. Probes ran foreground from
`/Users/tomas/.cache/tny-cpp-finish-20260917/env.sh`; scratch output lives in
`review/probe/` only. `git status` after all probes: staged set unchanged, no
new untracked files in the primary tree.

## Verdict

**Approve the production change; the mutation gate as staged cannot pass.**

The `jobs_txn` value-owner conversion, the record loader/store RAII locals,
the always-inline factories and the build/Nix fixes are correct as integrated.
I found no memory, cleanup-order or boundary defect in the modified lines. Two
of the three new runtime mutants in `tests/mutation/runtime_critical.py` are
flawed test oracles, so `make test-runtime-mutation` fails on its own
inventory before any behavior is judged. That is inside this change's scope
(F3/F4) and must be fixed before the F4/F5 claims can be made.

## Confirmed findings

### 1. Medium — mutant anchor is not unique; harness aborts

- File: `tests/mutation/runtime_critical.py`, entry `lost-copy-failure-callback-oom` (lines 29–36).
- Trigger: the anchor `if (tny_alloc_scope_failed()) e->oom_pending = true;`
  occurs twice in `src/core/runtime.c` (queue_event line 189 and after_backend
  line 1367). The harness asserts `original.count(old) == 1` and raises
  before compiling, so the whole runtime mutation run stops as an
  infrastructure error, not a kill.
- Evidence: `python3` count over the staged source returns 2; every other
  runner/runtime anchor returns 1.
- Minimal fix: widen the anchor to the copy-failure site, e.g. old =
  `"        if (tny_alloc_scope_failed()) e->oom_pending = true;\n        else e->overflow_pending = true;"`
  and new = same text with `= false`. Probe result with that anchor: the
  mutant is killed by `runtime_callback_oom_survives_allocator_scope_reset`
  (exit 1, `FAIL` at tests/test_runtime.c:493).

### 2. Medium — `forget-provider-scope-oom` mutant survives its assigned test

- File: `tests/mutation/runtime_critical.py`, entry `forget-provider-scope-oom` (lines 13–20).
- Trigger: it mutates the early latch in `queue_event` (runtime.c:177–180)
  but is judged by `runtime_callback_oom_survives_allocator_scope_reset`. In
  that test the scope only fails *inside* `event_copy`, so the early latch is
  never reached and the mutant passes: probe shows exit 0, 1 test passed,
  `killed False`. The harness's final `assert all(item["killed"])` therefore
  fails even after finding 1 is fixed.
- The same mutation with the pre-failed test
  (`lost-prefailed-callback-oom`) is killed (exit 1, `FAIL` at
  test_runtime.c:493). So each latch does have one killing mutant; the extra
  entry is a duplicate with the wrong oracle.
- Minimal fix: delete the `forget-provider-scope-oom` entry (or retarget it to
  `runtime_failed_scope_callback_oom_survives_scope_reset`, which makes it
  identical to `lost-prefailed-callback-oom`). ADR 0124's "each latch has its
  own intentional mutant" then holds with exactly two runtime marker mutants.

### 3. Low — duplicated Nix input line and tautological test

- File: `nix/source.nix` lines 55–56 add `../nix/package.nix` twice with two
  different comments; `tests/integration/test_nix_ci_matrix.py` lines 99–100
  assert the same string twice. Functionally harmless (fileset union), but it
  reads as a merge artifact and the second assertion checks nothing new.
- Minimal fix: keep one `../nix/package.nix` line and one assertion.

### 4. Low — directory-copy OOM returns `EINVAL` with no message

- File: `src/core/jobs.cpp` `jobs_txn_begin`, lines 352–356.
- Trigger: `tny_alloc_strdup(dir)` fails, `jobs_record_load` still runs, then
  `jobs_txn_end` and `return EINVAL` without `safe_err`. Callers that surface
  `err` (cancel/rm/retry) report an empty or stale message. Baseline `xstrdup`
  behaved the same, and the new admission sweep now proves cleanup on this
  path, so this is a message/return-code nit, not a leak.
- Minimal fix: `if (!t->dir) { safe_err(err, errlen, "out of memory"); jobs_txn_end(t); return ENOMEM; }`
  before loading the record.

## Reviewed and found correct

- `jobs_txn`: member order `lock_fd, dir, doc` gives destruction doc → dir →
  lock; `reset()` mirrors it and is idempotent; copy/move deleted with
  compile-time traits in the fixture; `= default` destructor. `jobs_txn_begin`
  calls `t->reset()` first, so reuse after a committed or abandoned
  transaction is safe. Failing admission ends the transaction and releases the
  lock (probe: 6 injected allocation faults, lock FREE and descriptor count
  unchanged after each).
- Record loader: `tny::c_string` and `tny::document` locals; `data.reset()`
  after `jparse` is valid because `yyjson_read_opts` with flags 0 copies its
  input; the immutable `doc` outlives `yyjson_doc_mut_copy`. Store: `json`
  and `path` owners, single return. All owners free with `std::free`, matching
  `path_join`/`buf_detach`, `tny_alloc_malloc`, and `jallocator()`'s
  `json_free`. No allocation-family mismatch.
- Every `t.doc` use in project/cancel/rm/retry/supervise/submit_finish_failed
  is a `.get()` borrow with an explicit `jobs_txn_end`/`commit` on each path.
- `[[gnu::always_inline]] inline` on `make_owned`, `parse`, `make_document`:
  semantics unchanged (bad_alloc on failure, storage released by
  `unique_ptr` on constructor throw). Attribute is accepted by GCC, Clang,
  MinGW GCC and emcc; no warning suppressed. `analyze-cpp-gcc` with g++-14 on
  macOS: 8 files clean; `test_cpp_analyzer.py` passes factories and rejects
  both uninitialized-read and use-after-free controls.
- CI change dropping `ANALYZER_CXX=g++-14` is correct: `cxx_driver` maps
  `gcc-14` → `g++-14`.
- `LEAK_SUITES` now lazily evaluated and guarded by `$(wildcard)`; the new
  `test_release_archive_does_not_read_missing_unit_test_inventory` passes and
  the whole `test_cpp_build.py` suite passes locally (11 tests, 1 wasm skip).
- Runner mutants `transaction-retains-document` and `transaction-retains-lock`
  both killed (abort on the fixture's post-reset assertion, line 269).
- `test_runtime.c`: the prefailed variant asserts the callback allocates
  nothing after the scope has already failed and that settlement allocates
  nothing; fault-sanitized runtime suite passes 40/40.
- `mutate.py` baseline now builds `debug release`; inventory test asserts it.
  Note the per-mutant rebuild is still `make debug` only, so a mutant in CLI
  code exercised only via the release binary is not observed. Pre-existing
  behavior; not introduced here.
- Size guardrail: stripped Release `tny` on this tree is 1,189,464 bytes,
  far below 6,000,000; the only C++ runtime dependency is `libc++.1.dylib`.
- Changed C/C++/Python files pass clang-format, ruff check and ruff format.
  Clang path-sensitive gate (`make analyze-cpp`) processed all 8 C++ units
  without diagnostics.

## Verification gaps (outstanding evidence, not code defects)

- `make test-runtime-mutation` end to end was not run here and cannot pass
  as staged (findings 1–2). Re-run after fixing; `test_fault_sweep_inventory`
  should also be re-checked if the runtime mutant list is used as inventory.
- Linux Valgrind proof for the raw yyjson and directory allocations in
  `jobs_txn` (ADR 0125 verification) is still planned, not recorded.
  macOS ASan runs here had `detect_leaks=0`.
- Hosted Linux/Windows/wasm/Nix lanes and the same-host performance
  comparison (F5/F6) are not yet claimed; `evidence.md` is "in progress".
- `make release` here reused up-to-date objects from an earlier build of the
  same tree, so the size number is for the staged sources but not a
  from-clean rebuild.
