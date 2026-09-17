# Independent review and dispositions

## Design review

Reviewer: Claude Fable, medium effort, fresh read-only CLI session.
Command: `claude --model fable --effort medium --permission-mode plan -p <prompt>`.
Exit status: 0. Full returned review: [artifacts/design-review.md](artifacts/design-review.md).
Source inspected: baseline 4e760be (the review ran while the implementation
worker began its separate slice). This is design evidence, not final code approval.

| Finding/proposal | Disposition |
| --- | --- |
| Checkpoint remains first: unchecked field copies, policy/instruction loss on OOM. | Accepted; #142 remains selected. All strings use checked copies; full allocation-index proof required. |
| Promote OpenAI request and MCP handoff over session storage. | Accepted. Final five issues/order: #142, #144, #145, #143, #146. Titles and ranking updated. |
| Recovery mutates caller and appends duplicate Grok routing headers. | Accepted as a defect fix, not merely a language rename. Rebuild one independently owned merged context, normalize saved-model routing there, never edit borrowed caller state. Positive proxy-routing and caller-preservation tests required. |
| Use caller rollback guard. | Replaced by stronger no-mutation staging: rollback cannot itself fail because none is needed. |
| Keep encoders/identity in C. | Not adopted: identity already owns JSON copies and secret buffers. One narrow C++ checkpoint unit shares checked field definitions and cleanup, while C facade, schema and OS/scheduling boundaries remain. A new scoped ADR must explicitly extend both historical ADR0114 decisions. |
| Add secure temporary owners and explicit tny allocator calls. | Accepted. Preserve context credential wiping; no plain-new/global allocator override. |
| Checked arrays, retained JSON lifetime, authority tests and exhaustive fault scopes. | Accepted; verify against the fully allocator-instrumented C/C++ object graph, not only C++ counters. |
| Configuration is a broad C-owned record with many writers. | Accepted as a constraint on #146: only staged replacement values gain private C++ ownership; do not double-own or mechanically rename the whole record. |
| MCP child reaping and broad public checkpoint authority deserve separate treatment. | Outside #142. MCP issue already requires explicit quiescence/reaping; no unrelated process change here. Existing public-field authority is retained; malformed representations fail closed, no new authority is granted. |

## Independent code reviews and dispositions

Two fresh Fable medium, read-only, single-pass reviews inspected source bundles.
Neither reviewer implemented the code or ran the coordinator's tests. Full
returned reviews and SHA-256 manifests are in `artifacts/first-code-review.md`,
`artifacts/first-code-review-manifest.json`, `artifacts/final-code-review.md` and
`artifacts/final-code-review-manifest.json`. Both CLI processes exited 0.
The first review required changes; the second approved conditionally on the
allocation-scope question below. This record does not relabel either as an
unconditional final-source approval.

| Finding | Resolution and evidence |
| --- | --- |
| Integer backend lacked a bound. | Reproduced by `review-backend-red` runtime assertion, then bounded by the declared backend count. The complete old unit suite exposed the legitimate `-1` unresolved sentinel from `tny_ctx_load`; it is preserved for private round trips, while values below -1 and at/above COUNT are rejected. Public recovery still requires resolved OpenAI on both sides. Tests cover sentinel and every backend. |
| Null/empty saved model retained stale Grok routing. | Reproduced by `review-routing-red`, then fixed. Routing is removed for null/empty models; otherwise exactly one replacement retains the first routing position. Three routing-shape allocation sweeps cover null, empty, middle-position and duplicate resolved headers with unchanged caller state. |
| Public backend/absent-identity negatives were missing. | Added saved nonnative/null/-1 backends, nonnative resolved backend, missing and null identity tests, and three behavioral mutants. All compile/link and die under assertions or UBSan. |
| Fixed-field bounds and padding oracle gaps. | Added overflow cases for ws_hash, task_digest and instructions_digest. Byte snapshots now use memcpy instead of struct assignment; pointed-to data is independently compared. Added a fixed-field truncation mutant. |
| Generic signed bounds assumed int. | Made that supported instantiation explicit with a compile-time assertion. Enum restoration checks named bounds before casting. |
| Could a checkpoint OOM permanently poison the runner's allocation latch? | `alloc.c` makes the state thread-local. `rn_restart`'s failure path calls `tny_engine_continue`, which calls `after_backend`; that invokes `tny_engine_fail_oom`, whose allocation-free settlement clears the latch. The new fixture injects a real checkpoint allocation failure, invokes the same real engine settlement without a test scope reset, proves zero settlement allocations, and successfully encodes the next checkpoint. This is a direct checkpoint/engine-settlement integration test, not a simulated full paused-backend restart. Existing runner/restart fixtures are separate gates. |
| Snapshot secret arenas/reallocated historical buffers are not universally erased. | No total heap-erasure claim. Explicit temporary serialized strings and the final identity buffer are wiped; public encoding never inserts private fields. Existing C context and yyjson destruction policies remain unchanged. No global secret allocator was introduced. |
| Raw C leaks were invisible to the C++ owner counter. | The final entire fixture was also rebuilt without sanitizers and run under macOS `leaks --atExit`: zero leaks/zero leaked bytes. ASan/UBSan and full C allocation injection are separate evidence. The host's restricted-content notice is retained in the log; it does not become a claim of universal memory safety. |
| Earlier fixture had a vacuous failed-scope test and self-consistent schema tests. | Final fixture passes valid snapshots to all four C entries in a failed scope, independently asserts 65 private and 55 public fields, and checks retained data after freeing inputs. A separately linked pinned baseline-C encoder validates candidate restore and encoding independently, including numeric/duplicate-key settings. |
| Constructor instruction snapshot / path_abs / ext_entry questions. | Source inspection: missing/unreadable instruction files still produce an allocated empty snapshot; NULL is an allocation failure. `path_abs` copies failed-realpath absolute names, so dangling symlinks retain skip behavior. `ext_entry` contains exactly name/path, both initialized. Extension discovery's environmental missing-directory case still succeeds empty; required allocation failures propagate. |
| Broader saved sandbox/SSH/directory authority. | No new policy introduced: pinned baseline permits the same public keys. The existing permission/tool clamps and identity checks remain. This PR does not claim to authenticate arbitrary caller-edited checkpoints or redesign wider authority. |
| Already-duplicated routing in the saved live context. | Duplicate resolved headers are normalized. An already-duplicated saved identity can fail closed rather than selecting an ambiguous route; documented in ADR0126. Rewriting all profile-switch behavior is outside #142. |

The coordinator's tests found and corrected the unresolved-backend compatibility
case after the second review snapshot. Final evidence binds the delivered source,
not just the reviewed snapshot. No review finding is treated as proof by itself.
