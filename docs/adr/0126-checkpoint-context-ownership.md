# ADR 0126: Checkpoint context ownership

Date: 2026-09-17. Status: accepted for issue #142.

## Decision

Extend both ADR0114 ownership authorizations narrowly to `core/checkpoint.cpp`:
private serialization, context reconstruction, public serialization/identity,
and recovery use private C++20 owners behind the unchanged C-compatible
`core/checkpoint.h`. Scheduling, OS operations, durable schema, anonymous
credential IPC and the public libtny ABI remain unchanged. Existing accepted
ADRs are not edited. Native and wasm use the same checkpoint source and C
host seams; this adds no runtime or third-party dependency.

An owned context releases through `tny_ctx_free`. Checked member descriptors
cover existing strings and booleans; bounded numeric/array helpers reject
malformed values before narrowing or committing partial elements. Backend and
enum limits use their named declarations rather than a shared magic bound.
Private reconstruction also preserves the existing `-1` unresolved-backend
sentinel from `tny_ctx_load`; public recovery still requires resolved OpenAI. Documents
and retained strings are deep copies. Optional absence/null remains distinct
from allocation failure. Temporary serialized configuration and identity
buffers use scoped wiping cleanup; context credential fields retain the C
secure-free path. Public encoding skips private fields entirely.

Every C entry catches exceptions. New copies/arrays and JSON use the existing
tny allocator. Checkpoints observe but never clear or restart the caller's
sticky allocation scope. The explicit C constructor's optional defaults are
checked before replacement. Extension-manager construction/discovery now
propagates missing required storage instead of exposing a partial manager;
the C implementation keeps discovery and host lifecycle ownership.

Recovery merges allowed saved noncredential fields with the caller's refreshed
credentials, restores one independently owned context, checks identity, then
constructs its optional extension manager. It never borrows mutable ownership
or changes the resolved caller. Grok proxy routing is recomputed on that owned
context after removing stale model-override headers, including for an explicitly
empty/absent saved model. Replacement preserves the first routing header
position because identity includes header order. This fixes append-only
routing that previously changed the caller and made saved-model identity checks
fail. Final routing completion is checked for allocation failure and exact
header contents. Existing permission/tool clamps and public-key restrictions
remain; unknown/duplicate keys and malformed typed fields fail closed. A saved
identity that already encodes duplicate routing headers can be refused after
normalization rather than selecting an ambiguous route; general profile-switch
cleanup is not expanded into this ownership slice.

## Alternatives and limits

A rollback guard would still mutate the caller and require reliable restoration
of allocated headers. Repeated complete context restores would duplicate costly
configuration and extension work. One merged snapshot and owned reconstruction
avoid both. A whole-context rewrite, generalized serialization framework,
provider-policy redesign, MCP conversion, and public ABI changes are excluded.

Stricter malformed-type rejection and failed-copy/identity rejection are
intentional error-path fixes, not changes to valid snapshot meanings. The
existing wider recovery-policy question (for example saved sandbox/SSH/options)
is not expanded into a new policy model in this ownership slice.

## Verification

The [contract](../verification/cpp-checkpoint-142/contract.md) requires full,
empty, optional and long-value round trips; destroyed input documents;
all discovered allocation indices on the complete injected object graph;
caller preservation; refreshed credentials and saved-model routing; authority
and private-field negative controls; sanitizer/leak checks and runtime mutants.
`test-checkpoint-ownership` and `test-checkpoint-mutation` are native CI/Nix
gates. The same fixture has `--bench` for baseline/candidate measurements.
Measurements, review dispositions and exact-source evidence belong in the
[verification record](../verification/cpp-checkpoint-142/evidence.md). This ADR
makes no speedup or total memory-safety claim. ADR 0121's
strictly-below-6,000,000 byte artifact limit was current when this shipped;
[ADR 0150](0150-agent-first-harness-and-measured-footprint.md) removes that
ceiling. Separate runtime dependency accounting remains active.
