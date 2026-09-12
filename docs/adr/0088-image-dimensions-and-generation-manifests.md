# 0088 — Image dimensions and immutable generation manifests

Date: 2026-09-11
Status: proposed; implementation and verification pending
Requirements: R122.1–R122.4, R127.1–R127.8 and shared-call-boundary prerequisites.
Contract: ../verification/open-issues-2026-09-11/contract.md, amendments A1/A2.

## Decision

Keep the shared C11 image service and existing provider, CLI, typed-tool, interception and SDK boundaries. A bounded PNG/JPEG/WebP header reader reports dimensions derived from returned bytes without claiming full pixel decoding. Requested size and exact wire size are distinct from returned dimensions; auto/opaque/unverifiable/mismatch are explicit rather than invented exact matches. Optional strict size requires concrete positive WxH and rejects mismatch/unverifiable bytes before output replacement. A paid strict rejection discards those bytes rather than silently retaining or transforming them. Ordinary success stdout remains a path; additive JSON carries actual MIME, native dimensions and transform provenance. Provider rejects are never automatically retried.

Generation records use versioned, private, unique immutable per-operation manifests, not a mutable latest sidecar. Reserve a normalized-output owner guard and write private intent before spending quota. Commit image bytes atomically, then finalize the manifest atomically under that guard; this is deliberately not a two-file transaction. Post-image manifest failure is a distinct visible error and preserves the paid image. A dead intent is interrupted/unknown, never succeeded merely because an output exists. A privacy opt-out creates no manifest or persisted prompt record. Only recognized, bounded, actually returned provider identifiers may enter the record.

Reference lineage records hashes of the exact bytes used, explicit settings, original versus derived artifacts, and source operation identifiers. Replay is explicit, bounded, version checked, requires a new output, supports overrides, and verifies references before reuse. Manifest parsing alone never uploads or grants access to referenced paths. Relative references are resolved against the recorded workspace, not a later caller directory. Output hashes prevent an old record silently referring to bytes overwritten by a later operation. Shared-service callers deliberately carry the options/results; no frozen public ABI record changes.

The precise commit, crash, metadata, replay and privacy procedures are preserved in contract A2. This ADR supersedes only ADR 0074's absence of operation manifests and strict-size metadata; its no-auto-retry, bounded request, independent credentials and private atomic output rules remain. ADRs 0075, 0084 and 0086 remain the shared-surface/default/SDK decisions. Native-versus-derived metadata, artifact/job references and previews remain pending until their respective later phases are integrated.

## Alternatives and consequences

A linked decoder merely for dimensions adds avoidable runtime dependencies; bounded header parsing answers the metadata question but cannot certify all pixel data. A mutable `<output>.json` sidecar loses lineage under concurrent writers and replacement. A database or permanent daemon is unnecessary for immutable operation records plus an output guard. Unique records cost small private JSON files and need explicit opt-out, but preserve provenance and honest crash states.

## Validation and checkpoints

Independent A2 design review e886941c-7768-4304-a219-22f446864679 approved this foundation. Implement #122 first; a fresh parser/strict-size/caller review precedes manifest persistence work. Validate exact/one-pixel mismatch, auto/opaque, malformed/truncated/oversized headers, no overwrite on strict failure, no guessed provider size catalog, all four callers and shared wasm behavior. Manifest checks add exact uploaded-byte hashes, permissions/opt-out, provider-field absence, future-schema rejection, concurrent normalized aliases, every paid-operation commit boundary, replay missing/changed references and live fixture state. C122/C127 and all original/A1/A2 mutation rows remain gates, not claims of passing.
