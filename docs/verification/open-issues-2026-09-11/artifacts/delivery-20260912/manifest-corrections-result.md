I confirmed the replay-operation and inherited-quality bugs in the current worker code. The Python discriminants and permission tests also match the review findings. I will keep edits within your file scope and save evidence only in the canonical corrections directory. Pattern reuse will remain blocked pending a different fresh reviewer.The new tests pass for all three code fixes. The native pending-permission matrix also passes: 20 typed/intercepted cases cover one-time approval, record remapping, changed reference bytes, cancellation, and destruction while pending. Each case starts a second turn with the same permission engine and confirms that approval was not remembered.

The first unit run failed because the harness supplied `TNY_NESTED`. I preserved that log and will rerun with that variable removed.`make test` and `make quality` passed. The focused Python, TypeScript, and native ABI toolkit suites passed too. Image-service and interception unit suites reported zero leaks.

Controlled faults also confirmed that the tests catch wrong replay metadata, skipped quality validation, manifest rereading after approval, and skipped byte-hash checks. The worker source stayed unchanged during those checks. I am finishing pending-permission leak checks and the final hash manifest.Implemented all four validated findings. None were false positives.

| Work completed | Checks and results | Blockers |
|---|---|---|
| Replay serializes the resolved edit operation on success, strict rejection, and retained failure. | Replay regressions pass. Wrong-operation mutation is detected. | None |
| Shared execution validates inherited quality before provider availability or HTTP. | Malformed-record test observes zero requests. Skipped-validation mutation is detected. | None |
| Python detail types use frozen, non-init `Literal[False]` / `Literal[True]` discriminants. | Runtime checks and strict mypy pass. Boolean-type and constructor regressions are detected. | None |
| Added the actual native pending-permission matrix. | **20 cases pass:** typed/intercepted × artifact/replay × unchanged/remapped/changed bytes/cancel/destroy. Each receives two permission events and stores zero grants. Original uploaded bytes and settings are verified. | Fresh review required before reuse |

### Verification
- **`make test` passed:** 499 unit tests plus the integration runner.
- **`make quality` passed.**
- Focused workflow: **49 passed**.
- Python toolkit: **16 passed**; native toolkit ABI: **12 passed**.
- TypeScript toolkit: **13 passed**, with loader evidence binding the current library.
- Leak checks: **zero leaks** in both relevant unit suites and all 20 pending-permission processes.
- Original/mutant/restored checks passed their expected outcomes.

### Changed files

SHA-256 prefixes are below. Full before/after hashes are in the evidence manifest.

| File | SHA-256 prefix |
|---|---|
| `src/core/image_service.c` | `5dcf22e4afda75c28` |
| `src/core/image_service.h` | `cc6dec90e3f270d8b` |
| `sdk/python/src/tny/toolkit.py` | `05f1a2aa911cbc9aa` |
| `sdk/python/tests/test_toolkit.py` | `2b2019f1febc92494` |
| `sdk/python/tests/typecheck_usage.py` | `8ef6243bccd0f4158` |
| `tests/test_image_service.c` | `ae6b160cd7c1b2415` |
| `tests/integration/test_image_workflow.py` | `fadc57c1f5c9a16d7` |
| `tests/integration/test_manifest_permissions.py` | `9eb04c975d496bba7` |
| `tests/fixtures/manifest_pending.c` | `e2f70cec254a9a65d` |

All other pre-existing worker files are byte-identical, including `tests/test_intercept.c`. No deletions, contract/ADR edits, commits, pushes, children, goals, or worktree operations.

Evidence is stored only in the canonical **`manifest-corrections/`** directory:
- [Report and check limitations](../tny-open-issues-2026-09-11/docs/verification/open-issues-2026-09-11/artifacts/delivery-20260912/manifest-corrections/report.json)
- [Full changed-file hashes](../tny-open-issues-2026-09-11/docs/verification/open-issues-2026-09-11/artifacts/delivery-20260912/manifest-corrections/changes.sha256.json)

Optional Linux/browser/wasm checks remain unrun or skipped, as recorded. Initial failed attempts and exact logs are preserved.

**Stopped for a different fresh code reviewer. Do not reuse this pattern in preview, jobs, or exports before that review.**
