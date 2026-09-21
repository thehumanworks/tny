# ADR 0163: Explicit reasoning effort for embedded runtimes (ABI 1.3)

- Status: accepted
- Date: 2026-09-21

## Context

The CLI selects reasoning effort with `--effort`, `TNY_REASONING_EFFORT` and
settings defaults ([ADR 0009](0009-reasoning-effort.md),
[ADR 0015](0015-settings-default-effort.md)). Embedded runtimes deliberately
read neither the environment nor settings, and no options record carried an
effort, so Python and TypeScript callers could choose a model but not how hard
it thinks. Multi-agent scripts want exactly that pairing: a cheap model at low
effort for wide fan-out, a strong one at high effort where a mistake is
expensive (`examples/sdk/models.json`).

`tny_runtime_options_v2` could not take the field. Its reserved tail is never
read, so an older library would silently ignore a consumed slot, and
`tny_runtime_create_v2` requires a task name, so an effort-only caller had no
valid record.

## Decision

ABI 1.3 adds, in a new `LIBTNY_1.3` node with every earlier symbol unchanged:

- `tny_inference_options_v1` — sized record with one `tny_bytes
  reasoning_effort` and a reserved tail (72 bytes, minimum prefix 24);
- `tny_runtime_options_v3` — embeds the frozen v2 record and the inference
  record (568 bytes, minimum prefix 504);
- `tny_inference_options_v1_init`, `tny_runtime_options_v3_init`,
  `tny_runtime_create_v3`.

`tny_runtime_create_v3` accepts an empty `base.task.name` (no preset), unlike
v2, so one record serves every embedder; task instructions without a name are
rejected rather than ignored. An empty effort omits the field on the wire. A
non-empty effort is one token of 1–32 bytes from `[A-Za-z0-9_.-]`, validated
before any runtime state exists. Canonical levels (`off`, `light`, `medium`,
`high`, `xhigh`, `max`) map to the provider wire word per request through the
existing `tny_effort_wire`; any other token is a provider-advertised value
passed through verbatim, as in the CLI. The environment and settings remain
unread. `TNY_CAP_FEATURE_REASONING_EFFORT` (bit 13) is available in ABI 1.3
and enabled only for a runtime created with an effort.

Python names the field `RuntimeConfig.reasoning_effort`; TypeScript names it
`reasoningEffort`. Both validate the token locally, take the v3 path only when
an effort is set (so ABI 1.0–1.2 libraries keep working without one), and raise
their unsupported-feature error against an older library. Workflows inherit it
through per-task runtime configuration; no workflow API changed.

## Consequences

- Effort is fixed for a runtime's lifetime, like the model. A caller that
  wants a different effort creates another runtime; workflows already do.
- The token is not checked against a provider catalog: embedders have no
  catalog, and a provider rejects an unknown value with its own error event.
- wasm: not applicable — libtny's shared ABI and both SDKs are native only.

## Verification

`tests/abi/current_v3_consumer.c` (creation with and without task/effort,
minimum prefix, malformed tokens, nested size/version rejection),
`tests/abi/capacity_boundaries.c`, the ABI baseline/signature checks, and SDK
tests in both languages that run turns against the strict mock with
`MOCK_EXPECT_EFFORT` on both wires — the mock rejects a missing, unmapped or
unexpected effort, and an ambient `TNY_REASONING_EFFORT` must not leak in.
Replacing the adoption with a no-op fails the wire tests.
