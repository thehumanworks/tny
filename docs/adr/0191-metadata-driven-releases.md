# 0191 — Explicit metadata-driven releases

Date: 2026-10-19
Status: accepted

## Decision

Root `release.json` requests the next stable release version. This supersedes
ADR 0085's commit-message version guessing for new releases, while retaining
ADR 0014's tag-derived build identity. A request must exceed published tags;
unchanged metadata after a release is a no-op. Metadata is read from the exact
CI-qualified commit, not the working tree or the latest branch tip. Initial
request: 0.28.0 (latest existing tag at implementation: v0.27.0).

Reuse the existing automatic/reusable tagging/publishing workflows, distribution
matrix, CI + SDK gates and token permissions. Only main-push runs satisfy the
gates. Old queued runs without metadata cannot release after activation. The
owner must re-enable the manually disabled automatic workflow in GitHub.

Retry the same tagged SHA without moving its tag. Skip already-published or
active publication, serialize release runs by ref, and upload to a draft before
publishing. Published assets remain immutable. A tag's metadata and binary must
agree. Registry publishing remains behind the existing opt-in and environments.

## Verification

Throwaway Git histories exercise metadata parsing, exact-commit selection,
merge/squash behavior, monotonicity, collisions, unchanged requests and retry.
Workflow fixtures enforce both main-push gates and explicit tag dispatch.
No release is published during development; hosted build/publication is verified
only after the owner merges and enables the workflow.
