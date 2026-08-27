# `@thehumanworks/tny` native SDK (experimental)

This package is the Node.js/TypeScript binding for experimental **libtny ABI
0.5**. It embeds the native OpenAI-compatible agent runtime through a small C
Node-API addon. It does not spawn `tny` and contains no provider-wire or agent
loop implementation in JavaScript.

The package is intentionally marked `UNLICENSED` until the repository adopts
project licensing. It is not an ABI-1 or production-stability claim.

## Supported systems

- Node.js 20 or newer
- Apple Silicon macOS
- glibc Linux on x64 or arm64

The install script builds the addon against the enclosing tny checkout. For an
installed libtny, set `TNY_INCLUDE_DIR` and `TNY_LIB_DIR`; alternatively set
`TNY_ROOT`. The versioned shared library is staged beside the addon and the
addon uses a loader-relative rpath. Unsupported systems, a missing library, or
an ABI older than 0.5 fail with a targeted diagnostic.

```sh
TNY_ROOT=/path/to/tny npm install /path/to/tny/sdk/typescript
```

## Use

```ts
import { Runtime, PermissionDecision } from "@thehumanworks/tny";

const runtime = await Runtime.create({
  workspace: process.cwd(),
  stateDir: "/tmp/my-tny-state", // required only with persistence: true
  baseUrl: "https://api.openai.com/v1",
  apiKey: process.env.OPENAI_API_KEY,
  permissionMode: "ask",
});

const session = await runtime.createSession();
const answer = await session.ask("Summarize this repository", {
  onEvent: async (event, current) => {
    if (event.type === "permission_request") {
      await current.respondPermission(event.permissionId, PermissionDecision.deny);
    }
  },
});
console.log(answer.text);

await session.close();
await runtime.close();
```

`session.run()` is a pull-driven `AsyncGenerator`. One native `next_event`
request exists only while JavaScript asks for another item, so a slow consumer
does not accumulate an unbounded second event queue. Breaking iteration cancels
and drains the turn. `AbortSignal` requests cancellation; callers still observe
ordered events through `turn_end`.

Handle operations run on the addon's dedicated owner thread. Commands and JS
completions are bounded to 64 ordinary entries, plus reserved session-close,
runtime-close, and pending-demand slots. libtny ABI 0.5 provides cross-thread
cancellation and a wake primitive; scheduler-side cancel/AbortSignal calls use
that operation directly while registry and session-lifetime locks prevent a
concurrent owner close.

The exception begins only after a turn is active. Initial provider connect/TLS
handshake still runs synchronously on the owner and is bounded by libtny's
15-second connect deadline; cancellation or worker-environment cleanup cannot
preempt that pre-turn handshake yet.

## ABI 0.5 limitations

`runtime.capabilities` and `await runtime.getCapabilities()` are copied from
libtny's sized capability view; the latter reflects reachability changes after
ordinary turn traffic. The view reports these limitations explicitly:

- multiple isolated runtimes per process, with one open session per runtime;
- native OpenAI-compatible backend only;
- no public `send_ex`, so image inputs and output schemas throw
  `UnsupportedFeatureError`;
- cross-thread cancellation is available and reported in the capability mask;
- SecureTransport on macOS and dynamically loaded system OpenSSL on glibc
  Linux.

Platform release tarballs bundle a matched addon and shared library with a
loader-relative rpath. Installation verifies hashes, Mach-O/ELF architecture,
ABI/capability size, and the build host's deployment contract. The manifest is
authoritative: current local macOS artifacts have a macOS 27.0 floor; Linux
artifacts record and require their build-time glibc version. No older macOS or
glibc baseline is claimed without a corresponding CI artifact.

Release builds may set `TNY_LIB_DIR`, `TNY_EXPECTED_LIB_SHA256`, and
`TNY_ARTIFACT_METADATA` to bind the addon to a separately staged libtny
artifact. A hash/platform mismatch fails before compilation. Linux manifests
derive referenced GLIBC symbol versions and reject release floors above 2.34.

`npm run footprint` records byte counts and SHA-256 values for the addon,
staged library, and generated `.tgz`; it does not make a performance claim.

Explicit async `close()`/`Symbol.asyncDispose` is the supported lifecycle.
Finalizers request cleanup only as a last-resort safety net.

## Conformance

The source package includes a protocol-v1 Node adapter at
`test/conformance-adapter.mjs`. It reads the canonical request from stdin and
writes exactly one JSON response to stdout; child diagnostics go to stderr.
Executable Node, strict-mock, C split-boundary, and ctypes runs are referenced
by exit-code ID. Results that lack the canonical fixture are reported
`not_run` rather than promoted to self-attested passes.
