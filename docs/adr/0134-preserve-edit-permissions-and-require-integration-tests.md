# ADR 0134: Preserve edit permissions and require integration execution

Date: 2026-09-18. Status: accepted.

## Context

During the ADR 0133 experiment, the harness's own `tny edit` replaced two
executable test scripts with mode 0600. The exact-match editor used a private
temporary file and renamed it without restoring the destination's permissions.
`make test` guarded its integration recipe with `if [ -x ... ]`, so it then
returned success without running integration tests. That apparent pass was
rejected, not reported as evidence. The executable modes were restored.

## Decision

The shared local exact-match editor retains the destination's read/write/execute
bits (`0777`), independent of the caller's umask. Resolve symlinks as before and
preserve the target's bits, not the link's. Keep the temporary file private
while writing. Apply the saved bits with `fchmod` only after all replacement
bytes have been written, before closing and renaming it. A stat/chmod error
returns the existing write-error status; a chmod error closes and removes the
temporary file without changing the original content or mode.

Do not copy set-ID or sticky bits onto newly edited content. This decision
does not promise preservation of ownership, ACLs, extended attributes, hard-link
identity, or concurrent external edits. Remote SSH edit already has its own
write-back path and is not changed here. No new OS seam or C++ scope is added:
these filesystem operations remain in the existing C11 editor. The same local
filesystem operations compile for wasm's virtual filesystem; no external tool
or native-only branch is introduced.

`make test` now invokes the integration runner unconditionally. A missing or
non-executable runner is an error, never an implicit skip. A temporary Makefile
fixture executes the actual recipe with missing, non-executable and executable
runners, so the test fails if the silent guard returns.

## Verification

The permission regression failed against the original implementation and
passes after the fix. It covers 0600, 0640, 0644, 0750 and 0755 under umask 0077;
symlink-target mode and the actual CLI edit path are also checked. A separately
compiled copy substitutes a refusing `fchmod` at the OS boundary and asserts
unchanged content/mode, the error code, temporary-file removal and descriptor
release over repeated failures. Production has no injected callback.

[The experiment record](../verification/subagent-launch-ownership.md) retains
this follow-up, including the invalidated test run and final evidence. Existing
ADRs and production permission/authorization policy remain unchanged.
