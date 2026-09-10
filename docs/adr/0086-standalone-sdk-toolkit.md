# ADR 0086: Standalone toolkit in the native SDKs

Status: accepted. Date: 2026-09-10.

## Context

SDK consumers can embed agent sessions, but images, speech, dictation, and
prompt optimisation require CLI/TUI adapters or agent tools. All four already
have shared native services. SDK callers need those services independently
with the same validation, provider routing, and output semantics.

## Decision

Add typed `Toolkit` / `AsyncToolkit` APIs in Python and `Toolkit` in TypeScript
for image generation/editing, speech export/playback, WAV transcription, timed
microphone capture, and prompt optimisation. Reuse core services; add no
provider wire implementation to the language adapters.

ABI 1.2 adds five symbols for a single-use opaque job. A bounded, versioned JSON
envelope describes configuration and heterogeneous operation options without
changing frozen agent runtime records or exposing private C layouts. The C
boundary validates fields/types/duplicates/UTF-8/bounds; SDKs supply typed methods.
ABI 1.0/1.1 agent APIs remain usable, with a separate ABI 1.2 toolkit gate.

Create copies input without I/O. Run owns a fresh context and blocks on a
caller-selected thread; cancellation is atomic, sticky, and cooperative. SDKs
copy borrowed result JSON and destroy each job. There is no CLI spawn, global
chdir, environment mutation, persistent session, or shared native runtime.
Media credentials/gateway and optimisation endpoint/key/wire overrides are
per-context/per-call. Existing agent runtime defaults remain unchanged.

Python async calls use an executor and join cancelled work. TypeScript uses one
native thread and a bounded one-result thread-safe function per operation, plus
an environment cleanup hook that cancels/joins it. Node-API async work drains
before cleanup hooks during worker termination, so it cannot promptly cancel a
stalled request. The worker-termination regression test exposed that delay and
now verifies the dedicated-thread teardown.

Outputs retain atomic replacement and bounded capture/response rules. The
optimiser retains its read-only allowlist and returns a draft for caller review.
Errors omit provider bodies; native request strings/staging are wiped before
release. Language-owned immutable strings cannot promise wiping.

## Verification and limits

Native tests cover copied inputs, strict schema, single use, pre-cancellation,
cross-thread cancellation, busy destroy, and fork rejection. Exhaustive
allocation-failure sweeps cover toolkit request creation and context setup,
including allocation failures while constructing errors. Both SDKs use a
shared loopback fixture for actual image/MP3/WAV/project-exploration flows,
output preservation, concurrency, and cancellation. Fake recorders/players
test host boundaries without physical devices. ABI inventories/signatures cover
the additive symbols, and frozen consumers remain unchanged.

The native SDK platform matrix is unchanged; no browser/wasm SDK is introduced.
Shared CLI services retain their documented wasm behavior. Fixture tests do
not establish live provider entitlements or physical microphone/playback behavior.
See [SDK toolkit](../sdk-toolkit.md) for the complete public contract.
