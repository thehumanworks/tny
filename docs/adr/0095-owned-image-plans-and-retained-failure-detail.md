# 0095 — Owned approved image plans and retained-artifact failure detail

Date: 2026-09-12
Status: proposed; implementation and final verification pending
Requirements: R127.4/R127.6/R127.8; R122 caller parity. Canonical verification contract A13.

## Decision

Permission preparation owns a fully resolved image plan, including deep-copied settings and record-derived expected hashes. The prepared native call or intercepted command retains that plan until completion. Execution uses the same shared service body without re-resolving records or re-running permission checks. The existing one-time grant authorizes that exact call, not a new remembered grant. A missing prepared plan on any permission-gated route refuses. The expected hash is checked against the exact loaded bytes used in the upload.

The existing permission detail format remains for ordinary calls, but a replay reports its actual effective provider; old mistaken provider-specific grants fail closed rather than silently approving another provider. A metadata file edited after preparation does not redirect the pinned request. All allocations are owned and freed exactly once on normal, failed, cancelled and pending-permission paths. Public libtny ABI stays unchanged.

When image commit succeeds but manifest finalization fails, every caller returns a failure that identifies the retained artifact. The native SDK assigns IO explicitly before late-cancel classification, with a separate committed detail shape; strict/precommit cancelled failures keep their old shape/clearing. Generic exception messages stay secret-safe. OOM never deletes paid artifacts or fabricates success.

## Alternatives and consequences

Repeated post-grant parsing had both a race and an ALLOW_ONCE regression. Re-serializing an expanded grant fingerprint adds rule-key compatibility cost and another read. Retaining the already-approved bounded plan eliminates both and reuses the existing dispatcher ownership lifecycle. Deep copies cost at most the already-bounded prompt/options and five references, avoiding dangling interception JSON pointers.

## Validation

Independent designs challenged the exact permission and lifetime boundaries; A13 records their concrete dispositions and a C11 const-pointee probe. Actual native/interception one-time-grant and record-mutation tests, same-byte hashes, retained-error SDK/lifetime/late-cancel tests, critical faults and a fresh actual-code review are required before this proposed decision is finalized.
