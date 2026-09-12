# Runner lifecycle slice — independent source review

**APPROVE the inspected lifecycle slice. No blocking source finding.** This is a read-only review of the current runner.c, runner.h, runner unit-test changes and accepted ADR0104/A26 at the hashes below. No runtime execution, interposer, injected hook, fault program or reproduction was performed for this review. No product file was written.

## Ownership and handoff

`tny_runner_spawn` now acquires or verifies the writer before constructing/binding the listener. A failed acquisition returns before pid/log/session writes or socket operations. Acquiring through `session_lock_acquire` preserves directory creation previously performed by spawn. Path allocation, listen and fork failures release only a descriptor acquired by spawn; an already-owned caller descriptor remains available to its existing fallback/error path.

The successful parent closes only a newly acquired descriptor; the child inherits the same flock open-file description. Existing foreground cmd_ask explicitly closes its caller-owned parent copy after fork, and background cmd_ask exits after reporting. TUI and unit callers enter with no descriptor, so spawn drops their parent copy itself. `session_lock_release` closes the descriptor rather than issuing LOCK_UN on the shared description. No handoff gap or accidental child unlock is introduced.

Removing the late child acquisition and per-turn acquisition is justified by that pre-bind guarantee. The child writes pid/task.log and prepares providers only after inheriting ownership. A competing TUI spawn can no longer remove a live runner's listener between a liveness probe and binding.

## Errors, cancellation and shutdown

Both once and serve finalization retain ownership. Serve `rn_turn_err` now leaves the lifetime lock held so empty/malformed turn, prepare failure and start/save failure cannot admit a competing writer while the runner remains alive. Once errors still finalize the existing error result and converge on teardown. Existing active-turn cancellation, hard cancellation, orphan exit and end handling remain intact.

Final engine end/free and MCP shutdown occur before the last session save and stderr/stdio flush. The listener is closed and its socket pathname removed while ownership remains held. Only then is the writer released and bye broadcast. After release, engine is null; bounded client flush/drop and permission/buffer frees do not save sessions or unlink the shared socket. Permission callbacks require a live engine, and pending-question failure only updates control state/sends a response. This closes the reviewed late-save/socket-removal ownership gap without holding the writer during the final client-drain wait.

## Test fidelity and remaining boundary

The ordinary unit additions inspect the relevant boundaries: refused competing spawn preserves socket device/inode and current ownership; an actual empty serve turn returns TURN_ERR while retaining ownership; receiving bye coincides with absent socket and a free writer. Saving the helper session first ensures the running probe observes a real session document. These are source-inspected assertions, not claimed executed results.

The unit additions do not alone establish every once-mode/provider-failure path, successful serve retry after an error, or resumed snapshot freshness. Existing isolation/background/steer/task/runner/platform gates remain required. ADR0104/A26 explicitly retain those obligations. Resume reload and the orphan-resume test's actual-writer-freedom wait are a separate slice and are **not approved by this report**. Those files began changing concurrently during this review; only their unchanged parent handoff was considered here.

## Inspected slice hashes

- `src/core/runner.c`: `f922eb27782016f9381b5321cf432fef5a64d78184d8f5dff48ba48d37e4db1d`
- `src/core/runner.h`: `0bba4db2abb89e0afaef84f451602502b252f3997ac88e5461f203e1d415b015`
- `tests/test_runner.c`: `7434f084f72b3a2b9f138106ea8061b1f1717a3742eab380a330a0119e38dc36`
- `docs/adr/0104-runner-quiescence-ownership.md`: `56ef65783df7f75dc21345a6c9e735e93a0e307894d6a645b90cb55fe2f49f39`
- `docs/verification/open-issues-2026-09-11/contract.md`: `237945677d1fb5d5ba064cc02d4a9fb8be339f9a7280694177d649663ff64ecf`

Worktree base: `f9682656395ef7e024f6ba5abcd3759f588ac31a`. `git diff --check` reported no whitespace errors. No global completion or runtime PASS is assigned.
