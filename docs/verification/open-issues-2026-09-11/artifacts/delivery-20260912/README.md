# Delivery reconciliation — 2026-09-12

The user explicitly requested execution of HANDOFF.md through commit, push and
PR creation. Branch: `feat/durable-image-workflows`, based on
`b80c04b9df740c8388da03991cf4808c07e9cb50`. Existing dirty work is preserved.
No merge, direct-main push, release, deployment or issue closure is authorized.
If acceptance remains incomplete, delivery is a draft PR, not issue completion.

Fresh independent read-only review sessions (separate worker cwd per handoff):

| Boundary | Session |
| --- | --- |
| Owned manifest plans and retained failures | `6b47897537cd56c9` |
| Exports and contact sheets | `165b2e428ea549b2` |
| Corrected durable jobs | `224e945e75f18eda` |
| Captured preview queue | `b844f92ede16d150` |
| Reconciliation, secret safety and native goal discovery | `afffb479715227a0` |

Review prompts exclude prior findings/verdicts. Reviewers cannot write sources,
run tests, spawn children, commit or create goals. They may inspect source with
read-only shell commands. The coordinator alone integrates shared files.

I-G6/C-G6 remains BLOCKED: baseline ADR prefixes 0030 and 0045 collide, while
both immutable filenames/bytes and global uniqueness remain required. No waiver
is inferred from delivery authorization. All other original gates remain active.

## Fresh review dispositions

All four first delivery reviews rejected integration. Raw findings are retained
in `manifest-review.md`, `exports-review.md`, `jobs-review.md`, and
`preview-review.md`. Static findings are not credited as executed regressions.
Each component has a separate correction worker in its existing isolated tree:

| Component | Correction session | Required follow-up |
| --- | --- | --- |
| Manifest | `da86b4a070b64544` | Resolved replay metadata/validation, Python discriminants, actual permission matrix |
| Exports | `0da8284aae21c2b0` | Bounded child drainage, same approved bytes, source dimensions, precommit hashing |
| Jobs | `f20beb266cffed22` | Ownership/failure/removal races, credential environment, reservations, handshake, history, cleanup |
| Queue | `6ac9b2542b30eb83` | Terminal disposition, bounded reads, complete control strings, SSH captured bytes |

Corrections require fresh independent review and integrated reruns. The full
preview design checkpoint is session `6ac36b843180179d`; no generated-result
preview implementation is inferred from the captured queue.

## Initial canonical-tree check

D001 `env -u TNY_TOOLS make -j4 test` failed (491 unit tests, 490 passed).
The new subagent environment assertion assumed `TNY_NESTED` was absent, but
this harness legitimately supplies it. The fixture now sets an explicit parent
sentinel, verifies preservation, also tests absence, and restores the caller's
original value. This strengthens the no-global-mutation oracle instead of
removing it. D002 reruns the full command. Both runs bind to source hashes in
`checks/`; neither establishes later combined-tree acceptance.

## Host-package scope incident

Worker `0da8284aae21c2b0` ran Homebrew installation of ImageMagick and
its dependencies outside the bounded worker/task-scratch scope. The existing
task-local ImageMagick copy was available. This was not authorized by the
worker assignment or original preservation contract. The user was notified;
no uninstall or further system changes are attempted. Exact package/version
receipts and the install-log hash are recorded in `host-package-incident.json`.
I-G9 preservation conformance remains unresolved pending explicit disposition.
A successful test run does not excuse or retroactively authorize this action.
