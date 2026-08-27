# tny Python SDK

This is the typed Python adapter for the **experimental libtny ABI 0.5+**. It
does not claim ABI 1 stability or production readiness. The native runtime
remains the source of truth; this package owns lifecycle, thread-affinity,
event copying, error mapping, and sync/async ergonomics.

```python
from tny import Runtime, RuntimeConfig, TextDeltaEvent

config = RuntimeConfig(
    workspace=".", state_dir=".tny-sdk-state",
    base_url="http://127.0.0.1:8080/v1", api_key="...",
)
with Runtime(config) as runtime:
    with runtime.create_session() as session:
        for event in session.run("Summarize this repository"):
            if isinstance(event, TextDeltaEvent):
                print(event.text.decode("utf-8", "strict"), end="")
```

See `examples/` for complete sync, asyncio, permission, cancellation, and
resume flows.

This package is currently **UNLICENSED**: distribution by the repository owner
does not grant downstream reuse rights. Do not upload it to PyPI or another
public package registry until the owner adopts a project license. Its runtime
dependency is `cffi>=1.16`; cffi is independently distributed under the MIT
license. GitHub release artifacts retain this notice and the vendored-library
notices.

## Native library and wheel policy

Pure `py3-none-any` wheels contain no native artifact and discover an explicit,
environment, or system-installed libtny. Bundled platform wheels contain
exactly one validated library in `tny/.libs`:

| Wheel target | Bundled artifact | TLS behavior |
| --- | --- | --- |
| macOS arm64 | `libtny.0.dylib` | SecureTransport from macOS |
| Linux glibc x86_64 | `libtny.so.0` | dynamically loads system OpenSSL |
| Linux glibc aarch64 | `libtny.so.0` | dynamically loads system OpenSSL |

There is no Windows, musl, wasm, or public static-library wheel. Set
`TNY_BUNDLE_LIBRARY=/absolute/path/to/the/versioned/library` to build a platform
wheel. Without it, ordinary source installation emits a pure discovery wheel.
Source users may provide `TNY_LIBRARY_PATH`, or install libtny in the dynamic
loader's normal search path. Discovery order is explicit argument,
`TNY_LIBRARY_PATH`, wheel payload, then the system loader. The current working
directory and repository-relative paths are never searched.

The wheel uses the `py3-none-<platform>` tag because cffi ABI mode has no
package-specific CPython extension ABI. Python 3.10 and newer are supported.
Release jobs must run the
repository's libtny conformance runner and inspect macOS wheels with
`delocate-listdeps` or Linux wheels with `auditwheel show` before publishing.
Bundled builds validate the native magic, OS, architecture, and filename before
tagging. `TNY_BUNDLE_SHA256` optionally pins the digest. Alternatively,
`TNY_BUNDLE_MANIFEST` may name a small JSON object with exact `filename`,
`sha256`, `os`, and `arch` fields. Linux wheel claims require Linux CI and
`auditwheel`; they are not inferred from a macOS build.

Wheel versions come only from a canonical release tag. Stable
`vMAJOR.MINOR.PATCH` tags map directly; the narrow
`vMAJOR.MINOR.PATCH-(a|b|rc).N` prerelease form maps to canonical PEP 440 (for
example, `v1.2.3-rc.4` becomes `1.2.3rc4`). Registry builds fail if the tag is
missing or noncanonical. Trusted PyPI publication is separately gated by an
owner license, an explicit repository opt-in, and the protected publisher
environment; the current unlicensed package cannot pass that gate. Once a root
license exists, its exact bytes must be included in every wheel. Publication
rejects partial or hash-mismatched existing releases, then verifies PyPI JSON,
downloads and hashes all three wheels, and performs a clean installed
import/native-library readback. Ordinary release-wheel install tests use a
local cffi dependency wheelhouse with network access disabled.

## Ownership and concurrency

`Runtime` and `Session` are context managers. Explicit `close()` is required;
GC finalizers warn and are only a same-thread fallback. Native events are read,
all borrowed `tny_bytes` fields are copied, and the native event is freed before
the immutable Python event is returned. Native error bytes are likewise copied
before the error is freed.

