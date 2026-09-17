# Final review of #142: private C++ checkpoint ownership

I found no confirmed correctness regression in `checkpoint.cpp` against the pinned baseline. There is one open question that could be High if it fails, plus Medium test-oracle gaps.

This is a read-only review of the supplied text; I compiled and ran nothing.

## Confirmed defects

**1. Medium: recover's backend and identity-presence gates have no negative test.**
- **Anchor:** `recover()` first `check(...)` in `checkpoint.cpp`; `edge_cases()` reject list in the fixture.
- **Problem:** `restore()` now accepts backends 1 and 2. The only thing stopping a forged public snapshot from recovering as CURSOR or ACP is `jget_int(saved,"backend",-1) == TNY_BK_OPENAI` together with `resolved->backend == TNY_BK_OPENAI`. `backend` is a `public_key`, so without that clause it merges straight over the resolved OPENAI context. No `reject()` case covers it, so a mutant deleting either clause survives.
- **Same gap for identity:** absent or null `identity` is untested; only `"mismatch"` is. Dropping `saved_identity &&` leaves `strcmp(NULL, …)` unexercised.
- **Reproduce:**
  - Add `reject(full, d, "backend", "1")`.
  - Add `reject(full, d, "backend", "null")`.
  - Add `reject(full, d, "identity", NULL)`.
  - Add `reject(full, d, "identity", "null")`.
  - Add a case with `full->backend = TNY_BK_CURSOR` against an unmodified snapshot.
  - Add matching mutants in `checkpoint_ownership.py`.

**2. Medium-low: fixed-buffer overflow rejection is tested on one field of four.**
- **Anchor:** `restore_fixed`; `invalid(d, "ssh_port", "\"123456\"")`.
- **Problem:** `ws_hash`, `task_digest` and `instructions_digest` have no over-length case. If `check(!text || strlen(text) < N)` is deleted, those three silently truncate as the baseline did, and the suite still passes.
- **Reproduce:** add strings of 17, 41 and 17 characters as `invalid()` cases, plus one mutant.

**3. Low: the structural-immutability oracle compares padding.**
- **Anchor:** `tny_ctx before = *c; … memcmp(&before, c, sizeof before)` in `reject`, `sweep`, `recovery_routing` and `routing_edges`.
- **Problem:** `tny_ctx` has interior padding (bool runs before pointers, `char[6]`, `char[17]`). Struct assignment does not have to copy padding. The test passes today because compilers emit a `memcpy`, but it is not a guaranteed oracle. It would be an uninitialised read under MSan or an SRA-style optimisation.
- **Fix:** use `memcpy(&before, c, sizeof before)`.

**4. Low: `restore_number`'s signed branch hardcodes `INT_MIN`/`INT_MAX`.**
- **Problem:** the bounds should come from `numeric_limits<T>`. Only `int` is instantiated today, so nothing breaks yet.
- **Fix:** either `static_assert(std::is_same_v<T,int>)` in that branch or use `numeric_limits<T>`.

## Questions (not confirmed; the source needed was not supplied)

**Q1. Potentially High: what clears the sticky allocation flag in the runner/CLI process?**
- **Anchor:** `encode`, `restore`, `recover` and `identity` all gate on `allocation_ok()`.
- **Concern:**
  - `alloc.h` says CLI builds use libc for C allocations, but `jallocator()` and the C++ owners always go through `tny_alloc_*`.
  - Suppose a transient yyjson allocation failure sets the flag, and nothing in `runner.cpp` calls `tny_alloc_scope_begin` or `tny_alloc_scope_clear`.
  - Then every later `rn_checkpoint` returns NULL for the life of the process. The baseline never consulted the scope.
  - The fixture asserts this stickiness as a requirement, which is right for libtny API scopes but unverified for the long-lived runner.
  - Is the flag thread-local? It matters for the prewarm threads.
- **Verify:**
  - Grep for scope begin/clear on the runner paths.
  - Add a runner-level test that fails one allocation and then checkpoints successfully on the next request.

**Q2. Live contexts that carry duplicate routing headers.** If a TUI `/model` switch calls `tny_finish_builtin_profile` again without clearing, the saved identity hashes two routing headers. `finish_profile` now normalises to one, so recovery is refused. That fails closed and is arguably correct, but confirm it is intended and record it in ADR 0126.

**Q3. `enable_extensions` is now a hard failure.** I can find only OOM or an empty `tny_dir`/`cwd` as causes of a NULL return, so this looks fine. Confirm that no environmental failure, such as an unreadable directory, reaches `return false` in `discover`. From the excerpt, none does.

## Verified by reading

- **Schema:** field counts are 65 private and 55 public. The nullable set equals the baseline's 20. `public_key` equals the baseline `allowed` set plus `identity`.
- **Defaults:** missing and null values for bools, numbers, arrays and `max_tool_result_bytes` match the baseline.
- **Routing:** the `finish_profile` compaction and `memmove` are correct for the routing header at the first, middle and last positions and when it is absent. The array stays NULL-terminated on every throwing exit.
- **Ownership:** `restore_array` owns exactly the copied prefix on failure. The duplicate-key check is sound, because `yyjson_obj_get` returns the first match.
- **Exceptions:** all four facades catch everything, so no exception crosses a C frame.

## Limitations

- I did not see `alloc.c`, `secure_free`, `parser_ownership.py`, `collect_raw` or yyjson internals, and assumed their standard semantics.
- `throw` itself allocates outside the injected allocator, so true-OOM behaviour rests on the C++ runtime's emergency pool.
- These match the baseline and are not counted as defects:
  - `agent_argv` and `cursor_config` are not checkpointed, so an ACP round trip loses its argv.
  - Adjacent unchecked `yyjson_mut_obj_add_*` calls remain in `rn_checkpoint`.

## Verdict

Approve, conditional on Q1 being answered with evidence. The item 1 tests and mutants should land in this PR, because they guard the authority boundary that the new backend bounds widened. Items 2–4 can follow. The final-code leak run and the broad gates are still outstanding, as stated.
