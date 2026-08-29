# `@thehumanworks/tny` native SDK

This package is the Node.js/TypeScript binding for stable **libtny ABI 1**. It embeds the native OpenAI-compatible agent runtime through a small C
Node-API addon. It does not spawn `tny` and contains no provider-wire or agent
loop implementation in JavaScript.

The package is intentionally marked `UNLICENSED` until the repository adopts
project licensing.

## Supported systems

- Node.js 20 or newer
- Apple Silicon macOS
- glibc Linux on x64 or arm64

Registry releases use one JavaScript-only `@thehumanworks/tny` meta package.
It has exact-version optional dependencies on the three native payloads:
`@thehumanworks/tny-darwin-arm64`, `@thehumanworks/tny-linux-x64`, and
`@thehumanworks/tny-linux-arm64`. npm selects the compatible payload. The
runtime validates its package name and version, target, ABI manifest, and both
native SHA-256 values before loading the addon. Missing optional packages,
musl, the wrong CPU, tampering, and any ABI major other than 1 produce targeted
`TnyLoadError` diagnostics.
Platform packages contain only `libtny.1.dylib` or `libtny.so.1`; the loader
never searches for ABI major 0 and rejects a manifest which claims it.

The repository checkout is deliberately private/non-publishable and has no
automatic install build. Source users explicitly build against the enclosing
checkout, or set `TNY_INCLUDE_DIR` and `TNY_LIB_DIR` (alternatively `TNY_ROOT`)
for an installed libtny. The versioned shared library is staged beside the
addon and uses a loader-relative rpath.

```sh
cd /path/to/tny
TNY_ROOT="$PWD" npm --prefix sdk/typescript run build
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
runtime-close, and pending-demand slots. libtny ABI 1 provides cross-thread
cancellation and a wake primitive; scheduler-side cancel/AbortSignal calls use
that operation directly while registry and session-lifetime locks prevent a
concurrent owner close.

The exception begins only after a turn is active. Initial provider connect/TLS
handshake still runs synchronously on the owner and is bounded by libtny's
15-second connect deadline; cancellation or worker-environment cleanup cannot
preempt that pre-turn handshake yet.

## Dependency workflows

`Workflow` adds a validated DAG over independent native runtimes. Ready tasks
run concurrently up to `maxConcurrency`; successful direct-dependency outputs
are appended in declaration order before a consumer starts.

```ts
import { PermissionDecision, Workflow } from "@thehumanworks/tny";

const workflow = new Workflow({
  runtime: {
    workspace: process.cwd(),
    baseUrl: "https://api.openai.com/v1",
    apiKey: process.env.OPENAI_API_KEY,
    permissionMode: "ask",
  },
  maxConcurrency: 2,
  onPermission: () => PermissionDecision.deny,
});

workflow
  .task("architecture", "Audit the architecture")
  .task("tests", "Audit the tests")
  .task("implement", "Implement and verify the change", {
    dependsOn: ["architecture", "tests"],
  });

const result = await workflow.run();
result.raiseForFailure();
console.log(result.output("implement"));
```

Failed tasks block descendants without cancelling independent branches.
`includeDependencies: false` makes an edge ordering-only;
`maxDependencyBytes` bounds direct-output fan-in. Results retain status,
output, session id, stop reason, blocked dependencies, and an explicit error,
while representations and aggregate errors omit prompts, credentials, output,
and underlying exception text.

`workflow.run({ signal })` propagates `AbortSignal` cancellation to every active
native task and waits for cleanup. Native permission requests default to deny.
A custom `runner(task, prompt, { signal })` can replace native execution while
retaining the scheduler; it returns `WorkflowTaskExecution` or the equivalent
object shape and must honor the signal.

See [`docs/workflows.md`](../../docs/workflows.md) for the Bash/Zsh surface,
complete result semantics, and safety guidance.

## ABI 1 limitations

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

Each platform package bundles a matched addon and shared library with a
loader-relative rpath. Installation verifies hashes, Mach-O/ELF architecture,
ABI-1 major/capability size, SONAME/install-name, and the build host's deployment contract. Release CI sets
the macOS 13 deployment target; Linux artifacts record and require their
derived glibc floor, which may not exceed 2.34.

Release builds may set `TNY_LIB_DIR`, `TNY_EXPECTED_LIB_SHA256`, and
`TNY_ARTIFACT_METADATA` to bind the addon to a separately staged libtny
artifact. Artifact metadata schema 2 and build manifest schema 2 bind ABI
major 1, library version, hashes, and SONAME/install-name; registry manifests
use schema 3 to add exact package identity. A mismatch fails before compilation. Linux manifests
derive referenced GLIBC symbol versions and reject release floors above 2.34.

`npm run footprint` records byte counts and SHA-256 values for the addon,
staged library, and generated `.tgz`; it does not make a performance claim.

Package versions come only from the release tag. Stable tags use
`vMAJOR.MINOR.PATCH`; prereleases use the deliberately narrow
`vMAJOR.MINOR.PATCH-(a|b|rc).N` form. CI creates a byte-identical meta package
on every native runner, one target-specific payload per runner, offline package
content reports, source-commit-bound SHA provenance inputs, and SPDX SBOM
inputs. Registry publication verifies existing versions by integrity, publishes
all three verified native payloads before the meta package, and ends with a
clean registry install/native-load readback. A partial rerun resumes only when
every existing package digest is identical. Registry
publication remains disabled until the repository owner adopts a license and
configures the protected trusted-publisher environment.

Explicit async `close()`/`Symbol.asyncDispose` is the supported lifecycle.
Finalizers request cleanup only as a last-resort safety net.

## Conformance

The source package includes a protocol-v1 Node adapter at
`test/conformance-adapter.mjs`. It reads the canonical request from stdin and
writes exactly one JSON response to stdout; child diagnostics go to stderr.
Executable Node, strict-mock, C split-boundary, and ctypes runs are referenced
by exit-code ID. Results that lack the canonical fixture are reported
`not_run` rather than promoted to self-attested passes.
