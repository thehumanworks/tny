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

Final product/test input: `7f7531c8eaaf3caf1f5d3e0d64e8a48756b861ca`. The final
serial script completed with exit **0** in 650 seconds. Preliminary worker checks
are not substituted for these integrated-source checks. The subsequent evidence
commit changes documentation only.

| Check on final input | Observed result |
| --- | --- |
| `make test-unit` (includes release build) | 530 tests: 529 passed, no failures, one Linux-only skip on Darwin; both explicit tool-profile probes passed. |
| Team-control integration | 12 passed, including future-attempt refusal for new and historical packets, retained older attempts, bounded claims, no execution, authority, integrity and immutable history. |
| Mailbox service integration | 38 passed, including exact 64/256 saturation, over-quota corruption, old-attempt accounting, acknowledgment and publication receipt counts. |
| Typed-message integration | 16 passed with synthetic inherited-agent poison; CLI status projection, no-delivery/byte-identical storage and post-ack capacity checked. |
| Purposeful / factory / collective integration | 14 / 11 / 13 passed, including profile-specific recipes and existing dependency/resume behavior. |
| Effectiveness self-tests / environment helper | 7 / 3 passed. These are deterministic tests, not live effectiveness measurements. |
| Mixed C/C++ build checks | 17 tests: 16 passed, one Emscripten CI-only skip; new header-dependency regression passes in both instrumented lanes. |
| `make test-runtime-ownership test-libtny-fault` | Passed after the dependency fix; runtime lane 49 passed/one platform skip, plus exhaustive allocation/fault sweeps. |
| `make quality` | Passed. GCC analyzer explicitly skipped on Darwin; Linux/GCC execution is not claimed locally. |
| `make leaks` | Passed; checked suites and CLI probes reported zero leaked bytes. |
| Mailbox mutation harness | Baseline/restored suites passed and all six behavioral mutants were killed, including retained-history omission, older-attempt omission and remaining-capacity underflow. |
| `make size-check` | Stripped Darwin arm64 binary: 1,203,504 bytes; runtime libraries are `libc++.1.dylib` and `libSystem.B.dylib`. No speed claim. |

Binary SHA-256:
`9cd92d2c02e5c759b17f3347761e2e6d86b5737da36b5519b51cba1fc52273b9`.
Sanitized machine-readable results are in [checks.json](checks.json). Raw commands,
exit files and logs are retained in the private
`~/.cache/tny-swarm-review-continuity-20260920-7f7531c/` directory. Provider fixtures
use localhost and synthetic credentials; only the user-requested collaborating
agents used the configured provider. No live comparative experiment was run.

### Full-suite failures and rerun boundary

The initial **repository-wide `make test` failed (exit 2)** before the final
future-attempt and incremental-build fixes. It completed all integration entries
and reported three failures:

- `test_background_agents`: `locked_saved_inspection_retry` timed out waiting for
  `Saved read-only`.
- `test_tui`: `test_menu_overlay_transient` reported `transcript was wiped`.
- `test_libtny_mutation_fault`: the stale instrumented ownership object described
  above caused an ASan invalid free. The final ownership/fault gate now passes.

Both PTY failures were independently reproduced using a fresh baseline worktree at
`4c81d60`, built from that commit, with the same fixture commands/environment. They
are not silently waived or reported green. The baseline's clean ownership/fault
build passed, helping isolate the third failure to incremental dependencies.

The full repository suite was **not rerun wholesale** after the final fixes; all
affected suites, the main unit suite, quality and leak gates were rerun as listed
above. The first check driver also mistyped `FixtureEnvironmentTests`; its selection
error is retained, and the correct three-case `FixtureEnvironment` run passed in
the final script. These observations do not establish Linux, wasm, live-provider
performance or a completely green repository-wide suite. PR CI remains separate.

## Deferred work

- Machine-observed external check/acceptance records and automated revision loops.
- Authenticated cross-run contribution links and direct reviewer artifact mounting.
- Mailbox archival/rollover and longer-lived decision carry-forward.
- Ownership conflict warnings and extraction of pure helpers from `jobs.cpp`.
- Matched current-main writable and isolated-writer effectiveness experiments.

These remain design/measurement work, not capabilities implied by this PR.
