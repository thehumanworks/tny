# PR 148 integration contract

Scope: user authorized integration of existing issue-144 fixes, conflict resolution,
push and merge to remote main when appropriate, and synchronization of local main.
On 2026-09-17 the user confirmed there is no active writer in this checkout.
This contract precedes new integration edits; existing fixes predate this task.
The original issue-144 records remain historical evidence; this task makes no new
performance claims. Owner: primary agent.

| Invariant | Requirement and check |
| --- | --- |
| I1 | Preserve all existing local fixes and remote-main changes. Check checkpoint commit, merge diff, independent review, clean index and no conflict markers. |
| I2 | Integrated code passes make test, make quality and focused native ownership/runtime mutation gates; hosted CI, SDK and Nix checks pass on the published head. Inspect errors rather than suppress checks. |
| I3 | Keep public C ABI and existing ADRs unchanged. Hash baseline ADRs; review C/C++ boundaries and failure paths. Record any substantive new decision separately. |
| I4 | Push exact tested feature state, verify PR148 merged, and ensure local main equals latest origin/main after fetch. Preserve other worktrees. |

Review checkpoints: fresh read-only integration-plan review before merge; a separate
fresh review of fixes and merge result before publication. Mutation checks reuse
maintained native/runtime targets; no new product feature is requested.
Timing: baseline.json and contract.initial.md captured before integration.
Native goal: not created; tool instructions permit creation only on explicit user
or system/developer request. That restriction overrides skill goal-linked mode.
Evidence: evidence.md. Stop only after all four invariants have current proof, or
report a concrete external blocker without claiming merge success.

## Review amendment

Independent plan review: integration_plan_review. Add to I2: `make test-abi`,
`make test-native-leaks` and `make leaks`; release artifact <6,000,000 bytes.
Hosted wasm/SDK/Nix/Windows checks are required on the delivered source.
No public ABI or historical ADR changes.
