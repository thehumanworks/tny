# 0096 — Captured image bytes in one queue, explicit preview lifecycle

Date: 2026-09-12
Status: proposed; first-slice implementation and final verification pending
Requirements: I126.1/I126.4/I126.5/I126.6/I126.7. Canonical contract A15.

## Decision

Retain exact bounded image bytes at queue admission in the existing tools environment, then feed the existing user-message attachment representation from those bytes. Manual path loading and captured-byte flushing share one builder. This prevents two generations of the same output pathname in one tool batch from both attaching the later file. Preview origin is explicit and stricter than unknown-capability manual attachment.

Admission requires the owning native backend to be in a tool batch that can continue; a generally active but streaming/terminal turn is not enough. The control route uses the same queue and a new narrow preview op, not a fallback to manual attach. A private data-returning control helper separates transport result from command stdout/exit behavior. Existing fields and commands stay compatible.

Flush checks the whole batch before mutation. A preview-incompatible batch reports fatal non-delivery and preserves its entries until the backend records the failure, stops the next request, ends the turn, then explicitly frees the batch. Manual-only failures retain their existing behavior. This preserves atomicity without keeping stale bytes or repeatedly poisoning later turns. Queued is receipt at that moment, never evidence of visual perception.

## Alternatives and consequences

Re-reading paths at flush is smaller but attaches the wrong version of a mutable output. A second image uploader or out-of-band provider message duplicates transport and breaks ordering. Partial mixed-batch flush would add partial-delivery/retry rules not required here. One captured queue with explicit terminal cleanup has bounded memory, exact byte identity and testable failure state. Full-resolution generation and optional derived preview remain separate artifacts in the later integration slice.

## Validation

Independent design reviews verified the actual queue/control seams and challenged readiness, disposal and manual compatibility; A15 records corrected semantics before implementation. Tests must prove same-path repeated-generation identity, actual next-request pixel parts, policy/roots/hash/capacity gates, terminal cleanup and later-turn recovery, no manual fallback and no false visual approval. Native/browser and controlled-fault evidence and a fresh first-slice code review remain required before full preview reuse.
