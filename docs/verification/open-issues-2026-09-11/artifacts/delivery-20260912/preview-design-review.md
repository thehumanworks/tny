## Recommendation

**Conditionally approve a full-resolution-first design.** Add explicit preview orchestration above the image services. Reuse the captured attachment queue and control channel. Do not add an uploader, decoder, automatic resize, or background review loop.

**Bounded full-resolution bytes with an explicit oversize fallback meet original #126 acceptance.** The original requires separate identification of **any** downsampled preview; it does not require automatic downsampling. A1 explicitly permits either a derived bounded preview **or an actionable attachment failure that retains generation success**. This is the smaller design and preserves #125’s explicit-transform boundary.

This approves a design direction, not the current implementation or issue completion.

### Evidence basis

I used the original #126 body in `artifacts/issues.snapshot.json`, AGENTS, the product/architecture/implementation docs, image/SDK docs, and contract A1/A4/A13/A15. I did not use earlier review artifacts or their verdicts as evidence.

The raw prerequisites expose these integration seams:

| Raw source | Consequence for the remaining design |
|---|---|
| Preview worktree: `core/tools.c`, `image_preview.h`, `runtime.c`, `backends/openai/openai.c`, `cli/cmd_control.h` | Captured bytes, strict preview admission, statuses, and non-printing control exchange already have private interfaces. |
| Manifest-fixes worktree: `image_service.h` and `image_service.c:622–710` | Owned plans and retained failures exist. Generation results still need an exact output digest: hashing currently depends on manifest persistence, and the image buffer is freed before return. |
| Exports worktree: `image_export.h` | Derived results already carry canonical output, SHA-256, operation ID, dimensions, and transform settings. |
| Jobs worktree: `jobs.h`, `jobs.c:3312` | The artifact resolver verifies success but returns only a pathname from the current projection. That is insufficient to pin a selected attempt and later capture its exact bytes. |

## Remaining design

### 1. Explicit surfaces; no ambient attachment

- Add `--preview` to generate/edit/replay and explicit export/contact-sheet operations. Typed equivalents accept a strictly boolean `preview`, default `false`. Terminal interception uses the same parsing, execution, and serialization.
- For completed artifacts, add a narrow selection operation:
  - `tny image preview --artifact RECORD`
  - `tny image preview --job ID --attempt N --item N`
  - Equivalent typed `image_preview` selectors, mutually exclusive.
- Preview selection never generates, replays, exports, waits for a job, or retries one.
- Job submission, polling, and background completion remain metadata-only. Do not retain a submitting session’s socket as a future preview destination.
- Omitted/false preview performs **no attachment attempt**, even with `image_input:true`.
- Explicit preview still requires **configured true**, an actual compatible owning backend, and a continuable active tool batch. Unknown preserves manual attachment compatibility but cannot authorize this generated-result path.

Do not hide generation tools merely because conversation vision is unavailable. Gate the preview-only tool consistently at advertisement and execution. Keep existing profile, permission, library, SSH, and transport restrictions.

### 2. Pin one selected artifact

Use one private owned artifact reference:

```text
operation_id, operation_kind
canonical_path, sha256, bytes, mime_type, width?, height?
manifest_path?, native, transform/lineage reference
job?: {id, attempt, item, carried_from_attempt?}
```

- Generation computes the digest from the exact validated output buffer, including `--no-manifest`, before freeing it. Do not obtain identity by reopening the mutable destination or manifest.
- Export uses its existing committed-result identity.
- Job selection returns an owned reference from the explicitly selected attempt, not merely a verified path. Preserve carried-success provenance.
- Resolve record-derived selectors once before permission. Retain their identity through execution. Capture verifies the pinned digest against the bytes actually loaded.
- A replaced file therefore causes `hash_mismatch`, never attachment of the replacement. Once accepted, queue bytes cannot change.

For generating calls, permission detail must include the requested conversation upload and target profile when `preview:true`. Keep ordinary non-preview grant details unchanged. Preserve A13’s retained plan and `ALLOW_ONCE`; preview must not cause generation to be resolved or authorized a second time.

