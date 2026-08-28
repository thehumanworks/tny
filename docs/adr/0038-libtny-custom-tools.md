# 0038 — libtny custom tools use copied specs and generation-tagged completion

Date: 2026-08-27
Status: accepted

## Context

Host services provide environmental primitives, but an in-process tool also
needs schema publication, permission classification, owner-thread invocation,
and a completion path that can wake a blocked pull without allowing arbitrary
concurrent runtime mutation.

## Decision

ABI 0.7 adds frozen v1 tool-spec, tool-result, and capability types plus opaque
registration and call handles. Registration copies and validates name,
description, object JSON schema, sensitivity, callback, and argument/result
limits before a session exists. Built-in names, aliases, `mcp_` names,
duplicates, malformed UTF-8/JSON, excessive limits, and post-session
registration fail deterministically. Existing built-ins and their schemas are
unchanged; active custom function schemas are appended by the native registry.
The supported schema dialect is deliberately narrow and executable: an object
root, `properties` whose entries contain only one primitive/container `type`,
`required`, and boolean `additionalProperties`. Unknown or nested constraint
keywords, malformed property declarations, and required names without a
matching property are rejected instead of being advertised without enforcement.

Sensitive custom tools use the ordinary permission engine before invocation.
Safe tools are explicitly classified safe. Denial produces the ordinary tool
error and never enters the callback. Invocation is synchronous on the runtime
owner and non-reentrant. C++ callbacks are `noexcept`; language trampolines
translate exceptions into stable status values.

The callback returns `TNY_TOOL_INVOKE_SYNC` with a borrowed UTF-8 result, or
`TNY_TOOL_INVOKE_ASYNC`. Async calls receive an opaque handle and immutable,
nonzero generation. `tny_tool_call_complete` is the sole thread-safe mutation:
it copies the bounded result, rejects a wrong generation or second/stale
completion, and signals a registry wake descriptor included in the existing
pull loop. The owner drains it, completes the tool transcript, and continues
the provider step. Callback arguments live only for the callback; result input
lives only for completion; retained data must be copied by the recipient.

Cancellation and session destruction invalidate pending calls before backend
teardown. A late completion returns `TNY_STATUS_BAD_STATE`. Hosts join their
completion workers and call `tny_tool_call_release` exactly once for every
callback that returned ASYNC. The registry and host hold separate references:
session/runtime close detaches and invalidates the registry reference, so a
late completion safely returns `TNY_STATUS_BAD_STATE`; memory is reclaimed when
the host releases its reference. Synchronous and consumed registry references
are reclaimed immediately. Unregister is owner-thread-only after the session
closes, invalidates all calls for the registration, and guarantees no later
invocation callback.

The linearization point is the registry's mutex-protected active/closing state.
Completion takes the call lock and then the registry lock; unregister and close
publish invalidation under the registry lock, detach their lists, release that
lock, and only then take call locks. Completion therefore cannot return OK once
invalidation is published and the ordering contains no lock cycle. A runtime
admits at most 64 registration objects total, including inactive tombstones
retained for deterministic repeated-handle rejection, bounding their memory.

`tny_capabilities_v1` reports registry count/name/schema/argument/result limits;
the v0 feature mask advertises custom-tool availability and enables it only
while at least one registration is active. No provider-wire logic is delegated
to language packages.

## Verification

The strict custom-tool integration fixture covers C synchronous and asynchronous
results, generation mismatch, double completion, wake latency, cancellation,
denial-before-invocation, duplicate/reserved/post-start registration, schema and
limit fuzz cases, unregister, and bounded teardown. C++ `noexcept` and Python
`ctypes` callbacks execute through the same native OpenAI mock. The registry is
compiled into the Linux TSan lane so completion and invalidation synchronization
receive race-detector coverage. Go and Swift consume the same plain C header;
no generated language-owned wire implementation exists.

The wasm CLI does not expose or instantiate this public libtny registration
surface. It advertises no custom-tool embedding capability and makes no async
completion parity claim.
