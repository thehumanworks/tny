# 0094 — Explicit optional image exports and contact sheets

Date: 2026-09-12
Status: proposed; final integrated verification pending
Requirements: R125.1–R125.7, R122.4, R127.3 and artifact lineage; canonical contract A12.

## Decision

Keep provider generation native and unchanged. Only an explicit export or contact-sheet operation runs a resolved ImageMagick7 executable, using finite validated options and fixed private ASCII stage filenames with forced PNG/JPEG/WebP coders. It receives already approved/read/hashed source bytes, never arbitrary user filenames or shell syntax. Verify the entire encoded output through the actual decoder plus exact MIME/dimensions before commit. Local cache/time/size limits are described precisely without claiming a kernel sandbox.

Hold a canonical destination guard and parent-directory fd. Reject source aliases and unexpected target identity changes. Create the target stage relative to the retained directory fd with exclusive/no-follow flags; install complete bytes via linkat for no-overwrite or renameat for explicit overwrite. Neither follows the target for writing, so source inode bytes remain intact. Uncooperative external namespace races are detected where observable, not misrepresented as atomic inode-CAS.

Fit proportionally contains then pads; crop proportionally fills then crops using explicit gravity; pad never enlarges and shrinks only as needed. Contact sheets use ordered inputs, explicit grid, exact canvas and optional fixed numeric bitmap labels, avoiding an unrecorded font dependency. Every output is derived, never native generation, with source hashes/operation IDs and transform lineage in the shared manifest format. Missing/failed/cancelled converters preserve originals and old targets. A metadata-finalization failure preserves the paid/derived artifact but remains a separately visible error at every offered surface.

## Alternatives and consequences

A linked image decoder/encoder increases mandatory runtime weight. Shelling out with user paths exposes coder/@file/selector syntax. Header-only validation cannot establish a complete valid pixel stream. A mutable sidecar loses lineage under replacement. The optional bounded external tool and shared immutable records meet this scope without those tradeoffs; users need magick7 only for explicit native transforms. Wasm rejects external transforms before side effects but retains shared image operations and small direct previews.

## Validation

Two fresh design reviews challenged executable discovery, full decoding, retained-dirfd commit, exact grants and effective limits. A12 fixes the final openat stage-creation detail before source writes. Required real decoder/pixel/canvas/source-preservation/cell-order/label/alias/injection/permission/cancel tests, controlled faults and independent actual-code review remain gates. No standalone SDK export API is separately promised; existing reference consumers must preserve derived metadata.
