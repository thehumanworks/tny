# Purposeful swarm lifecycle hardening

Date: 2026-09-20

This hardening resolves independent-review issues 2, 3 and 5 without adding a
scheduler or changing child-context transport.

## Durable activation protocol

1. Validate and canonicalize the saved manifest.
2. Resolve `team_start` permission against the compiled request.
3. Generate and save a unique 128-bit activation identity with `launching`.
4. Submit through the existing jobs owner-lock path. `job.json` records the identity,
   trusted parent session and definition digest before the supervisor is launched.
5. Save `active` with the returned run ID.

The parent session writer lock serializes this sequence. On resume, `launching`
recovery reads only confined bounded job records. Zero matches means no durable record
crossed step 4 and permits retry with the same identity. One match is adopted only if
parent, digest, participant count, admission cap, ordered members, roles, groups,
purposes and coordinator links exactly match the canonical manifest. Multiple matches
or any mismatch are refused. `active` restore validates the same tuple before provider
I/O.

## State and topology validation

The saved state machine is closed:

| State | Activation identity | Run ID |
| --- | --- | --- |
| `not_started` | absent | absent |
| `launching` | required | absent |
| `active` | required | required |

Wrong JSON types, duplicate swarm metadata, invalid digest/source/snapshot/count/cap,
and invalid state combinations fail restore. Job records reject duplicate keys.
Purposeful `swarm_*` fields are rejected on public job/team requests. The compiler's
validated manifest is passed through a narrow internal C API and is never serialized
as a capability.

## Regression coverage

`tests/integration/test_swarm_lifecycle.py` deterministically covers exact interrupted
adoption, ambiguous identity refusal, corrupt session types/combinations, wrong job
ownership/digest/count/cap/topology, duplicate metadata and forged public topology.
The existing localhost-backed fixture is intentionally left for the primary lane,
because this restricted agent sandbox cannot bind loopback.

## Verification in this lane

| Command | Exit | Result |
| --- | ---: | --- |
| `make -j2 build/tny build/tny-test` | 0 | Release and ASan/UBSan unit binaries compiled; stripped release is 1,153,904 bytes. |
| `./build/tny-test -s swarm_manifest_suite` | 0 | 6 tests, 67 assertions. |
| `./build/tny-test -s core_suite -t session_swarm_definition_restores_snapshot_and_rejects_change` | 0 | 1 test, 29 assertions. |
| `./build/tny-test -s core_suite -t context_checkpoint_preserves_resolved_selection` | 0 | 1 test, 36 assertions. |
| `./build/tny-test -s team_runtime_suite` | 0 | 5 tests, 77 assertions. |
| `ruff check tests/integration/test_swarm_lifecycle.py` | 0 | New lifecycle fixture passes static checks. |
| `python3 -m py_compile tests/integration/test_swarm_lifecycle.py` | 0 | New fixture imports and compiles. |
| forged `tny team start --request -` smoke | 1 (expected) | Rejected compiler-owned metadata before creating the jobs directory or contacting a provider. |
| `git diff --check` | 0 | No whitespace errors. |

No live inference was used. The localhost-backed lifecycle fixture was not invoked in
this sandbox, per the known `bind(2)` restriction; the primary lane owns that run.
