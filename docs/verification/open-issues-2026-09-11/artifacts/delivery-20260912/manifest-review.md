## Reject for reuse

The owned-plan implementation follows the intended design in the main execution paths. However, I found three correctness/type-contract issues and one required coverage gap. These should be resolved before reuse in preview, jobs, or exports.

This was a read-only source review. I ran no tests, builds, agents, or mutations. I made no edits or commits. I did not read handoffs or prior review/evidence/verdict files.

### Findings

| Priority | Finding | Action |
|---|---|---|
| **P2** | **CLI replay of an edit reports the wrong operation.** `tny_image_options()` leaves `r.edit == false` for `replay` (`src/core/image_service.c:67–68`). Resolution correctly changes `plan->edit` at line 228, and execution uses it. However, success, strict-failure, and retained-failure serializers use the original `r->edit` at lines **761, 795, and 811**. Thus, replaying an edit reports `"operation":"generate"`, including when the edited artifact was committed but manifest finalization failed. Interception resolves the operation separately, so caller metadata also differs. | Carry the resolved operation into the private result and serialize that value. Add CLI edit-replay assertions for success, strict rejection, and retained finalization failure. |
| **P2** | **Inherited quality bypasses option validation.** `image_run()` checks `valid_quality(r->quality)` at `src/core/image_service.c:518`, but inherits the recorded quality into the plan at line **239** without validating the resolved value. The manifest reader checks only its string shape/length (`src/core/image_manifest.c:309`). The adapter then forwards it directly (`src/core/image_codex.c:80–81`). A recorded quality that a direct invocation rejects can therefore reach the provider through replay. This misses the requirement to retain option validation and reject invalid replay settings before HTTP. | Validate resolved plan settings before provider availability/rendering, in the shared body used by normal and prepared execution. Add a malformed-record case that asserts zero provider requests. |
| **P2** | **Python does not expose the promised typed discriminant.** Both detail classes declare `committed: bool`, rather than different literal types (`sdk/python/src/tny/toolkit.py:122,150`). Consequently, checking `detail.committed` cannot reliably narrow `ImageDetail` to the retained type for access to `byte_count` or a non-null path. The public constructors also accept the opposite boolean despite documenting an invariant. TypeScript correctly uses literal `false`/`true`. | Use `Literal[False]` and `Literal[True]`. Prefer non-init discriminant fields so callers cannot construct the opposite state. Add a static typing check for narrowing; the existing test at `sdk/python/tests/test_toolkit.py:354–401` checks exports and runtime parsing only. |
| **P2 — coverage gate** | **The A13 permission matrix is incomplete.** The one-time approval test deliberately fails on a directory destination before loading/uploading references (`tests/test_image_service.c:1002–1027`). The record-change and changed-byte cases use typed `from_manifest` calls and invoke the executor directly (`:1051–1174`). The intercepted case similarly bypasses an actual approval response and fails on a directory, with no references (`tests/test_intercept.c:604–641`). These do not cover artifact-only mapping changes across approval, intercepted changed-byte refusal, or the native pending-permission transfer through execution/cancellation. | Add deterministic typed and intercepted cases that prepare, receive **ALLOW_ONCE without a remembered grant**, and successfully reach a local HTTP fixture. Exercise artifact-only mapping changes and changed original bytes between preparation and approval. Assert original request bytes/settings, request counts, and pending-permission cancellation cleanup. |

### Corrections that look sound in source

- Native calls and intercepted commands have separate plan ownership. Settings are deep-copied and freed through their respective lifecycles.
- Prepared execution uses the retained plan without reopening source manifests or repeating permission checks. Missing plans refuse.
- Permission detail uses the inherited provider, with `codex` as the null default.
- Expected hashes are compared against the same loaded buffers supplied to the provider.
- Pending native permission paths transfer the complete `tools_call`; cleanup reaches its owned plan.
- Typed tools, interception, and toolkit select retained detail using committed state plus the local finalization code.
- Toolkit assigns IO explicitly and checks it before late cancellation, while OOM remains first.
- The reserved-name detector checks the actual final suffix.
- Real filesystem-finalization fault tests exist for CLI, tools, interception, Python, and TypeScript. The native ABI fixture also targets cancellation after retained serialization. **Their execution results were not verified.**
- The prepared-plan API remains private. SDK detail uses the existing opaque toolkit job/result accessor; binary ABI compatibility was not tested.

### Review binding

HEAD: `b80c04b9df740c8388da03991cf4808c07e9cb50`. File hashes, rather than HEAD alone, bind this worker-tree review.

| File | SHA-256 prefix |
|---|---|
| `src/core/image_service.c` | `cd34c3713869aab65` |
| `src/core/image_manifest.c` | `18e5891fea11d6847` |
| `src/core/tools_image.c` | `c4717d0a92b31e4c3` |
| `src/core/intercept.c` | `95f20498297aa60a1` |
| `src/lib/toolkit.c` | `2d4d3bb7b3a011c92` |
| `sdk/python/src/tny/toolkit.py` | `7496c63069a42d354` |
| `tests/test_image_service.c` | `a07cc86e52cc7aaa8` |
| `tests/test_intercept.c` | `f14481e8577d5db52` |

Full hashes were emitted during inspection. The SHA-256 of the sorted 25-file source/test hash list is:

`0f463c956691f56bbbf9841d79f11be71b308354d0f58e5ccc64236bb1eb84b2`
