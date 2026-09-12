# Final bounded runner reload source review

**APPROVE the inspected runner spawn/resume correction, including the new-session publication delta. No remaining blocking finding in this bounded slice.** Exact source hashes are recorded below; previous slice-review.md and reload-review.md remain unchanged as historical reviews. This review performed no runtime execution, interposers, injected hooks, fault scripts or reproductions.

## Final new-session handoff

The internal persisted flag is set by a successful session_open and immediately after successful atomic session.json publication. Setting it before any later task-sidecar rename result is correct: the metadata has already been published, and a failed sidecar completion does not make this a new unsaved session again.

Spawn acquires ownership before any reload, new publication, listener bind or child storage work. If spawn newly acquired a persisted session, it reloads/reconciles the resolved durable snapshot under that descriptor. Missing/corrupt previously saved data refuses before bind and releases only the newly acquired lock. A genuinely new session is now saved under ownership before fork; actual success makes both parent and child inherit persisted=true. This closes the reviewed initial-prewarm/end-before-TURN_END gap. A failed initial save refuses before listener creation and preserves caller-owned descriptor semantics.

The helper retains the same descriptor and process-local extension/lifecycle identity while replacing the validated durable document/task body. CLI resume still performs fresh validation after acquisition and before provider work. The earlier approved shutdown ordering remains: retain ownership through engine/MCP teardown, final save, log flush and socket unlink, then release before bye.

## Ordinary test assertions

The new saved-snapshot test starts with a stale parent, performs a later writer save, starts and normally ends a runner, then verifies the later title/turn count survives final teardown. Missing-saved-data refusal checks no listener and released newly acquired ownership. The new-unsaved test now confirms parent and reopened saved state both have persisted=true, normally ends the first child, performs a later durable update, then respawns from the same parent object and verifies that update survives the second child. This specifically protects the fork-local flag problem identified in the prior review.

These are inspected assertions, not executed proof. Parent owns the ordinary runtime suites and the separately proposed session_end extension regression. The latter is a sound design: session_end is synchronously invoked after turn finalization through the supported extension host; a session-bound ready marker allows checking that the writer is still held before releasing the handler. Require normal handler completion without timeout and subsequent bye/free lock plus retained transcript. Default handler timeout is 5000 ms and its private configurable range is 1..600000 ms; bounded driver/handler margins must be explicit.

## TUI interaction and scope

Shared pre-bind refresh now covers TUI initial resume, explicit resume and runner replacement. Normal isolated prewarm uses the runner process rather than a parent prewarm thread. The fork-safety state is monotonic, and relevant context-changing commands already drop the parent prewarm reader. No new ctx-reader race was found in the normal shared-spawn path. Process-local extension fields remain preserved by reload.

A separate pre-existing /rename and /compact issue was reported to primary: they can save after sending end without acquiring/waiting for the old writer. Primary explicitly classified this outside the original six issues and current runner spawn/resume correction. It is recorded here as an unrelated finding, not silently fixed or represented as covered by this approval.

The new ordinary extension regression was still being authored when this review froze its C/header/unit scope; it is not covered by the hashes below. Existing native/WASM, quality, size and exact-head CI gates remain required. No global completion or runtime PASS is assigned.

## Inspected source hashes

- `src/core/runner.c`: `55e5957b4ce746fdea44d52d8a74aa95bd13437f7feec2bfaed2bb8ab39dc65e`
- `src/core/runner.h`: `0bba4db2abb89e0afaef84f451602502b252f3997ac88e5461f203e1d415b015`
- `src/core/session.c`: `d50cef6ae9effbcc873b5f69e583327428c9276d568d31adbe098dd4dcfc1815`
- `src/core/session.h`: `2a34be3dbc6a9964af56ff577c619d329caed0b5b6a801bd2de20031f33e6b26`
- `src/cli/cmd_ask.c`: `479ecae3aba82e2d84671037633ca9432f39201bf0f09b54cd475aa812ce9ee5`
- `tests/test_runner.c`: `d3ce1b384da98470bcf3e29802e8b81bf1ab97a26278a16ec635685a1fcd4234`
- `tests/test_core.c`: `c392f37675253839fca5a5aa8bc5f13e7a68208cc8333fbec4e235d3881447a3`
- `docs/adr/0104-runner-quiescence-ownership.md`: `56ef65783df7f75dc21345a6c9e735e93a0e307894d6a645b90cb55fe2f49f39`
- `docs/verification/open-issues-2026-09-11/contract.md`: `237945677d1fb5d5ba064cc02d4a9fb8be339f9a7280694177d649663ff64ecf`
