# 0089 — Explicit image-input policy at shared runtime boundaries

Date: 2026-09-11
Status: proposed; implementation and verification pending
Requirement: R126.7 / I126.7; canonical contract ../verification/open-issues-2026-09-11/contract.md, amendment A4.

## Decision

Use a top-level image_input map of canonical provider selectors to booleans. It is separate from provider objects so builtin subscription OAuth/auth wiring is never shadowed merely to configure image input. Strict runtime parsing rejects malformed/ambiguous maps independently of the editor schema. Unknown is the default private enum; true means configured, unverified; false prohibits conversation image input. The resolved provider determines the map key, ACP aliases share the existing parser, and switches recompute the value.

Unknown preserves existing explicitly requested manual image behavior but cannot authorize new automatic preview. A configured true cannot override an unsupported transport, native-only attachment queue, tool profile or permission. False gates engine start before turn-state mutation, both image-queue entry points before file read/append, pending flush before mutation, and read_image advertisement plus execution. Refused flush preserves pending data. An additional CLI fast-fail before session creation avoids an unnecessary session/provider operation; it is not a replacement for engine guards. Generation uses its independent image provider and remains available even when chat cannot see pixels.

## Alternatives and consequences

A field under a named builtin provider object risks replacing its OAuth profile through existing shadowing rules. A model-name heuristic or default-true capability would overstate knowledge. Default-false for all absent settings would break existing explicit image workflows. The separate tri-state map distinguishes deliberate refusal, asserted configuration and unknown support without a new provider abstraction, transport or ABI record. Users selecting automatic previews must deliberately configure the conversation profile; metadata-only fallback remains a later #126 feature.

## Validation

Fresh independent design review 77a85d3c-ac1c-4317-bed4-adefab0c9d73 approved the mechanism; its four precision qualifications were resolved before implementation in A4. Tests must demonstrate actual strict parsing, builtin auth noninterference with fake credentials, ACP alias/provider-switch reset, no provider call/session mutation on refused CLI/engine input, schema/execution agreement, unchanged pending queue on refusal, actual allowed request image parts, independent generation and unknown/configured labels. Native/wasm runtime evidence and controlled guard faults are required. A later code review is required before preview pattern reuse. No acceptance or whole-issue completion is asserted by this proposed ADR.