ABI 0.5 is owner-thread-affine except for native cancel. Synchronous callers
must drive all other session operations from the creating thread; `cancel()` is
safe from any Python thread and wakes a blocking event wait. A lifetime lock
serializes cancel against close. `CancellationToken.cancel()` remains a
thread-safe cooperative request for iterator-driven code. cffi releases the GIL
around each native wait. `AsyncRuntime` serializes owner-affine operations on
one dedicated thread and calls the thread-safe native cancellation path.

Text-bearing event fields and identifiers are `bytes`. The SDK never performs
implicit replacement decoding. Call `.decode("utf-8", "strict")` explicitly.
Synchronous failures are typed `TnyError` subclasses. Post-start failures are
typed `ErrorEvent` values by default; `raise_on_error=True` maps them to
`EventStreamError` without putting provider text in exception strings.

API keys and base URLs are omitted from configuration reprs. Native diagnostics
are retained as bytes on `TnyError.message` for explicit inspection but do not
enter `str(error)`, `repr(error)`, or tracebacks automatically.

## ABI 0.7 callbacks and remaining limitations

`Runtime.capabilities` is copied from `tny_runtime_get_capabilities`; no
borrowed native field survives runtime closure. Basic runtime/session use stays
compatible with ABI 0.5. Passing `HostServices` requires ABI 0.6. Registering
`CustomTool` / `AsyncCustomTool` and reading custom-tool limits requires ABI
0.7 and uses the extended capability snapshot.

Host-service handlers are synchronous, execute on the native runtime owner,
receive copied `bytes`, and cannot re-enter that runtime. Custom-tool handlers
also receive exact argument bytes. Synchronous handlers return NUL-free,
strictly valid UTF-8 `bytes` or `ToolResult`; binary result payloads are not an
ABI 0.7 feature, and `ToolResult` content is redacted from its representation.
Asyncio handlers use the same result contract and complete through the sole thread-safe native
completion call. The SDK retains every cffi closure, handler, user object, and
outstanding call until unregister/runtime close has invalidated native state and
each async host reference has been released exactly once. `BaseException` is
caught at every trampoline; exception text is never placed in native errors or
reports. Explicit close remains required. GC/interpreter finalization is a
leak-safe fallback and never knowingly calls a stale native handle.

Async completion authority is retired only after a successful completion, an
already-invalid native call, an accepted session cancellation, or explicit
session/unregister invalidation. If completion and best-effort cancellation
both fail, the adapter retains the host reference until close publishes native
invalidation, then releases it exactly once. Cancellation of the scheduler
Future is not treated as handler termination: close waits for the coroutine's
actual `finally` acknowledgement and the completion callback's pending-empty
acknowledgement before dropping Python references.

See `examples/sync_custom_tool.py` and `examples/async_custom_tool.py`. MCP
ownership and providers other than the native OpenAI-compatible backend remain
unavailable; capability discovery, rather than ABI/platform guesses, governs
all optional behavior.

## Shared conformance adapter

The executable protocol-v1 adapter reads one request JSON value from stdin and
writes only its response JSON to stdout. From the repository root:

```sh
make lib-shared debug
uv run --project sdk/python python sdk/conformance/run.py \
  --artifact build/lib/libtny.0.dylib \
  --report build/conformance/python.json -- \
  python sdk/python/conformance_adapter.py
```

Use `build/lib/libtny.so.0` on supported Linux CI. The adapter drives live
Python SDK turns, permissions, cross-thread cancellation, persistence, auth
errors, the unknown-event decoder fixture, and the Python misuse suite. It also
launches the shared zero-exit steering, backpressure, and every-split-boundary
fixtures; those results are never inferred or self-attested.

To certify a bundled wheel itself, install it into a clean environment and set
`TNY_CONFORMANCE_USE_INSTALLED=1`. Pass the wheel path as `--artifact`; the
adapter imports only the installed package, resolves `tny.Library()` from its
bundled `.libs`, uses that native path for live/shared probes, and reports the
independently checked wheel SHA with `artifact.kind` set to `wheel`.

The release-gate test creates its own venv, installs the named wheel, invokes
the canonical runner, and requires all ten v1 scenarios to pass:

```sh
TNY_TEST_BUNDLED_WHEEL=/absolute/path/to/tny-*.whl \
  python3 -m unittest sdk/python/tests/test_bundled_conformance.py -v
```
