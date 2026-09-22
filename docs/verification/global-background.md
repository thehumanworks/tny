# Global sessions and reliable native backgrounding

Date: 2026-09-22. Baseline: `5d1770b`.
Status: verified locally — implementation and required acceptance coverage complete.

## Acceptance

| Requirement | Verification | Status |
| --- | --- | --- |
| Saved native turns retain a detached runner after caller-side TLS initialization; startup failure never silently weakens isolation. | Runner policy/launch tests and local transport fixtures. | Passed |
| Left with an empty composer opens the dashboard while an active HTTP or ACP turn continues; idle Left opens the same dashboard. Draft editing and focused inputs retain their controls. | PTY fixtures during streaming and tools, idle and reattached sessions. | Passed |
| Detachment preserves one writer, permission authority and ongoing effects; background work survives caller exit and permits owner reattachment. | Runner/background/interrupt integration suites. | Passed |
| `tny agents`, `/agents`, and Left show all saved sessions across stored workspaces, including completed foreground sessions and more than 100 entries. | Session unit tests and CLI/PTY fixtures. | Passed |
| A session opened elsewhere uses its saved workspace and execution context; a competing owner cannot be displaced. | Cross-workspace continuation and owner conflict fixtures. | Passed |
| Native build, required local tests, quality and leak checks pass; changed decisions and current docs agree. | Integrated gate results below. | Passed |

Fixtures use temporary state directories and synthetic credentials. Existing
user sessions and processes are preserved. The clean `build-background-verified` output avoids replacing a binary mapped
by an existing user session. Objects were rebuilt from scratch after changing
the physical session metadata layout.

Explicit ephemeral/in-process operation and wasm cannot provide detached native
process persistence. Background navigation must report these limits honestly;
ordinary saved native CLI/TUI operation must not enter those modes implicitly.
The dashboard covers the current user's configured tny state directory, rather
than scanning arbitrary files or other users' private state.

## Results

Commands ran through `mise exec -- env -u TNY_TOOLS -u TNY_SELF_IMPROVE`.
`TNY` pointed at the corresponding absolute build path for integration tests.

| Check | Observed result |
| --- | --- |
| Immutable baseline `5d1770b` regressions | Global inventory from an unrelated cwd returned zero expected rows; post-TLS native TUI failed to create a runner. Both pass on the fixed binary. |
| Clean native release and unit build | Passed; 547 unit tests passed, one Linux event-decoder test skipped on macOS; 30,100 assertions. |
| Full native test coverage | 547 unit tests passed (one platform skip), and all 97 integration checks passed across the full run and targeted reruns described below. |
| `make BUILD=build-background-quality quality` | Passed; final test-only edits also passed `format-check lint-py`. GCC analyzer is skipped by the Darwin gate and remains a Linux CI check. |
| `make BUILD=build-background-verified leaks` | Passed; zero leaks in every supported macOS suite and CLI check. The gate's documented process-spawning exclusions remain. |
| Runner ownership and mutation gates | Passed; 9/9 mutations killed, with recorded source hashes matching the final runner/jobs sources. Final-save and unlink probes now work across exec. |
| Focused lifecycle regressions | Passed: caller TLS, active ACP, same-PID streaming/tool detach, reattached Left, queued questions/permissions, startup rejection, and worktree-lock survival after frontend close. |
| Discovery/continuation regressions | Passed: 106 rows, unrelated workspaces, duplicate IDs across buckets, legacy missing-workspace metadata, correct working directory, and competing owners. |
| Real saved-state inventory from `/tmp` | 367 sessions across 113 workspace buckets; no provider startup. |

The final full `make test` invocation completed all 97 integration checks and
returned 2 because `test_native_search.py` and `test_search_service.py` still
expected the retired “Background armed” message. Their assertions were updated
to prove immediate detachment, same-runner continuity, cancellation and
exactly-once effects. Both entire files then passed against the same verified
binary (search service: 17/17). No product source changed. The other 95 checks
passed in the full run, including the updated worktree suite (33/33). Those
reruns reconcile all 97 checks; a second unchanged full run was unnecessary.

The full suite exposed lost task sidecars and swarm policy after exec; both
were corrected by reloading the saved session under the inherited writer lock
before READY. Task creation on both HTTP wires, resumed swarm policy, and the
full background-ask fixture then passed on the clean build.

Stripped Darwin arm64 release: **1,286,400 bytes**, compared with **1,269,840**
for the immutable baseline. Linked libraries: `libc++.1.dylib` and
`libSystem.B.dylib`. No performance improvement is claimed.

Changed source/test manifest SHA-256:
`54b7d59d142e36dd6ffe667ffcfd982703ab2d338ceaca2781e9d0c90b3ca2c6`.
Verification binary SHA-256:
`b24a9d0d04f00ca3bccf149361e3f5805bc23a2e19b0a0ec9ca0bafc639b8038`.
The installation rebuild may change version metadata after commit.

Sol xhigh implemented both workstreams; Luna max explored and independently
reviewed ownership, detachment and cross-workspace behavior. Review findings were
resolved with regression coverage for reattached Left, permission/question parking,
startup failure, worktree lock transfer and legacy storage identity.

No live provider inference, hosted CI, full Linux/Windows runtime, Nix or browser
wasm run is claimed. Local fixtures use synthetic credentials and loopback servers.
