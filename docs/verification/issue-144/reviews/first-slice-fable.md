# Independent first-slice review

Reviewer: Claude Fable, effort medium, read-only Read/Glob/Grep tools.

Verdict: accept the first slice, but fix F1–F3 before starting the pending/retained slice. The ownership is real, not a facade, and the wire behaviour is unchanged. One documented invariant is not enforced (a second `oa_request_prepare` on the same request frees the auth secret unwiped), one real-runtime OOM oracle silently changed its target, and the JSON view document is kept alive far longer than anything needs it.

I did not run any build, test, sanitizer, Nix evaluation or wasm build, so every conclusion below comes from reading source. The handoff's PASS rows are its own claims; I did not reproduce them.

I read:
- `AGENTS.md`, `contract.initial.md`, `ownership.md` and ADR 0127.
- `first-slice.diff`, `first-slice-summary.md` and `first-slice-evidence.md`.
- `request_owner.h`, `request_owner.cpp` and `tests/fixtures/native_request_ownership.cpp`.
- `openai.c`: both request builders, `start_post_mode`, cancel, destroy and the constructor.
- `util/ownership.hpp`, `json/ownership.hpp`, `util/alloc.c`, `net/http1.c`, `net/net_wasm.c`, `core/provider_extras.c` and the `config.c` auth defaults.
- The `tests/test_openai.c` request-OOM test.
- `tests/mutation/{runtime_critical,mutate}.py` by grep and excerpt only, around the `openai.c` anchors.
- `tests/integration/test_net_host_safety.py`, the secret-wipe check only.
- `tests/integration/test_fault_sweep_inventory.py`, by grep of test names only.
- The Makefile, `nix/source.nix`, and the CI and `nix/tests.nix` hunks in the diff.

I did not open `first-slice-build.log` or the other cache logs and helper scripts.

## Findings to fix before the next slice

**F1 (Medium): "prepare exactly once" is documented but not enforced.**
- `request_owner.h:30-31` states the contract, but `prepare()` (`request_owner.cpp:60-85`) never checks the `prepared` flag; only `oa_request_send` does (`:132`).
- On a second call, including a call after a failed first one, `oa_request_prepare` does `body.reset(body)` at `:99`, freeing the first body.
- `secret_header::build` then runs `bytes.reset(...)` at `:33`. That frees the previous auth allocation with plain `free`, because the wipe only happens in the destructor (`:22-24`), and `size` is overwritten.
- This is exactly the "release an unwiped copy" case the comment at `:14-15` and ADR 0127 lines 32-35 say cannot happen.
- Today's single caller never does this, so the defect is latent. The pending slice is where this API shape gets copied.
- Fix:
  - Adopt the incoming body into a local `c_string`.
  - If the request is already prepared, or `body`/`auth.bytes` is already set, drop the new body and return -2.
  - Alternatively, or additionally, have `build()` call `secure_zero` on the existing bytes before the reset.
- Add a fixture check that a second prepare returns -2, leaks nothing, and the wipe count stays at 1.

**F2 (Medium): the real-runtime request-OOM test now hits a different allocation, and the evidence overstates it.**
- `request_fault_control` in body mode (`tests/test_openai.c:1159-1171`, comment at `:1180-1182`) arms the first allocation after `http_open`.
- That allocation used to be the body buffer. It is now the `oa_request_new()` aggregate at `openai.c:1095`.
- The test should still pass, but it no longer reaches the body builder.
- No real-path test covers an OOM while the request already owns a live view:
  - builder failures after `take_view` (`openai.c:833`, `:961`);
  - a failure in the middle of `oa_request_prepare` (`:1124`);
  - a -2 from the reopen (`:1147`).
- The fixture sweep does cover those states inside the owner. It cannot show the contract's V03 requirement that an active-turn OOM emits exactly one ERROR/TURN_END pair, because it has no scheduler or event queue.
- The handoff line "Includes real C provider request-construction OOM" should say it injects at the request-handle allocation only.
- Fix:
  - Discover the allocation count between POST entry and the PROVIDER_REQUEST control callback.
  - Sweep every index on both wire formats.
  - For each index, assert one ERROR/TURN_END pair, zero allocations during settlement, no request submitted, and an unchanged session snapshot.
- Also correct the stale comment at `tests/test_openai.c:1180`.

**F3 (Medium-low): the provider-view document outlives every borrower.**
- After `build_request_*` returns, nothing reads the view. The request still holds it (`request_owner.cpp:51`) for the rest of the POST:
  - the blocking `nstream_write_all`;
  - the control callbacks;
  - the stale-connection reopen, which can block up to 15 s (`http1.c:78`).
- For a whole-session JSON tree plus the serialized body plus the transport's copy, that is the peak-memory regression the ADR defers measuring.
- Fix: call `view.reset()` at the start of `prepare()`. It is noexcept and allocates nothing. This removes the ADR tradeoff and the V08 measurement debt.
- Fixture line 131 then asserts something that is not a requirement; flip it to assert the view has been released.

**F4 (Low): null-pointer and status handling is inconsistent across the facade.**
- `oa_connection_get` and `oa_connection_drop` tolerate a NULL owner (`request_owner.cpp:120-125`). `oa_connection_open` (`:116`), `oa_request_send` (`:132`, which dereferences `request`), `oa_request_take_view`, `oa_request_prepare` and both `*_free` functions (`:106`, `:127`, which dereference `*ptr`) do not.
- If the connection owner were NULL, `oa_request_send` would return -1, a retryable I/O error. The stale-retry path at `openai.c:1147` would then crash in `oa_connection_open(NULL)`.
- An unprepared request also returns -1, which triggers a reopen and a second attempt for what is a programming error.
- `o->connection` cannot be NULL after construction (`openai.c:2665-2670`). Pick a single non-null contract, remove the partial NULL checks, and return -2 (or assert) for an unprepared request.

