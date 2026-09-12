# Runner quiescence — independent static lifecycle review

The proposed ordering correction is approved in principle. Source inspection identifies an actual ownership gap; no runtime reproduction, interposer, injected hook, or mutation program was used in this review. The reported hosted failure is external evidence, not a reproduction claimed here.

## Minimal finalization ordering

`rn_finalize` saves terminal status at runner.c:610 and releases the once-mode writer lock at614. The runner subsequently calls engine session-end hooks, frees the engine/backend, saves session.json again at1351, and unlinks the shared socket pathname at1355. A new owner admitted during that interval can therefore be overwritten by the old final save or lose its newly bound socket to the old unlink.

Keep writer ownership across the entire once-mode teardown. Remove the release from rn_finalize, which should finalize the turn and publish turn_end but not terminate the runner's ownership. Also remove rn_turn_err's serve-mode release: serve already promises a lifetime writer, and a rejected/malformed turn or prepare/start failure must not silently give its live session/backend to a competing writer.

The final boundary should be:

1. Resolve any active turn through the existing hard-cancel/finalize path.
2. End the engine session, run its existing end hooks, preserve/free the engine and backend. Finish any shutdown that can still write session state or task.log. In particular, if MCP shutdown can emit diagnostics, perform it before the final stderr drain/stdio flush rather than after bye.
3. Perform the final session save under ownership, then drain stderr and flush task.log.
4. Close the listener and unlink this runner's socket while still owning the session. No later cleanup may unlink that pathname or save session state.
5. Release the writer lock. Only then broadcast bye, finish the existing bounded client flush/drop, free inert memory and exit.

After step5, the old runner must perform no session-directory/task-log mutation and no live engine callback. Existing client dropping is inert once engine is null and turn/question state is resolved. Do not wait for a reader to consume bye while holding the writer unnecessarily. Existing serve turns keep their lock between turns; bye remains the quiescence marker for final exit. Preserve existing turn_end content and error/status semantics.

The parent-side release in cmd_ask after successful fork is correct and must remain: it closes the parent's copy of the inherited flock description while the runner retains its copy. Keeping both copies would pin ownership after child loss. session_lock_release closes the descriptor; it does not issue LOCK_UN on the shared description.

## Resume snapshot and completion distinction

`cmd_ask` opens and reconciles a resumed session before acquiring its writer lock. A successful acquire proves exclusion now, but does not prove that its earlier loaded document includes the preceding owner's final save. Refresh the resumed session under the already-held lock before provider preparation/start, and reconcile the fresh task snapshot. Transfer the owned descriptor to the fresh private session state before disposing the old object; never close/reacquire the lock between refresh and use. If reload/reconcile fails, refuse without saving the stale document. Use the resolved session ID, not another lookup of “last”. Preserve existing preflight task validation before a destructive steer/stop, then validate the fresh state again under ownership. New unsaved sessions should not be reloaded from a nonexistent file.

`test_client_sigkill_mid_turn_survives` currently polls raw session.json until status is terminal and immediately performs one resume. Terminal turn data can become durable before final runner quiescence. Retaining ownership correctly can turn that old race into a legitimate transient busy refusal. ADR0053 explicitly identifies the flock as liveness; terminal JSON and turn_end are not substitutes for that condition. The ordinary regression should wait for actual writer release (and verify terminal data), or a separately chosen CLI design may boundedly wait only for a terminal runner's cleanup. Do not merely enlarge arbitrary sleeps or assert that moving one unlock makes raw done atomic with lock release. A still-active turn must retain the existing immediate-busy behavior.

## Early-error ownership

Prepare failure, initial running-save failure, engine-start failure, malformed/empty turn, orphaned launcher before a turn, poll failure and explicit end must all converge on the same owned final cleanup. A serve error retains ownership and may accept another turn. A once error records failure and exits only after owned cleanup.

An actual failure to acquire ownership is different: it must not call rn_finalize, write a pid/task log, save the session during final cleanup, or remove another owner's socket. Current rn_turn_begin sends a failed acquire to rn_turn_err (which finalizes in once mode), and serve startup ignores its acquire result. Once callers normally already own the inherited lock, so the latter paths are not necessary to explain the reported once-mode race; they remain concrete startup hazards if this change claims lifecycle-wide ownership.

For full startup correctness, ownership must precede listener binding and all child storage writes. The TUI caller currently only probes session_is_running before spawn; that is not acquisition. A bounded internal solution is for tny_runner_spawn to acquire/verify ownership before binding, remembering whether it acquired a new parent descriptor itself. On failure it releases only that newly acquired descriptor; on successful fork the parent drops that new copy while the child inherits it. Preserve caller-owned once-mode descriptors/fallback behavior. This avoids both the ignored child acquire and socket hijack between a TUI probe and bind. No public ABI change is necessary. If primary limits this slice to the once-mode ordering defect, explicitly retain these startup hazards as separate findings rather than call every error path safe.

## Verification boundary

Use existing isolation, background/resume/steer, runner serve/malformed-turn/prepare-error, task snapshot and bye lifecycle regressions after the source correction. Relevant assertions are: no old save or socket unlink after a new owner can acquire; bye follows final storage mutation and lock release; serve errors do not drop the lifetime lock; failed acquisition performs no writer mutation; resumed state is refreshed while ownership remains held. This review authorizes no runtime action and supplies none.

## Inspected source hashes

- `src/core/runner.c`: `c136a7ceb0311d2ee9c091c93fa6b6af5c4843604d180e23bfb852c721acff94`
- `src/core/runner.h`: `398e5e7a428b9cc5342d8ccaec21a6f0ed4a27dbf96a1ec84824234a1126d31f`
- `src/core/session.c`: `b7db23d2589f554dc33ba993ebf257a3c13a34b3b69dfb54eb1ccf9523b980eb`
- `src/core/session.h`: `3b2ce5dc8eae0f14ef5f8bccc74ef9f61d8d41b6b492cd4774b26f346c00fa04`
- `src/cli/cmd_ask.c`: `f5614c0a6460e05586fdca2926bb9e16b82df7e436fb095c530b5e655e596746`
- `src/tui/tui_runner.c`: `e646a5b4457d16dbd76bbce8f8c49f707de776459f3a97918edd725cf7d8151e`
- `tests/integration/test_isolation.py`: `dfeed6708caadd4cccfc233d87e7fb75a4c04c8d50ad83299eef53cca82f4073`
