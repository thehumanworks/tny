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
