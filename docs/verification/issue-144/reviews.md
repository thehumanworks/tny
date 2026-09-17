# Issue #144 review dispositions

Independent completed first-slice review: [Claude Fable, medium](reviews/first-slice-fable.md),
`first-review-live`, exit 0. Supervisor identified this as the authoritative
completed source review. Supplemental read-only findings were read from
`/Users/tomas/.cache/tny-issue-144/first-review.log`. Neither review ran tests.
Final independent review belongs to the supervisor and remains pending.

F1–F3 were fixed and `review-fixes-focus3` passed request ownership plus the real
construction regression (2598 assertions) before retained/pending implementation.
Later integrated evidence supersedes that intermediate source identity.

| Finding | Disposition / code and oracle |
| --- | --- |
| F1 repeated/failed prepare can replace auth unwiped | Fixed: `attempted` latch precedes preparation; incoming body always owned. Fixture checks second prepare after success and each failed attempt; exact wipe assertion, prepare-once/auth-wipe mutants. |
| F2 fixed allocation offset shifted to aggregate allocation | Fixed: runtime fixture measures save/open prefix and construction end at provider control, then sweeps every construction allocation for Chat and Responses. `request_construction_oom_after_usage_skips_finalization` checks reserved pair, zero settlement allocations, zero submission and identical persisted snapshot after previously recorded usage. Separate HTTP/parser modes remain. |
| F3 provider-view lifetime inflated | Fixed: builders release view at last use, before schema/header construction; prepare resets defensively. Fixture asserts released view; view-release mutant. |
| F4 inconsistent null/status preconditions | Fixed: documented non-null handle arguments; get/drop consistently require them. Empty connection/unprepared send returns -2, never stale I/O. Fixture exercises pre-prepare send and failed reopen. Free tolerates a null pointee, not a null pointer-to-handle. |
| F5 fixture environment/header/leak-oracle limitations | Fixed environment via unset of TNY_PROVIDER_EXTRAS. Header slots intentionally assert the existing fixed order for this named provider; maximum capacity is documented/static-asserted in production. Counter limitations are explicit in inventory/evidence; ASan/UBSan and maintained `test-native-leaks` complement them. Mac leaks stalled in this run and is not a pass. |
| F6 mutants outside repository | Fixed: `tests/mutation/native_ownership.py`, Make target, CI/Nix inputs. Each mutant must compile/link, baseline pass and named runtime/resource oracle fail; syntax failures/timeouts are infrastructure failures. |

Supplemental report uses a different numbering:

| Supplemental finding | Disposition |
| --- | --- |
| S1 null auth name/prefix crashes | Fixed: same default Authorization/Bearer values as configuration; fixture covers null fields. |
| S2 prepare-once / dangling prepared headers | Same resolution as F1; latch includes unsuccessful first attempt. |
| S3 preconditions retry as I/O; cancelled request can resend | Fixed non-retryable preconditions plus cancellation checks before reconnect and second send. Real TCP RST test checks attempts 1/2 and stop on the second control edge. It exposed and fixed a second-edge stop returning I/O instead of interrupted. |
| S4 failed open retains live connection | Fixed: allocator failure clears the returned connection before -2. Failed reopen/resource fixture checks empty owner and stable independent request bytes. |
| S5 owned counter excludes C allocations | Accepted limitation, not treated as proof. Dedicated host leak target plus Linux sanitizer lane; no new pointer registry/accounting allocator introduced merely for assertions. |
| S6 unverified musl/Windows lanes and unmaintained mutants | Mutants are maintained. Platform targets remain integrated; remote execution is explicitly pending supervisor CI. No claim that Darwin proves Windows/musl. |
| S7 provider view retained too long | Same resolution as F3. |

Other review requests/observations:

- Real write-side replay and second-control stop: added
  `native_request_real_stale_replay_and_control_stop`, real TCP RST; fake transport
  fixture separately asserts byte/address identity across both sends. A failed
  write whose bytes never reached the peer cannot be byte-compared at that peer.
- Prepared request stopped at first control: construction discovery runs this
  real C path with no second POST; owner fixture establishes wipe-on-destruction.
- Real HTTP construction -2: retained request fault regression's HTTP mode injects
  after provider request control; connection reopen failure is covered by the
  owner fixture. No live provider is needed.
- Path allocation: reserve complete prefix+endpoint length once, then append.
- Moving `tny::required` from JSON to util and removing duplicate allocator-latch
  checks were declined as unrelated shared-helper churn; neither affects ownership
  correctness. The existing helper remains reused.
- Pending guidance implemented: all copies before move, explicit C invalidation,
  no destructor behavior, source-preserving failure, late generation/duplicate
  rejection, retained queued bytes, zero settlement allocation and recovery.
  `native_pending_lifecycle_and_allocation_sweeps` and
  `native_pending_transfer_preserves_source_on_failure` cover the boundary.
- Decode borrows remain valid through reentrant cancel via the existing
  `parser_active` deferral and `cancellation_inside_decode_preserves_callback_and_reuse`.
