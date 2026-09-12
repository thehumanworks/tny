# 0097 — Explicit generated-artifact preview

Status: accepted design; implementation and prerequisite reviews pending.
Date: 2026-09-12
Related: issues #122, #124–#127; ADRs 0089 and 0093–0096.

## Context

Image generation uses an independent provider. Success does not establish that
an agent can receive or inspect the result. The captured queue in ADR 0096 is
a prerequisite, not the generated-result feature. Preview must not turn a
successful paid operation into a failed generation that scripts retry.

## Decision

Keep metadata-only behavior as the default. An explicit `--preview` or boolean
`preview` on generation/edit/replay/export/contact-sheet requests asks one
shared post-success coordinator to submit the selected bytes through the
existing native attachment queue. No second uploader or provider loop is added.
The CLI uses the existing non-printing control exchange; tool/intercept callers
use the owning backend's admission callback, not an ungated queue helper.

Use full-resolution original bytes within the existing 8 MiB/eight-image
bounds. On oversize, return an actionable fallback: explicitly export to a
separate smaller artifact, then select that artifact for preview. Preview
never invokes conversion, regeneration or retry implicitly. A derived selection
remains derived and keeps its source/transform lineage.

A producer supplies its exact validated output hash before commit, even with
manifest persistence disabled. A selected manifest or job artifact is resolved
once into an owned identity before permission. Job selection pins job, attempt
and item, including carried-success provenance. Capture compares that identity
against the bytes actually loaded; a changed file refuses instead of replacing
the approved selection. Accepted bytes are immutable until delivery/cleanup.
Background submission and completion remain metadata-only; they do not retain
an old session socket for future upload.

Preview requires configured-true conversation image input, compatible native
ownership and a continuable active tool batch. Unknown is not support. Permission
detail includes the requested upload and target profile while retaining the
ordinary non-preview identity. The same approved execution plan runs once;
ALLOW_ONCE must not become a second permission check or a remembered grant.

An optional nested preview result distinguishes queued, unsupported,
unavailable_session, turn_not_ready, failed and not_attempted. It identifies
the selected artifact, original-byte representation and safe fallback. Queued
is a time-local receipt, never delivered/inspected/approved. A missing or
uncorrelated acknowledgment is not queued; no acknowledgment retry or manual
attachment fallback is permitted.

Successful generation/export retains its success and CLI exit 0 when preview
fails. Report the preview failure separately on stderr and in structured data.
Generation failure queues nothing. A committed manifest-finalization failure
keeps ADR 0095's failure status and retained artifact; no automatic preview is
attempted. A preview-only selection operation fails if its preview fails.

Terminal paths must account for both pending bytes and constructed-but-unsent
image messages. Cancellation, denial, policy changes, extension stops,
persistence errors and exhausted step budgets report non-delivery before
cleanup. Later turns must not inherit unsent preview images. Manual-only
attachment behavior remains compatible.

Native SDK toolkit operations remain metadata-only. No public image-send ABI
or ambient socket lookup is added. Preview misuse must be rejected rather
than silently ignored. Wasm uses shared native-loop attachment where available;
socket-only controls and external jobs/transforms return tested clean fallback.

## Alternatives and consequences

Automatic thumbnails add converter availability, permissions, output ownership
and lineage failure paths. They are unnecessary: #126 permits an actionable
bounded-attachment failure while preserving the generated artifact. Explicit
exports already supply smaller derived artifacts without hidden processing.

A path-only selection is smaller but cannot pin a generation or job attempt.
A second queue is unnecessary and would duplicate ordering/cancellation logic.
The chosen design adds only identity and orchestration above existing services.

## Verification

Contract C126 and A15 remain binding. The design checkpoint is fresh session
`6ac36b843180179d`, recorded in
`docs/verification/open-issues-2026-09-11/artifacts/delivery-20260912/preview-design-review.md`.
Its producer-hash, selected-attempt, caller parity, unsent-message cleanup,
mutation and prerequisite-review conditions are not yet discharged.

Required integration evidence includes actual next requests in Chat Completions
and Responses, same-path replacement, explicit-only behavior, selected derived
and job lineage, bounds/roots, actual ALLOW_ONCE, retained failures, control
correlation/split boundaries, second-turn recovery, SDK/ABI and browser-WASM.
No implementation, platform or visual inspection claim follows from this ADR.
