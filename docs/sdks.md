# Python and TypeScript SDKs

tny ships two language adapters over the same native `libtny` runtime. Neither
adapter reimplements provider wire protocols or the agent/tool loop.

| SDK | Package | Binding | Scheduler model |
| --- | --- | --- | --- |
| Python | `sdk/python` (`tny`) | cffi ABI mode | sync owner thread or one dedicated asyncio executor |
| TypeScript | `sdk/typescript` (`@thehumanworks/tny`) | C Node-API addon | one dedicated native owner thread per runtime |

Both require the frozen ABI-0.5 v0 layouts, copy every borrowed event and
capability field before freeing its owner, expose unknown events explicitly,
and validate `tny_abi_version()` before creating runtime state. Capability
snapshots—not platform guesses—govern supported behavior.

## Python

The Python package provides context-managed `Runtime` and `Session` objects,
typed immutable events and errors, sync iterators, asyncio adapters,
permissions, steering, and cross-thread cancellation. Text and identifiers
remain `bytes`; decoding is always explicit.

Two wheel modes are deliberate:

- a `py3-none-any` discovery wheel contains no native library and requires an
  explicit, environment, or system-installed libtny;
- a platform wheel contains one target- and SHA-validated shared library under
  `tny/.libs`.

See [`sdk/python/README.md`](../sdk/python/README.md) for examples and the full
wheel policy.

## TypeScript / Node.js

The TypeScript package exposes ESM-first `Runtime`, `Session.run()` as a
pull-driven `AsyncGenerator`, `Session.ask()`, permissions, steering,
`AbortSignal` cancellation, explicit async disposal, and canonical typed
events. The C addon owns a bounded command/completion queue; all owner-affine
libtny calls run on its native owner thread, while ABI-0.5 cancellation uses
the runtime wake path.

Cross-thread cancellation applies after a turn has started. A first provider
connect/TLS handshake remains an owner-thread operation bounded by the native
15-second deadline; environment teardown may wait for that bound rather than
preempting an in-progress handshake.

Release tarballs contain a matched addon, libtny library, and integrity
manifest. Source compilation remains an explicit fallback requiring libtny
headers and a C11 compiler. See
[`sdk/typescript/README.md`](../sdk/typescript/README.md).

## Platforms and authority

The native SDK matrix is macOS arm64 and Linux glibc x86_64/aarch64. There are
no Windows, musl, wasm/browser, or public-static SDK artifacts. The current
public runtime embeds only the native OpenAI-compatible backend. MCP, custom
tool callbacks, host-service callbacks, Cursor, Codex, and ACP remain disabled
unless a future capability snapshot advertises them.

The SDK default permission mode is `ask`, not the CLI's yolo default.
Credentials are copied into the native runtime, excluded from repr/error/report
text, and wiped from language-addon staging as soon as native creation returns.

## Verification and release

```sh
make test-sdks
python3 sdk/conformance/check.py
```

The dedicated SDK workflow expands this to the CPython/Node version matrices,
strict local-provider fixtures, clean wheel/npm installs, type checking,
artifact hashes, native dependency/rpath inspection, macOS 13 deployment, and
glibc 2.34 compatibility. The cross-language conformance runner is the release
gate; structural JSON alone is not certification.

The repository currently has no project license grant. Owner-published GitHub
artifacts carry `LicenseRef-UNLICENSED` metadata and third-party notices but
grant downstream recipients no reuse rights. PyPI/npm registry publication
waits for the repository owner to choose a license.
