# Independent final review of initial candidate

Read-only Claude Fable review; findings require disposition, not automatic acceptance.

## Verdict: conditional pass. Approved as a code review subject to obligations 1–4 below; this is not issue completion.

I ran no tests. This is a read-only review based on the sources and the named logs.

**Inspected**
- Docs: AGENTS.md, `contract.initial.md`, `ownership.md`, ADR 0127, `reviews.md`, `implementation-summary.md`.
- `final-source.diff`: the CI, Makefile, Nix, `openai.c`, `tools.{c,h}`, `mutate.py` and docs hunks.
- Sources: `request_owner.{cpp,h}`, `turn_owner.{cpp,h}`, `responses.cpp`, the live `openai.c` sections, `tools.c`, `custom_tools.cpp`, `runtime.c` cancel paths, `skills.c`, `util.c`.
- Tests: `native_request_ownership.cpp`, `native_ownership.py`, `tests/test_openai.c:1146-1900`.
- Logs: `native-focused-{sanitize,nosan}.log` and the nosan mutant run logs.
- Not read in full: `first-review*.log` (I relied on `reviews.md` plus my own recheck), `bench_requests.*`, `evidence.md` beyond the diff.

**First-review rechecks.** I confirmed each of these in the code and saw them pass in the logs:
- Prepare-once latch before any work; body consumed on every outcome (`request_owner.cpp:123-131`).
- Auth allocated once at final size and wiped in the destructor, including after a failed prepare (sweep oracle at fixture `:168`).
- NULL auth name/prefix fall back to the config defaults (`request_owner.cpp:81-82`).
- Provider view released at last use (`openai.c` chat/rsp builders).
- A failed or sticky-OOM reopen leaves the connection owner empty (`request_owner.cpp:146-149`).
- Unprepared send or empty connection returns -2.
- Request-construction and pending allocation ranges are discovered, not hardcoded.
- Admit copies all metadata before moving the call, and the source is preserved on failure (`turn_owner.cpp:65-71`).
- `tools_call_free` behaves as before for C callers; its only new callers are via `oa_pending_reset`.
- Headers are bounded at 19 slots plus the terminator.
- Public headers and ABI are untouched.
- New `.cpp` files are picked up by the `CPP_SRC` wildcard, so the wasm build includes them, and they compile with `-fexceptions`.

### Blocking obligations

**1. The `tools_call_release_storage` precondition is enforced only by `assert`, and two "behavioral" mutant kills depend on that assert.**
- Evidence: `turn_owner.cpp:6` asserts no live lease before `tools.c:834-846`, which `memset`s a live `custom_call` away with no detach or free.
- The nosan logs for `cancel-authority.log` and `pending-lifetime.log` both die at `Assertion failed: (!pending->call.custom_call)… turn_owner.cpp, line 6`. No test oracle fired.
- Scenario: any build with `NDEBUG` defined (the Makefile currently does not define it). A transition that misses invalidation then leaks the lease handle, and its registry slot stays active, so the late completion is accepted instead of rejected.
- Fix: replace the `assert` with an always-on fail-closed check (`if (custom_call) abort();` or similar) and document it in `tools.h`. Do not invalidate inside the destructor; V02 forbids that.
- Test oracle: the two mutants must be killed by test assertions. Candidates are the late-completion `BAD_STATE` check and the `owned_live == baseline` check in `native_pending_lifecycle…`. Prove this by running those mutants with the assert stubbed in the mutant copy.
- Until this is done, "nine behavioral-oracle kills" is overstated; seven are shown.

**2. Reentrant `cancel()` from the control callback at the request edge produces two TURN_END events.**
- Evidence: `openai.c:1046`, `:1062`, `:1077`, `:1095` call `emit_turn_end` when `o->cancelled` is set.
- `oa_cancel` (`:2009-2091`) has already emitted TURN_END and set the state to idle by the time the callback returns, and `emit_turn_end` (`:226`) has no idempotence guard.
- Scenario: a direct backend consumer is in the retry-wait state and its control callback, at the provider-request edge, calls `backend->cancel()`. It receives TURN_END twice.
- The baseline was worse here: it sent on a closed connection. The runtime is safe because it defers cancels (`runtime.c:1382-1385`).
- None of the new `|| o->cancelled` branches has a test; `native_replay_control` only sets `response->stop`.
- Fix, choose one:
  - (a) A turn-open latch set in `oa_send` or restore, cleared in `emit_turn_end`, which returns early when the latch is clear.
  - (b) State in `openai.h` that control callbacks must use `response->stop` and must not call `cancel`. Then only emit the terminal in these branches when the state is not idle.
