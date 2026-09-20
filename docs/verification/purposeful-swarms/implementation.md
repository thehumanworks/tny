# Purposeful swarms implementation evidence

Date: 2026-09-20  
Baseline implementation-contract revision: `b9d6cdf3c093814e2c1e705dc431303075ee0196`

This lane implemented the version-1 schema/parser, CLI validation and selection,
canonical session/checkpoint persistence, flat durable-team compilation, nested
identity/status propagation, scoped coordinator completion delivery, documentation,
example and focused tests. It did not change the separately owned wait-any loop,
`jobs_host` implementation, wait integration test, or swarm benchmarks.

## Verification run in this worktree

| Command | Exit | Result |
| --- | ---: | --- |
| `make format` | 0 | clang-format, Ruff format and shfmt completed |
| `make release` | 0 | stripped `build/tny`, 1,137,360 bytes |
| `make build/tny-test` | 0 | sanitizer unit binary rebuilt after final source changes |
| `./build/tny-test -s swarm_manifest_suite` | 0 | 5 tests, 54 assertions |
| `./build/tny-test -s core_suite -t session_swarm_definition_restores_snapshot_and_rejects_change` | 0 | 1 test, 29 assertions |
| `./build/tny-test -s core_suite -t context_checkpoint_preserves_resolved_selection` | 0 | 1 test, 36 assertions |
| `./build/tny-test -s team_runtime_suite` | 0 | 5 tests, 77 assertions |
| `make test-help-flags` | 0 | 4 tests; the new `swarm` dispatch/parser/help mapping is covered |
| `ruff check tests/integration/test_purposeful_swarm.py tests/integration/test_help_flags.py` | 0 | all checks passed |
| `git diff --check` | 0 | no whitespace errors |
| `./build/tny swarm validate examples/swarm/purposeful-review.json --json` | 0 | version 1; 4 participants, 2 groups, depth 2; canonical digest `ed6a493a8e73dd4052635ac0c14b4c0ac217cd7dffca6a4a01742a06b3d08d6b` |
| `./build/tny --swarm-file examples/swarm/purposeful-review.json --ephemeral ask test` | 1 (expected) | rejected unsupported ephemeral activation before provider/job setup |

`make test-unit` exited 2 in the restricted workspace sandbox. Before the failure,
the new manifest suite passed 5/5 and the team runtime suite passed 5/5. The reported
failures were all in fixtures that bind localhost: one network test, seven HTTP-server
tests and three Grok device-login tests. The OpenAI suite then aborted before printing
a detailed assertion. Loopback `bind(2)` returns `EPERM` in this sandbox, so the
localhost-dependent failures are environmental; the exact cause of the later OpenAI
abort was not independently established here.

`python3 tests/integration/test_purposeful_swarm.py -v` exited 1: all three cases
errored in `JobsFixture.setUp` before their test bodies because binding
`127.0.0.1:0` raised `PermissionError: [Errno 1] Operation not permitted`. Therefore
the executable nested-launch fixture is present and linted but has no runtime result
from this sandbox.

No live inference was run. Per lane ownership, full integration, quality, leak, Nix,
benchmark and live-evaluation gates remain for the primary lane. No push, PR or merge
was attempted.

## Covered behavior

- Closed version-1 JSON validation rejects missing nested coordinators, unknown or
  duplicate fields, blank/oversized text, duplicate names, malformed JSON, files over
  64 KiB, depth over 4 and total participants over the shared capacity of 16.
- The root lead is not relaunched. Root agents and every nested coordinator/agent are
  compiled into one durable worker DAG with explicit role, group, purpose and upward
  coordinator metadata. Team-local concurrency equals validated participation while
  existing global admission remains authoritative.
- Canonical JSON, digest, absolute original provenance, capacity, activation state and
  durable run ID persist in the session; checkpoint tests cover the snapshot/digest.
  Resume works from the snapshot when the source is absent or changed, while an
  explicitly selected changed definition and numeric-mode substitution are rejected.
- Permission precedes activation intent. A unique persisted identity is copied into the
  job record; interrupted activation adopts one fully validated parent-owned match,
  safely retries zero matches with the same identity, and refuses ambiguity. Active
  restore revalidates ownership, digest, cap, count and canonical topology.
- Public team/job requests reject all purposeful metadata. The trusted compiler passes
  its manifest through a narrow non-serialized API, which validates ordered membership
  and coordinator links before submission.
- Stable system policy contains identity/purpose only. Current task content remains in
  the dynamic participant prompt, and mailbox messages retain existing durable replay,
  acknowledgment and attempt fencing.

## Remaining limitations

- Purposeful activation is intentionally limited to native local saved Darwin/Linux
  leads with supported job execution and directory watches. wasm, Windows/MSYS, SSH,
  embedded, nested-team and ephemeral activation are rejected. Definition validation
  itself is local and context-free.
- Linux execution and all localhost-backed integration behavior still require primary/CI
  evidence. This lane does not claim the blocked integration tests passed.
- Corrupt or ambiguous activation candidates require repair; recovery never guesses or
  adopts an unrelated run.
- Flattened nesting is bounded and uses one scheduler. It does not provide recursive
  autonomous supervisors, guaranteed discussion, progress, agreement, correctness or
  convergence.
- Provider prompt-cache hits were not measured and are not claimed.
