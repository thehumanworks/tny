# Verification contract: checkpoint ownership (#142)

Date: 2026-09-17. Baseline: 4e760be09908d61e23029486e1c9a3b91fbfcbbe.
Branch: feat/cpp-checkpoint-ownership. Scope: issue #142 only.
User deliverables: five ranked issues (#142-#146); complete #142; commit/push/open PR, not merge.
Existing original worktrees/branches/live sessions remain untouched.

## Invariants and required proof

| ID | Invariant | Evidence required |
| --- | --- | --- |
| CP1 | Private C++20 checkpoint owners, one owner per resource, unchanged public C ABI/schema/OS seams; exceptions never escape C. | Ownership inventory, source review, C caller and ABI builds, strict compiler/analysis gates. |
| CP2 | All existing snapshot fields round-trip; null/missing versus failed allocation stays distinct; retained data outlives input JSON. | Full/empty/optional/long-field, deep-copy and malformed-type tests. |
| CP3 | Encode/restore/public-identity/recovery fail closed on required allocation loss; partial arrays/documents/contexts release safely. | Discover baseline allocation counts, inject each index with an observed-hit oracle; sanitizer/leak cycles. No empty or compile-failed mutation passes. |
| CP4 | Recovery never mutates its borrowed resolved context on success or failure. No allocating rollback. Existing model routing, permission/tool clamps and credential sources preserved. | Full before/after snapshot and pointer/value checks across all fault indices; valid saved-model routing, identity/authority/private-field rejection tests. |
| CP5 | Public snapshots exclude private credentials/config/header bytes; temporary secrets use scoped secure cleanup; no new diagnostics or persistence of secrets. | Synthetic-secret absence tests, explicit secret-owner review and cleanup checks. |
| CP6 | Meaningful ownership mutations fail actual runtime oracles. | Valid baseline plus checked-copy, identity-failure, caller/authority mutations; record kills/survivors and fail closed. |
| CP7 | Integrated candidate passes existing and new gates without introduced warnings or disabled diagnostics. | make test, make quality, checkpoint fault/sanitizer/mutation, background/restart, ABI and relevant allocator gates; CI/Nix wiring and actual hosted results explicitly distinguished from pending. |
| CP8 | Strict artifact size <6,000,000 bytes; measured performance stays within existing startup thresholds. | Same-host pre-change/candidate stripped size+dependencies, startup raw samples and checkpoint workload/memory/allocations. |
| CP9 | Independent design/code review findings actioned; source-bound evidence and PR delivered. | Review text and dispositions; tested source manifest; commit, remote branch and PR read-back. |

## Baseline discovery

Original checkout /Users/tomas/projects/tny is clean on fix/cpp-ownership-finalization (cb0f74c).
Remote main includes merged #140/#141. No open PR at retrieval. Prior issues #137-#139 remain
open but their merged implementation is not repeated or automatically closed.
Work isolated at /Users/tomas/projects/tny-cpp-checkpoint.
Baseline `make -j8 release test-unit`: exit 0; 567 tests, 32,004 assertions.
Metadata and complete initial issue snapshot retained by coordinator.

## Status

All CP invariants pending candidate evidence; baseline success does not establish completion.
No whole-config/session/provider/MCP migration. No new third-party dependency or feature.