**F5 (Low): fixture robustness.**
- The fixture hard-codes header slots `headers[4..7]` (`native_request_ownership.cpp:51-62`).
- It fails if `TNY_PROVIDER_EXTRAS=0` is set in the environment (`provider_extras.c:75-79`). Add `unsetenv` at the start of `main`.
- The owned-object counter does not track the body, auth or add-on strings, because `c_string` uses a plain `free` deleter (`ownership.hpp:41-43, 70`).
- The Make target disables ASan leak detection on macOS (`Makefile:728`). A leaked consumed body on a prepare failure is therefore invisible to `make test-native-request-ownership` there. Only the sanitized Linux CI lane's LeakSanitizer or the manual `leaks` run would catch it. Say so in the evidence.

**F6 (Low): the three owner mutants run from a script in `~/.cache`.**
- `run-owner-mutants.py` is outside the repo, Make and Nix. V06 needs maintained mutants, so move it into `tests/mutation` before final review.
- An F1 mutant (remove the once-only guard) belongs there too.

## Verified correct

- **Ownership is real.** No raw `o->conn` remains in `openai.c` (grep), and `unique_ptr` with `http_close` is the only releaser. C frees one request handle on each exit (`openai.c:1134, 1174, 1209`; the handle is still NULL at the early `goto` on `:1096`). Previously it managed five resources across three exits. Every `return` after `:1095` frees the request.
- **Body and view transfer.** Both remaining `yyjson_mut_doc_free(view)` calls were removed, so there is no double free. A NULL body or an already-failed allocator still consumes the body.
- **Wire behaviour.** Header order and auth gating are identical to the old code. The header array holds at most 2 + 1 + 8 + 4 + 3 = 18 entries in 20 slots, so dropping the old `hn < 15` guard is safe; a `static_assert` or comment would document that.
- **Stale retry.** The path is copied once and no longer reads `http_prefix` at resend time. Body, auth and add-on addresses are stable across the reopen. Connection replacement closes the old connection first and returns -2 before any callback runs.
- **Allocation and exceptions.** Everything new uses the malloc/free family, and extras are malloc'd (`provider_extras.c:98-100`). Throwing entry points catch exceptions; the rest are noexcept. Destructors only free, wipe and call `http_close`.
- **Build integration.** The Make rule lists the production source file as a prerequisite with `-MMD`. Three CI lanes and `nix/tests.nix` name the new target. `nix/source.nix` already includes `../tests`. The `src/backends/openai/*.cpp` wildcard puts the owner in the shared source list (`SRC_SHARED`), which the wasm build also compiles, but I did not verify a wasm build.

## Missing oracles that matter

1. The real-runtime index sweep described in F2.
2. A second prepare, and a prepare after a failed prepare, as described in F1.
3. A loopback test that closes the keep-alive socket between turns. It should assert two byte-identical requests, `provider_attempt` incremented once, and the stop-at-second-control path.
4. A test where the control callback stops the request with a prepared request live. It should show the auth was wiped and nothing was sent through the real C path.

## Nonblocking observations

- If the response-control callback signals stop during the stale retry (`openai.c:1144-1150`), the code still reopens the connection, which can block for up to 15 s. This behaviour is unchanged from before. An `if (o->cancelled)` short-circuit would be a behaviour change, so treat it as a separate decision.
- The path string can allocate twice on libstdc++ (assign, then append). Reserving the full length once fixes that.
- `tny::required` lives in `json/ownership.hpp` but is used for a non-JSON allocation. It belongs in `util`.
- `oa_request_send` and its caller both check the allocator failure flag, which is redundant.

## Guidance for the pending permission/custom slice and retained buffers

- **Don't reuse the consume-then-prepare shape.**
  - Build the custom pending record completely first, copying its borrowed strings. That is the only step that can throw.
  - Then commit with a noexcept move of the parsed `tools_call` out of the permission record, and clear the permission record.
  - On OOM the permission record is untouched, nothing is replayed, and nothing is freed twice.
- **Keep generation and invalidation explicit.** The custom record's destructor must only release storage. `pending_custom_clear(o, true)` remains the one place that invalidates the async handle.
  - Add a mutant that moves invalidation into the destructor. It must fail a test where a late completion arrives after the transfer.
  - Add a mutant that skips clearing the permission record. It must fail a replay test.
- **Under settlement** (`oa_cancel`, `openai.c:2115-2147`), reset must allocate nothing and call no callbacks. Sweep each pending transition with the settlement-allocations counter asserted at 0.
- **`text`, `rawbody`, `toolcall_log` and `steer`.**
  - Wrap them in place as members of one turn owner inside `oa_impl`, not as per-buffer heap handles.
  - Preserve `if (!o->continuing) buf_clear(&o->text)` (`openai.c:1197`) exactly. Add a test where a retry after partial text keeps that text.
  - Returning a parked steer must stay an explicit operation, never something a destructor does.
- **Callback borrows.** Emitted event views point into `text` and `rawbody`. Owners must not shrink or free those buffers inside a callback. Keep the existing `parser_active` deferral in `oa_cancel` and add a test that cancels from inside an event callback, run under ASan.
- **Apply F1's rule to every new entry point.** If the API says it is called once, the code must reject a second call and the fixture must assert that.
