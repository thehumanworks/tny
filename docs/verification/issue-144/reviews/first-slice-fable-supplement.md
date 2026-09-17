# Supplemental independent first-slice review

Read-only Claude Fable review; findings require disposition, not automatic acceptance.

## Verdict

**Conditional pass.** The ownership design is sound, but F1–F3 should be fixed before the pending/retained slice. F5 and F6 are evidence gaps to close or disclose.

I ran nothing; everything below is from reading the code, and test results are the implementer's claims.

- **Unique ownership is real.** `oa_impl.conn` is gone and every C use goes through `oa_connection_get`. The request is a single aggregate whose members own their storage, with no manual cleanup hidden behind the facade.
- **Transfers are correct on every early return.** Body, view and add-on transfers hold on each exit path of `start_post_mode` (`openai.c:1096-1210`). No `goto request_oom` precedes the declaration of `request`, so the handle is never read uninitialised.
- **Wire behaviour is unchanged.** Header order and the endpoint path match the old code.
- **Destructors are release-only.** They make no RPC, callback or wait.
- **Exceptions are contained.** They stop in `oa_request_new`, `oa_connection_new` and `oa_request_prepare`; the makefile compiles C++ with `-fexceptions` for native and wasm.
- **Allocation is injectable.** All new allocations use `tny_alloc_malloc`, and everything is released with `free`, the same family `tny_alloc_malloc` uses.

## Findings to fix before the next slice

**F1 (Medium): a NULL auth name or prefix now crashes the process.**
- `secret_header::build` calls `strlen` on each part unconditionally (`request_owner.cpp:26-31`, called at `:67`).
- `tny_checkpoint_context_restore` sets `auth_header_name` and `auth_header_prefix` to NULL when the field is absent, and unlike `api_key` does not check the `xstrdup` result (`checkpoint.c:143-148`).
- The checkpoint code itself treats NULL as legal (`checkpoint.c:15`, `:379`).
- The old `buf_appendf("%s: %s%s")` produced `(null)` on supported libcs; the new code segfaults, and C++ exception handling does not catch a segfault.
- Fix: default to `"Authorization"` and `"Bearer "`, or fail cleanly with -2, and add a fixture case.

**F2 (Medium): the "prepare exactly once" rule is documented but not enforced, and a second call breaks the wipe invariant.**
- A second `oa_request_prepare` call reaches `bytes.reset(...)` at `request_owner.cpp:33`, which frees the first secret without wiping it. The comment at `:14-15` claims this cannot happen.
- If that second call then throws at `:76`, `prepared` is still true from the first call. The add-on strings have already been reset at `:75`, so the header array holds pointers to freed memory, and `oa_request_send` would write them.
- Fix: reject a second prepare in `oa_request_prepare`, still consuming the incoming body, or clear `prepared` and wipe before `reset`. Add a fixture case.

**F3 (Medium): precondition failures are reported as a retryable I/O error.**
- `oa_request_send` returns -1 when there is no connection or the request is unprepared (`request_owner.cpp:132`).
- `start_post_mode` treats any nonzero return as a stale keep-alive write (`openai.c:1141-1163`). It fires the `PROVIDER_RESPONSE` callback, reopens the connection and sends the POST.
- If the `PROVIDER_REQUEST` callback at `:1125` re-entrantly cancels, `oa_cancel` disconnects (`openai.c:2134`, `:2183`). The old code would have dereferenced NULL. The new code reconnects and sends a cancelled turn.
- `o->cancelled` is not checked between `:1138` and `:1147`.
- This does not extend the documented callback contract, but it should fail closed. Fix: return a distinct status for a precondition failure, and/or check `o->cancelled` before sending or reopening.

**F4 (Low–Medium): `oa_connection_open` can return -2 while still holding a live connection.**
- When `http_open` succeeds but the allocator's failure flag is already set, the function returns -2 and keeps the connection (`request_owner.cpp:117-118`).
- `ownership.md` says no connection survives a failed reopen, and the first-open caller returns -2 without dropping it (`openai.c:1079-1088`).
- The fixture cannot reach this state, because its fake `http_open` returns NULL on any failure.
- Fix: `if (tny_alloc_scope_failed()) { owner->connection.reset(); return -2; }`.

## Evidence and integration gaps

**F5 (Medium): the leak oracle is weaker than the documents claim.**
- `tny_alloc_test_owned_live` counts only objects from the `tny::allocator` and `make_owned` paths (`ownership.hpp:29`, `:66`). Those are the two aggregates and `path`.
- It does not count the body, auth, add-ons, the JSON view, or the fake connection. The fake connection is covered by `live_connections`.
- The makefile disables leak detection on Darwin (`Makefile:728`), and the musl and Windows lanes run with `SANITIZE=0`.
- So the claim that prepare consumes the body on failure is checked only on the Linux ASan lane and in Nix. The `leaks --atExit` run was manual and is not a make target.
- None of the three mutants is an omitted body release. Add an accounting deleter or an interposed live count for the `free`-owned members, plus that mutant.