### 3. One post-success coordinator

Keep image services independent of sessions and control transport.

After successful commit/finalization, callers pass the result reference to one private preview coordinator:

- Typed tools and interception use an owner-loop admission callback that applies the same readiness checks as the engine entry point.
- Standalone CLI uses the existing non-printing `image_preview` control exchange.
- Never call the low-level queue directly to bypass readiness.
- Never fall back to `image_attach` when preview admission fails.
- Require a correlated, valid `status:"queued"` acknowledgment. Legacy `ok:true` without that status is not proof of preview admission.
- Do not retry a lost acknowledgment. Report uncertain delivery.

Use the existing bounds: **8 MiB per image, eight pending images**, including manual entries. Account for base64 expansion and request framing; those bounds permit 64 MiB of captured bytes, not a 64 MiB encoded request.

Oversize means: keep the output and explain how to explicitly export a smaller, separately named artifact and preview that result. No converter runs merely because `--preview` was requested.

### 4. Result and lifecycle contract

Keep existing top-level generation/export success and failure semantics. Add an optional `preview` object only when requested:

| Field | Meaning |
|---|---|
| `requested:true` | Explicit preview request |
| `status` | `queued`, `unsupported`, `unavailable_session`, `turn_not_ready`, `failed`, or `not_attempted` |
| `error_code` | Existing safe admission codes; add transport/protocol codes and `delivery_unknown` |
| `receipt_id` | Correlation identity, also retained with the queue entry |
| `artifact` | Selected owned reference above; null if no artifact was committed |
| `representation` | `original_bytes`; no claim that a thumbnail was created |
| `fallback` | Safe, actionable instructions on refusal/failure |

`queued` means accepted for that batch’s next request. It means neither delivered nor inspected. Do not add an `inspected:true`, “approved,” or visual-quality success field.

- Successful generation plus preview refusal/failure: retain `ok:true`, CLI exit **0**, normal path/JSON stdout, and a clear stderr warning. The typed result remains successful generation with nested preview failure.
- Preview-only command failure: exit **1**, because no generation success must be preserved.
- Generation failure: `preview.status:"not_attempted"`, no queue action.
- Committed manifest-finalization failure: preserve A13’s retained detail and failure exit/prefix; do not queue automatically or reinterpret it as success.
- Accepted previews later prevented from delivery produce correlated `IMAGE_PREVIEW_NOT_DELIVERED` disposition before terminal cleanup. Preserve A15’s atomic refusal, no next POST, explicit freeing, and manual-only compatibility.

Standalone SDK toolkit calls remain metadata-only. Do not introduce a public image-send ABI or consult an ambient session socket. Keep preview outside SDK option allowlists and reject attempted use explicitly, including JavaScript callers bypassing static types.

## Independent challenge

These are challenges derived from the raw interfaces, not inherited review findings.

| Tempting shortcut | Why approval must reject it |
|---|---|
| Hash the output after the service returns | Another generation can replace the pathname first. The producer must return its own byte identity. |
| Use `tny_jobs_resolve_artifact()` unchanged | Its path-only return loses the selected attempt and approved digest. A later retry can substitute another result. |
| Treat active runner or configured true as readiness | Neither guarantees a next request in the current tool batch. Typed execution needs the owner check too. |
| Clear only the pending queue on cancellation | The raw `finish_tool_batch()` flushes into session history before several stop checks. An unsent image message can survive even after queue cleanup. Stage or roll back the unsent preview-bearing message; prove it does not appear on resume. |
| Make preview failure fail successful generation | Shell/tool retry behavior can spend quota again. Keep the two outcomes distinct. |
| Automatically downsample oversize images | Adds converter availability, permission, failure, and lineage complexity unnecessarily. Explicit export meets the acceptance alternative. |

## Required integration tests and mutations

Add these named cases under the contract’s `Preview` integration family. These are proposed checks, **not executed checks**.

