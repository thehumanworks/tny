## Reject for integration

Two actionable defects remain in the current sources. This was a read-only source review. No tests, edits, subagents, goals, or commits were performed.

### 1. Successful CLI exports report failure in JSON

**Location:** `src/cli/cmd_image.c:58–64`; `src/core/image_export.c:1066–1068,1159–1173`.

`tny_image_export_retained()` returns `result->committed`, including on ordinary success. The CLI checks this before checking `rc`. Therefore, every successful `image export --json` and `image contact-sheet --json` selects the retained-failure formatter.

The command exits **0**, but its JSON contains **`"ok":false`**, the fallback export-failure code, and an empty error string. Typed tools select the success formatter correctly, so the public surfaces disagree.

**Required fix:** Select the success formatter when `rc == 0`. Use the retained-failure formatter only when `rc != 0 && result.committed`.

**Regression oracle:** Require successful export and sheet JSON to contain `ok:true`, with no failure code. Separately require manifest-finalization failures to exit nonzero and report `ok:false`, `committed:true`, and the retained path/hash. The current `json_result()` helper in `tests/integration/test_image_exports.py:232–234` checks only the process exit code before returning the JSON.

### 2. Post-exit converter cleanup can signal a reused PID

**Location:** `src/util/image_transform.c:372–374,429–436,458–460`; `src/util/process.c:66–110`.

The drain loop reaps the direct converter with `waitpid(..., WNOHANG)`, then continues waiting for stdout EOF. Cancellation or deadline expiry later calls `stop_child(pid)` with that already-reaped PID.

`tny_process_kill_tree()` sends signals to the numeric PID and its group without verifying continued ownership. If a descendant retains stdout after moving to another process group, the original converter identity is no longer reserved. PID reuse during the remaining drain window can make cleanup stop and kill an unrelated process.

**Required fix:** Retain the direct child’s identity until drain and cleanup finish—for example, observe exit without reaping, then perform cleanup and reap exactly once. Do not use an already-reaped PID as cleanup authority.

**Regression oracle:** Cover cancellation and deadline expiry after direct-child exit, including a stdout-holding descendant outside the original group. Verify that cleanup never signals an unrelated identity. The existing inherited-stdout cases establish bounded return, but do not establish this ownership property.

## Input hashes

SHA-256 values below identify the dirty-tree inputs, not just HEAD. For grouped inputs, the digest is over sorted lines of `relative-path<TAB>file-SHA256<LF>`.

| Input | SHA-256 |
|---|---|
| Canonical `contract.md` | `0dd75bf80800870c2ecf26a88ae8d5a3c7e81cf9b05b87fa7ecf3c53ac63999a` |
| Canonical `artifacts/issues.snapshot.json` | `2d4a361207b7c35f769dc69315c545aa19614602b1ae511b7ce48a63258a528e` |
| `AGENTS.md` | `5cda97093b0016abb77764f63681246cbb1026d0d90545f04beed95e973bfd7b` |
| Docs group | `c3eb8f456d105665e9029c4f8c374c83378a27004a0e8a36cc39424858ead978` |
| Export/manifest group | `b0ea20040572575d92ae471b220e9a14118e78dfdaf50f90e8b6c9a1b3fad09b` |
| IO/process group | `e0eaaf64689d877b29eb85e9865b8756e37434c3add0b049e9e788fffaa5791d` |
| Callers group | `559d09a8d0cd35e7b776246b88dfc265123a58cc907b3a172f0cef838098382e` |
| Checks group | `e0c0785bc6665b9036b23878c614793e04e85f71c49f0da6c0f4b9d5aa090c2e` |

Group inventory:

- **Docs:** `docs/{product,architecture,implementation-plan,images}.md`; `docs/adr/0094-explicit-safe-image-exports-and-contact-sheets.md`.
- **Export/manifest:** `src/core/image_{export,manifest}.{c,h}`.
- **IO/process:** `src/util/image_{io,transform}.{c,h}`; `src/util/process.c`.
- **Callers:** `src/cli/cmd_image.c`; `src/core/{tools_image,intercept}.{c,h}`; `src/core/{tools,runtime}.c`; `src/backends/openai/openai.c`.
- **Checks:** `tests/integration/test_image_exports.py`; `tests/test_{image_service,intercept,runtime}.c`; `tests/mutation/open_issues_20260911_image.py`.

HEAD: `b80c04b9df740c8388da03991cf4808c07e9cb50`.
