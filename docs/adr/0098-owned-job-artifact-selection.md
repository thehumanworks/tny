# Owned job artifact selection and persisted source identity

Status: accepted. Date: 2026-09-12. Scope: issues #124, #126 and #127;
verification contract amendment A20. Extends ADRs0093,0095 and0097.

## Decision

A job-backed edit or preview resolves one bounded, validated, read-only job
snapshot into an owned selection before permission. The selection retains job
and item identities, current projection attempt and original producing attempt,
carried origin, absolute path, producer digest/byte count, and optional validated
manifest/operation identity. Selection never projects job state, hashes image
bytes, retries work or initializes an unrelated provider.

Ordinary successful items have producing attempt equal to projection attempt
and no carried origin. Carried success retains its earlier producing attempt as
carried origin. Only the selected item must have succeeded. A declared manifest
must exist and agree with the complete selected artifact identity; absence is
allowed only when persistence was explicitly absent. A successful job adopts
the producer-supplied digest/length and verifies disk against them, never
replacing missing identity with a fresh digest of potentially replaced bytes.

The existing image plan and preview selection own the approved snapshot through
ALLOW_ONCE and execution. Exact loaded bytes are checked against the retained
digest, and path confinement is checked before referenced metadata/byte reads.
A later mutable job projection cannot substitute new inputs after permission.
The shared reference record stores bounded optional job provenance; manifests,
derived-source records and replay preserve it without reopening the job. Old
records without it remain valid; malformed present provenance fails closed.

Preview selects either a manifest or an explicit job/item pair. Edit accepts
that pair as one reference within the existing reference-count limit. CLI,
typed tools and interception share parsing/preparation. Direct SDK job/preview
selectors remain rejected; ordinary SDK manifest operations remain supported.
Wasm job selection returns an explicit unsupported result through the existing
platform seam; replay of already-recorded provenance remains shared behavior.

## Alternatives and consequences

Returning only a current path from the old resolver loses the approved digest,
attempt and provenance, and performs byte reads before permission. Resolving a
job again after approval can silently substitute another attempt. A new registry
or uploader is unnecessary: the existing owned image plan, manifest parser,
reference loader and captured queue provide the required boundaries.

This adds private structures and optional metadata, not a public ABI or new
provider loop. It does not reduce mandatory native platform support. Shared
integration may proceed after corrected Linux/macOS ownership review while the
isolated MSYS2 host implementation completes; all-platform completion remains
unproved until both are integrated and tested.

## Verification

A20 binds the actual byte-upload, no-provider preview, one-time permission,
post-grant mutation, producer replacement, carried-attempt, malformed/foreign
metadata, optional-provenance round-trip, replay, wasm and SDK rejection cases.
Source design and independent challenge records are under the task verification
artifacts. Runtime tests and fault injection, not this decision, establish the
implemented guarantees.