**F6 (Medium): the new CI lanes are unverified.**
- `ci.yml:237` and `:290` add `test-native-request-ownership SANITIZE=0` to the musl-static and Windows lanes.
- The target builds from `OWNER_OBJ_ROOT`, meaning `fault-pic` objects compiled with `-fPIC` and release flags, and links with `DBG_LDFLAGS`. No existing target on those lanes uses that object graph; `test-runner-ownership` uses `DBG_*`.
- It has not been run on either lane. Inspect the remote CI run, or limit the new target to the unix and Nix lanes.
- The mutant runner exists only under `~/.cache`. That is acceptable for this slice, but V06 requires it in the repo.

**F7 (Low): the view document is held longer than anything needs it.**
- Nothing borrows the view after `build_request_*` returns; the body is a detached buffer.
- The view is nonetheless kept through a blocking write and possibly a 15 s reconnect plus TLS handshake (`http1.c:78`).
- Fixture line 131 asserts this retention as if it were required.
- `oa_request_take_view(request, NULL)` at the end of each builder already releases it. That also removes the peak-memory question the ADR raises.

## Oracles still missing

- **Write-side stale reopen in the real `start_post_mode`.** No test checks the resend, a reopen returning -1, or a stop on the second attempt. `mock_openai.py`'s `MOCK_DROP_REUSED_ONCE` exercises only the read side.
- **A send returning -2.** The fixture's fake `http_request` never allocates, so allocation failure inside the real one goes unexercised.
- **Request shapes.** Missing cases: NULL `api_key`, the F1 NULL fields, and the maximum of 8 profile headers plus 4 add-ons plus 3 routing headers. That maximum fills 19 of the 20 header slots (`request_owner.cpp:56`) with no assertion guarding the limit. The old cap of 15 on add-ons was dropped.
- **Misuse.** Missing cases: a second prepare, send before prepare, and send after the connection is dropped.
- **Chat wire through the owner.** The whitebox fake proves address stability and cleanup of the aggregate. As the evidence file itself concedes, it does not prove active-turn OOM settlement or event ordering.

## Guidance for the pending and retained-buffer slice

- **Do not reuse the consume-even-on-failure prepare pattern.** Its failure mode is "request gone". For permission-to-custom transfer, build the complete custom record from owned copies first. Then transfer `tools_call` with a nothrow move as the last step, so a failure leaves the permission record intact.
- **Keep invalidation explicit.** `tools_call_invalidate_async` must never be called from a destructor. A moved-from record must destruct as a no-op; otherwise clearing the permission record would invalidate the generation just transferred (`openai.c:2159-2164`).
- **Sweep every allocation index of that transition.** Assert exactly one `complete_tool` call per tool id, with no replay and no double completion, and an unchanged `tool_index`.
- **Do not replace `text`, `rawbody` or `steer` with a growable `tny::string` without checking the consequences.** `oa_cancel`'s settlement path frees them without allocating (`:2137-2141`), and continuation borrows `o->text.data` across the builder (`:839`). Wrap the existing `buf_t` in a noexcept owner instead.
- **`steer` must come back through the explicit terminal path.** It must not be dropped by destruction.
- **Enforce preconditions in the API, not in comments.** See F2 and F3.

## Non-blocking observations

- Assigning then appending to `path` costs up to two allocations; reserve once. The allocation-index count also depends on the platform's small-string threshold. The fixture's long prefix hides that, but the ADR's "9 indices" figure does not hold across standard libraries.
- The `nix/source.nix` change is only a comment. It is correct, because `../tests` is already included wholesale.
- The constructor failure path (`openai.c:2665-2669`) and `oa_destroy` are correct.
- The `#define secure_zero` trick in the fixture depends on include-guard ordering. A comment saying so would help.
- The full `test_openai.py` failure at line 750 (`list_files` and `glob_files` report `unknown tool`) is probably environmental. Memory from earlier sessions records that the mise `python3` shim re-injects `TNY_TOOLS`, which would explain why `env -u TNY_TOOLS` did not help. I did not verify this; try running with the real interpreter path.

## What I inspected and did not run

- **Read:**
  - AGENTS.md as provided, plus the contract, ownership inventory, ADR 0127, the evidence file and the handoff summary
  - the full diff, `request_owner.h`, `request_owner.cpp` and the fixture
  - `util/ownership.hpp` and `json/ownership.hpp`
  - `openai.c`: the start-post, cancel and constructor regions
  - `http1.c`: open and request
  - the counters in `alloc.c`, and the context restore in `checkpoint.c`
  - Makefile, CI and Nix references
- **Not done:**
  - I ran no build, test, mutant or CI job.
  - I did not open `first-slice-build.log` or the other cached logs.
  - I did not audit `net_wasm.c` beyond its function signatures, or the provider-faults suite internals.