- New discovered CLI-only finding: the broadened first runtime probe, while still
  configured as a CLI, injected inside `skills_discover` and dereferenced null
  `path_home()` at `src/core/skills.c:105`. This file is unchanged from the base.
  The actual embedding regression now sets `library_mode=true`, as real libtny
  does. No baseline CLI fault run was performed, and no unrelated skills rewrite
  is included. This limitation is separate from the previously corrected provider
  environment diagnosis.

The supervisor's confirmed isolated full first-slice provider run and untouched
baseline provider/background runs passed. Earlier partial-environment failures
are superseded and are not evidence of a candidate provider defect.

## Final initial-candidate review and hosted CI corrections

[Claude Fable high final review](reviews/final-fable-initial.md), read-only,
completed on `2d710b60294c8bd5fa0657182b81984d4e13b1a4`, initial source manifest
`b18b0d5e...`, exit 0. The entire task-cache final-review.log was read before
implementing these corrections. This is a conditional review of the initial
candidate, not approval of the subsequently changed source. Both first reviews
remain explicitly dispositioned above, including the preserved
[supplemental report](reviews/first-slice-fable-supplement.md).

| Final finding | Implemented disposition / actual oracle |
| --- | --- |
| O1 guard absent in NDEBUG; two kills only hit assert | `tools_call_release_storage` now aborts on a live custom lease in all builds; pending reset calls it before freeing metadata. No destructor invalidates. Runner compiles/links an NDEBUG guard baseline and observes SIGABRT. It separately compiles a guard-removed **private test copy**, requires that normal baseline to pass, and then reruns cancel-authority/pending-lifetime mutants against it. Cancel-authority fails the actual late-completion BAD_STATE assertion; pending-lifetime fails actual pending result consumption. Guard catches and semantic failures have separate report fields/logs. No production bypass flag exists. |
| O2 reentrant request-control cancel can emit two terminals | Turn-open latch set by send/successful restore, consumed before terminal callbacks and cleared by OOM settlement. Direct `backend->cancel` is tested twice at first control and at the stale reopened second edge; exactly one terminal per turn, zero request bytes. `terminal-once` mutant fails the duplicate-terminal oracle. |
| O3 shipped CLI skills OOM skipped | Fixed bounded home/cwd, path join, frontmatter temporary strings, partial catalog and copied directory failures in skills.c. The complete construction sweep runs both CLI and embedding modes on both wires with dummy skills, canonical throwaway HOME/cwd, reserved pair/no settlement allocation/no failed POST/unchanged persisted session and same-engine recovery. The earlier CLI deferral above is resolved. |
| O4 inaccurate common assertion count | Historical evidence corrected to observed 18,763 sanitizer versus 18,785 nonsanitizer assertions. New per-gate counts are reported individually. Old manifests do not prove new code. Committed ADR0127 and initial contract remain unchanged. |
| Cancelled retry still constructs a request | `start_post_mode` exits before counters, allocations or control when already cancelled. It uses the idempotent terminal path. |
| -2 nonretryable status converted to retryable I/O | Propagate -2 and terminate an unlatch-ed precondition failure as internal error without declaring OOM. Direct control-disconnect fixture asserts -2, one terminal and no allocation-failure latch; cancellation cases assert successful interrupted completion. |
| Weak provider-view release oracle | Test-only sample records view state **before** prepare's defensive reset; actual provider control checks it. `builder-view-release` removes both builder releases and fails the real construction test; the defensive-reset mutant remains separately tested. |
| Retention/steer/index mutants missing | Added real partial SSE continuation/persisted-text oracle; checkpoint park/serialize/new-backend restore/continue asserts retained steer, first tool consumed once, second tool once and two retained log entries. Added continuation-retention, steer-transfer, checkpoint-index and cancel-consumed-index mutants; cancel fixture now asserts exactly one tool-end. |
| Include hygiene | tools.h now owns its C-linkage block, after includes. turn_owner.h includes it before its own extern-C block. |
| Runner inherits user environment | Runner uses an explicit allowlist plus fresh HOME/TMPDIR/XDG paths. Maintained unit test injects dummy provider/tool/fault/Make environment and proves exclusion. |
| Broader callback reentrancy unproven | Explicit limitations recorded in ownership.md; no claim for direct complete_tool/note_repairs/STEER_REJECTED reentry. |
| GCC subobject-linkage hosted failures | Request member types moved to a uniquely named private-detail namespace. No warning suppression. Local GCC14 compiles/runs the actual fixture with -Werror; Linux/MSYS hosted reruns remain supervisor work. |
| Windows GCC15.3 LTO ICE in Responses | Reused the existing native Windows/GCC-only exemption list for responses.cpp, preserving -Os and every other nonexempt object's/link's LTO. New ADR0128 records CI/upstream evidence and scope. Actual Makefile dry-run test verifies the narrow flag behavior. Local flag evidence is not a Windows build pass. |

The original nine-kill claim overstated the two assert-only results. The current
maintained runner records **15 semantic mutant failures**, with additional
production-guard catches for the two lease violations. Compilation, linking,
normal and guard-removed control baselines must pass; compile errors/timeouts
cannot count as kills. Full leak/remote/frozen-suite/performance/PR acceptance
remains with the supervisor.
