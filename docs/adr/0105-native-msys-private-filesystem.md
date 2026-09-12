# 0105 — Native private filesystem operations on MSYS

Status: Accepted — 2026-09-12.

## Context

Native MSYS default noacl creates nominal0600/0700 files/directories with broad
inherited Windows access. chmod success does not establish privacy. Actual Windows
probes also show openat/fstatat using a stale parent pathname after directory rename.
Both conflict with durable job and image privacy and retained-parent publication.

## Decision

Add an internal C11 private_fs OS seam for the six new-feature callers image_io,
image_transform, image_service, jobs_host, jobs and image_export. Keep native
handles as authority for component traversal, exclusive private creation, bounded
reads, target observation, publication and cleanup. Supply an owner-only protected
DACL at native creation, before content; use lazy system DLL functions with no new
linked dependency. Reject uncertain type, alias, owner, ACL and control rights.
Existing Windows private-role entries fail closed without automatic migration;
ordinary ancestors and explicit public inputs do not gain a private-ACL requirement.
Preserve existing POSIX behavior and WASM in-process/error behavior.

Encode literal leaf names without resolving links, reject MSYS escape collisions,
and account for mount/drive anchors, native reparse points, system links and
explicit/implicit shortcut aliases. Admit ordinary POSIX I/O descriptors only by
reopening without create/truncate and comparing native identity before use. Do not
use the unchecked-failure attach-handle API. Authoritative retained-parent work
never falls back to MSYS pathname-based *at operations.

Carry an opaque transform-stage context from creation through converter quiescence
and cleanup. Pathname-consuming children require a proven stable stage view through
retained ancestor/leaf handles and sharing exclusions, or refusal before spawn;
a precheck alone is insufficient. Ordinary replacement handles retain delete
sharing for POSIX semantics. This is an explicit exception to the ordinary sharing
policy for converter stages.

Publish nonreplacement by native no-replace link, replacement by supported native
RenameInformationEx with POSIX replacement semantics. Record the commit state
immediately; postcommit cleanup cannot remove the final output or erase committed
result reporting. Dispose only retained objects created by the operation, never
foreign entries selected by stale names.

## Alternatives and validation

chmod-only repair leaves an exposure interval and is ineffective on noacl. Global
remounts would change workstation behavior without delivering default-platform
privacy. Disabling Windows tests or increasing size limits weakens the contract.
A create-only seam leaves retained-parent reads/publication/cleanup incorrect.

The detailed implementation contract and independent design review are retained at
../verification/open-issues-2026-09-11/artifacts/review-merge-20260912/windows-private-creation-design/
DESIGN.v2.md (SHA256 2e10a81a4eb4edad11517f1e81a97cca853940f76abbc9795eed0d5f6db69233)
and review-v2.md. The77-check ARM Windows guest probe establishes feasibility only.
Require independent access-denial tests, prewrite privacy, filename/alias and
retained-parent cases, failure cleanup, committed-output preservation, actual guest
product flows and native x64 hosted units/jobs. Verify Mac/Linux/WASM parity and
unchanged size budgets. Review the first primitive slice before caller rollout.
No claim is made to audit legacy session/vault privacy outside these callers.