- Test oracle: a control callback that cancels on first send and on the stale second edge, asserting exactly one TURN_END and zero bytes sent.

**3. The CLI `skills_discover` OOM crash means V05 is not satisfied as written.**
- Evidence: `skills.c:104-111` calls `strlen(path_home())` and `strcmp(cur, …)` on the result of `xstrdup`, which returns NULL on OOM (`util.c:130-137`).
- This is reachable from the request builder (`openai.c:653-655`) whenever `library_mode` is false, which is the shipped CLI and session-runner configuration.
- V05 says "request creation" with no embedding qualifier, and the sweep found this crash and then sidestepped it with `library_mode=true`.
- The file is unchanged from base, so it is not a regression and I agree no skills rewrite belongs in this change. `reviews.md` and the summary correctly claim no baseline reproduction.
- The scope claims are not limited to embedding: the `evidence.md` V05 row and the ADR 0127 "discovers request… ranges" sentence carry no such qualifier.
- Required, choose one:
  - (a) A roughly 4-line guard (`if (!home || !cur) { free both; *count = 0; return NULL; }`) plus one CLI-mode request-construction sweep asserting the reserved ERROR/TURN_END pair. This is my preference: it is small, request-adjacent, and makes V05 true for the shipped path.
  - (b) Reword the V05 and ADR claims to "embedding configuration only; the CLI prompt helpers (skills, image capabilities, speech) are not swept" and file a follow-up issue citing `skills.c:105`.
- Embedding-only fault coverage must not be presented as native CLI OOM safety.

**4. Evidence inaccuracy.** The docs say each runtime gate ran 18,763 assertions. `native-focused-nosan.log:31` shows 18,785; only the sanitizer run shows 18,763. Correct the figure, or it contradicts the source-bound claim.

### Non-blocking findings

- **Wasted work on a cancelled retry.** After `failed.stop` sets cancelled, `openai.c:2200-2203` still calls `start_post_mode`, which bumps the attempt count, builds the request and fires the provider-request control for a POST that is never sent. Check `o->cancelled` at the top of `start_post_mode`.
- **Misclassified send status.** If `oa_request_send` returns -2 without the allocator latch set, `openai.c:1099` turns it into a -1 "provider request failed", and the retry-wait and stall callers (`:2163`, `:2206`) will retry it. It is unreachable today, but it contradicts the header's "non-retryable" wording. Map it to -2, or fix the comment.
- **Weak view-release mutant.** It removes only the defensive reset inside prepare. Removing the builders' `oa_request_take_view(request, NULL)` calls, which are the actual first-slice lifetime fix, would survive. Add an oracle, for example asserting the view is NULL at the provider-request control edge.
- **Missing mutants.** None covers retention of the continuation text (`if (!cont) buf_clear`), the steer transfer, or the consumed `tool_index++` on cancel.
- **Include hygiene.** `turn_owner.h:6-8` includes `core/tools.h` inside `extern "C"`; move the include above the block.
- **Mutant-runner environment.** `native_ownership.py:105` inherits the full user environment. Use an allowlist, or at least remove `TNY_TOOLS` and `TNY_PROVIDER_EXTRAS`.
- **Design.** The inline C-layout aggregates are honest but thin.
  - C mutates the fields directly, so RAII only covers final destruction.
  - Failure atomicity comes from `oa_pending_admit` and the request aggregate, and both are sound.
  - A heavier wrapper would not improve safety. The improvement that matters is obligation 1: make the one unenforced precondition always-on.
- **Pre-existing, not required for #144.**
  - Reentrant cancel from `complete_tool`, from a `note_repairs` event, or from the steer-rejected callback can free borrowed pending, steer or connection storage while it is in use (`openai.c:1014`, `:254`, `:1383`).
  - V01 only proves the decode-callback case via `parser_active`. Record these in `ownership.md` as unowned cases.

### Evidence gaps (pending, not code defects)

- **Leak proof is incomplete.**
  - `leaks --atExit` stalled on this host.
  - Darwin ASan runs with `detect_leaks=0`.
  - The owned-live counter excludes plain C and JSON allocations.
  - Do not claim leaks are absent until Linux LSan/valgrind and `test-native-leaks` actually pass.
- **Not run on the frozen source.** Full `make test`, `make quality`, `test-libtny-fault`, the `test_openai.py`, background and interrupt integration suites, wasm, Nix, and the hosted musl and Windows lanes. The real-socket tests skip on Windows. No benchmark, size or RSS numbers exist.
- **Invalidated results.** Any source change made for obligations 1–3 invalidates the `b18b0d5e…`-bound focused results; they would need re-running.
