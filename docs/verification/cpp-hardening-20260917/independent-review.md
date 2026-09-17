## Review: durable-job transaction ownership slice (feat/cpp-job-transaction-owners vs fdd5aa7)

**Verdict: approve. No ownership regression found in the production diff. Two evidence gaps and one small fixture/doc wording issue, none blocking.**

**What I verified**

- Diff is exactly src/core/jobs.cpp (mechanical `.get()` borrows plus the loader/store/txn rewrite), the fixture, and one mutation anchor pair. No public header, ABI, seam, or allocator change.
- Input-bytes lifetime is safe. `jparse` reads with flags 0, so yyjson copies the input; releasing `data` before validating the doc is correct and matches the old code, which also freed `data` before using `doc`.
- `jobs_txn` member order is lock, dir, doc. Both the defaulted destructor and `reset()` release doc, then dir, then the lock. The `lock_descriptor` reset saves and restores errno, and `free()` on Darwin/glibc does not clobber it, so commit's returned `rc` is unaffected.
- Transfer semantics are unchanged. `yyjson_doc_mut_copy` is a deep copy, so returning the raw mutable doc while `tny::document` frees the immutable one at scope exit does not alias.
- All 7 mutation anchors match exactly once in current source. Deleted copy/move never conflicts with a use site; every `jobs_txn` is a stack local.
- Failed commit keeps durable state: `write_private` writes a temp, and on any open/write/fsync/rename fault it unlinks the temp and returns errno before `jobs_txn_end`. The fixture's `unchanged()` plus final `rmdir` confirm no leftover temp.
- Lock-probe fix is sound. The probe opens with the fixture TU's libc `open` and calls the real `flock`-based `tny_jobs_host_lock_try` from `jobs_host.o`; only `tny_resource_open/write/fsync/rename` inside `jobs_host.o` are faulted, so fault 1 cannot silence the probe. flock is per open-file-description, so a second open in the same process correctly reports BUSY.
- Independent run: built in a scratch build dir under review-scratch, ASan/UBSan, `test-runner-ownership` passed (exit 0), with the new line reporting 32 cycles, 5 allocation faults, 4 persistence faults.
- Probe mutants I built against the new code (scratch only, removed afterwards):

| Mutant | Result |
|---|---|
| `reset()` omits `lock_fd.reset()` | killed (line 269 assert) |
| `reset()` omits `doc.reset()` | killed (line 269 assert) |
| loader leaks immutable doc via `doc.release()` | **survived** |

**Findings**

1. **Evidence gap: no leak oracle for the loader's `tny::document` on macOS.** A mutant that leaks the immutable yyjson doc in `jobs_record_load` passes the whole fixture. Darwin ASan has no LeakSanitizer, and `json_malloc` does not go through the owned-allocation counters. The Linux `make leaks`/valgrind gate would catch this, but the slice's macOS evidence cannot claim leak-freedom for the loader. Minimal fix: in the fixture's alloc-fault loop, after `transaction.reset()`, assert `tny_alloc_test_owned_live()` is unchanged only covers C++ owners; the cheapest real oracle is to run `test-runner-ownership` once under Linux valgrind in the gate log, or add a test-only malloc/free live counter in `alloc.c` under `TNY_ALLOC_TESTING` and assert it is balanced across each `transaction_owners` cycle.
2. **Evidence gap: dir-string OOM path is not fault-injected.** `xstrdup` uses raw `malloc`, so the `!t->dir` branch in `jobs_txn_begin` is never exercised, and the doc's "every JSON allocation" wording is accurate only for JSON allocations. Pre-existing behavior in that branch also returns EINVAL with no error text for an OOM. Not a regression; suggest either `tny_alloc_strdup` for `t->dir` (in scope, one line) or a sentence in transaction-ownership.md stating the directory copy is outside the injected set.
3. **Wording: "C++ unwinding" evidence is fixture-frame only.** The `throw abandoned{}` unwinds through the fixture's frame, not through any jobs.cpp frame, which never throws. The claim is true for what the RAII members promise, but the doc should say "unwinding past the transaction owner" rather than implying the jobs module's own frames were unwound.

**Not defects, noted for the coordinator**

- Commit-path allocations (`now_iso8601`, `jm_set_*`, `jwrite_pretty`) are not fault-injected; `jm_set_*` silently no-ops on OOM, so an OOM commit could persist a record without a revision bump. Pre-existing and outside this slice.
- OOM inside `jparse` is still reported as "not a JSON object" rather than "out of memory". Pre-existing, unchanged.
- Adding one of the two killed probe mutants above to `runner_critical.py` would give the ownership code its own mutation anchor. Optional.

Scratch artifacts left under review-scratch: `build/`, `ownership.log`, `probe_mutants.py`. No source, worktree, or primary checkout modified.
