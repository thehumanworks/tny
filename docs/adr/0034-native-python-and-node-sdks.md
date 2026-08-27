# 0034 — Python and Node SDKs are thin native schedulers over libtny

Date: 2026-08-27
Status: accepted (experimental ABI-0 packages; not an ABI-1 stability claim)

## Context

The canonical event schema and sized libtny views make language bindings
possible, but a direct FFI call from an arbitrary language scheduler would
violate owner-thread affinity, borrowed-memory lifetimes, bounded queues, and
ordered cancellation/teardown. Reimplementing the native OpenAI loop in Python
or JavaScript would create divergent authority and provider behavior.

## Decision

The Python package uses cffi ABI mode. Synchronous runtime/session objects stay
on their creator thread; the asyncio adapter serializes owner-affine calls on
one dedicated executor thread. ABI-0.5 cancel is the sole operation permitted
from another thread and is serialized against handle teardown. Every borrowed
byte view is copied before its native event/error/capability owner is freed.

The TypeScript package uses a raw C Node-API addon with no runtime JavaScript
dependencies. Each runtime has one native owner thread, a bounded command and
completion path, priority capacity for abort/close, and opaque numeric handles.
`Session.run()` requests one native event only when its `AsyncGenerator`
consumer advances. ABI-0.5 cancel wakes a blocked native pull; normal state
mutation remains on the owner.

Both packages:

- validate the ABI major/minor and real capability snapshot before use;
- copy inputs retained beyond a call and reject invalid UTF-8/NUL text;
- expose explicit close/disposal as the primary lifecycle and finalizers only
  as a recoverable fallback;
- map synchronous statuses to stable generic exceptions while preserving
  normalized asynchronous error events;
- use the generated numeric event vocabulary and a non-overlapping `unknown`
  representation;
- keep credentials out of repr, exception stacks, reports, and package
  metadata, and wipe addon staging copies immediately after native creation;
- fail unsupported send options/providers precisely rather than approximate
  CLI behavior.

Native package targets match libtny: macOS arm64 and Linux glibc
x86_64/aarch64. Python supports pure discovery wheels and single-library
platform wheels. Node release tarballs contain a matched addon/library/hash
manifest; source compilation is an explicit fallback. Windows, musl,
wasm/browser, and public static SDKs remain separate artifacts.

## Compatibility and release

All public `*_v0` C layouts are frozen by ADR 0033. A future field-bearing
layout receives a v1 type and new symbols, so an older language package may
safely accept later ABI-0 minors after checking its required capabilities.

The shared executable conformance runner, clean external installs, language
version matrices, native dependency/rpath checks, artifact hashes, deployment
floors, and issue-specific lifecycle stress are release gates. A hand-authored
report or structurally valid JSON is not evidence.

The repository currently grants no project license. Packages carry honest
`LicenseRef-UNLICENSED` metadata and third-party notices; public package-registry
publication waits for an owner licensing decision.

## Consequences

Language users receive idiomatic async/lifecycle surfaces without a second
agent implementation. The cost is native, platform-specific packaging and
strict scheduler code in each binding. New libtny capabilities become SDK
features only after the shared contract and conformance adapters prove them.