| Test | Exact oracle | Mutation that must fail it |
|---|---|---|
| `PreviewNextRequestParity` | CLI through real control socket, typed tools, and interception: one generation call; selected bytes appear in the immediate next chat request after all tool results, in both Chat Completions and Responses formats. | Queue after continuation starts; skip one caller. |
| `PreviewExplicitOnly` | Omitted/false with configured true: zero preview exchanges/image parts; unchanged ordinary stdout/result shape. Reject non-boolean tool values. | Default preview on; accept truthy strings. |
| `PreviewCapabilityReadiness` | Unknown/false, incompatible host, absent socket, idle/streaming/completed turn, denial/cancellation and last step: never queued; committed generation remains available. Test schema/direct-call agreement. | Bypass policy or batch/step guard. |
| `PreviewIdentityAndCapture` | Generate A then B to the same path in one batch: next request contains A then B. Replace before admission: hash refusal. Replace after admission: captured bytes unchanged. Include no-manifest. | Reread at flush; hash current path; omit no-manifest digest. |
| `PreviewSelectedDerivedAndJob` | Select an export/contact sheet, and a specific terminal job attempt/item. Assert exact digest, operation and lineage. Mutate records/files after prepare; no substituted selection or new paid work. | Return current job path only; re-resolve after permission; attach native source instead of export. |
| `PreviewBoundsAndRoots` | 8 MiB boundary, +1 byte, eight slots and ninth entry including manual images; roots/hash/format refusal leaves existing queue unchanged. Oversize retains artifact and starts no converter. | Relax limits/roots/hash; invoke automatic transform. |
| `PreviewRetainedFailures` | Strict/provider failure queues nothing. Real manifest-finalization fault preserves committed artifact and A13 detail. Preview transport failure preserves successful output and generation exit 0. | Delete artifact; flatten statuses; retry generation. |
| `PreviewPermissionOwnership` | Typed/intercepted prepare→`ALLOW_ONCE`→execute uses one retained plan, no remembered grant; metadata-only grant cannot authorize newly added preview upload. | Re-resolve plan; omit preview from permission identity. |
| `PreviewTerminalDisposition` | Mixed policy-fatal batch remains unchanged at refusal, then records non-delivery, makes no POST and frees ownership. Repeat for cancellation, extension stop, persistence failure and step exhaustion. Resume sends no stale unsent preview. | Partial flush; continue POST; omit queue or transcript cleanup. |
| `PreviewControlCompatibility` | Split request/reply boundaries; wrong IDs, malformed/missing status, old acknowledgment and socket loss. No duplicate enqueue, manual fallback, or helper stdout. Legacy manual controls remain unchanged. | Accept bare `ok`; retry exchange; fallback to attach. |
| `PreviewSDKAndWasm` | Python sync/async and TypeScript preserve metadata and retained-error types; preview misuse fails before provider I/O; ABI unchanged. Wasm native-loop attachment works where available; socket-only preview and external transforms/jobs give documented clean fallback. | Silently drop SDK preview input; use ambient socket; claim unsupported wasm path succeeded. |

Run each controlled fault only when it compiles, with original/restored passes and its intended assertion failing. Final integrated checks must also retain the contract’s unit, runner, capability, SDK/ABI, quality, leak, and platform gates. New fixtures must enter CI/Nix declarations.

## ADR and approval conditions

Add a **new ADR at the next unused number** for generated-result preview orchestration: explicit defaults, original-bytes/oversize policy, selected artifact/job identity, permission scope, partial-success exits, and metadata-only SDK behavior. Reference ADRs 0089 and 0093–0096; do not rewrite finalized decisions.

Approval conditions:

1. Producer-owned hashes and attempt-pinned job references are specified and implemented.
2. All callers share readiness, permission, result, and failure behavior.
3. Terminal cleanup covers unsent session messages, not only queue memory.
4. The integrated tests and compiled mutations above pass.
5. A15’s fresh prerequisite code-review requirement and final integration review remain satisfied separately.

**Checkpoint scope observed:** read-only inspection only. No source/test edits, builds/tests, goals, child agents, or commits.
