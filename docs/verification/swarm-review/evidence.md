# Swarm review continuity: delivery evidence

## Scope and collaboration

Base: `4c81d60` (includes the contribution-aware swarm merge). Branch:
`feat/swarm-review-continuity`. The starting checkout was clean.

A captured parent started one worker-only DAG with `peer_messages:true` and three
concurrent writable workers. Ownership was split among review packets, interaction
policy, and fixture validation; the parent owned capacity reporting, integration,
documentation, final checks and the PR. All three workers finished successfully;
parent collection matched their durable result/log integrity. Worker completion is
not counted as verification of the integrated change.

The debate changed the scope:

- **Acceptance ledger vs review packet:** chose an immutable packet that separates
  observed result/log identity from supplied reviewer claims. Commands, exit codes,
  artifact references and dispositions never become runtime verification.
- **Repeated conversation vs explicit follow-up:** completed peers are not silently
  relaunched. Root-owned subsequent work carries source identities, artifact and
  findings as context; this is not an authenticated cross-run DAG edge.
- **Persistent history vs silent eviction:** retain mailbox limits and retry
  identity; expose capacity without consuming messages or reserving slots.
- **Successful vs failed result packets:** this first packet API requires a
  successful integrity-matched contribution. Failed peers still require reporting
  through status/collect. Automatic failure/revision ledgers remain deferred.
- **Broad refactor/benchmark vs bounded delivery:** no scheduler rewrite, new
  acceptance authority, live comparative benchmark or performance claim.

## Review findings

1. Worker review caught an invented `team_start` tool recipe (a permission name,
   not the advertised tool). The policy now uses `team_control` and a regression
   checks the actual advertised surface.
2. Packet validation was strengthened to reject unknown top-level fields,
   modified safety labels and malformed source identities before both publication
   and historical reading.
3. The parent's concern about long result/log prefixes overflowing the packet was
   resolved by inspecting the code: packets store scalar identity, hashes and
   lengths, not the collected base64 content. Exact claims bounds and long evidence
   have focused tests. Large recorded workspace provenance still fails the packet
   bound honestly.
4. A reviewer questioned log-integrity checking, then withdrew the concern after
   verifying the successful-result path compares the current log hash with its
   recorded digest. This was not recorded as a bug.
5. A late review found an impossible future item attempt was accepted. The parent
   reproduced it by adding attempt 2 to the malformed-source test for run attempt 1:
   the pre-fix review incorrectly returned exit 0. The validator now requires
   `item_attempt <= attempt`; historical-read and source-record regressions cover
   future attempts while retaining older carried-success support. Final rechecks
   are recorded below; the initial gate input is not the final release claim.
6. The earlier mailbox denial was not independently reproduced by the fixture
   worker, but an additional inherited-team launch failure was. Sanitized fixture
   environments plus synthetic outer-agent poison now test the actual boundary.

7. The full suite exposed a stale ownership-test object after a prior `tools_call`
   header change. Its `.d` file existed but the Makefile did not include instrumented
   test-object dependencies. A disposable-tree regression failed in both ordinary
   and sanitizer lanes, then passed after including `OWNER_BACKEND_OBJS` dependency
   files. The untouched baseline passed a clean ownership build; this was a real
   incremental-build defect, not evidence of a new runtime memory bug.

## Verification status

Final gates are in progress. No complete quality/test/leak result is claimed yet.
Commands and per-check exit records are retained outside the repository under
`/tmp/tny-swarm-review-continuity/checks`. Preliminary worker checks are not used as
final integrated-source evidence. Provider fixtures use localhost and synthetic
credentials; only the user-requested collaborating agents used the configured
provider. No live comparative effectiveness experiment was run.

The final record will distinguish all passing checks, reproduced failures,
platform skips, source revision and stripped release size/runtime dependencies.

## Deferred work

- Machine-observed external check/acceptance records and automated revision loops.
- Authenticated cross-run contribution links and direct reviewer artifact mounting.
- Mailbox archival/rollover and longer-lived decision carry-forward.
- Ownership conflict warnings and extraction of pure helpers from `jobs.cpp`.
- Matched current-main writable and isolated-writer effectiveness experiments.

These remain design/measurement work, not capabilities implied by this PR.
