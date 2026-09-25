# Python and TypeScript SDKs

Agent tool execution is currently unavailable in library-hosted sessions under
[ADR 0174](adr/0174-execution-server-code-mode.md): `run_code` fails closed
rather than executing inside the embedding process. Installing a matching CLI
does not enable this path. Custom-tool and host-service callback registration
APIs retain their ABI, but their callbacks are not invoked by this execution
path. No-tool inference and standalone SDK toolkit services remain separate.
See the [execution evidence](verification/execution-code-mode/evidence.md) for
verified coverage and remaining gates.

tny ships two language adapters over the same native `libtny` runtime. Neither
adapter reimplements provider wire protocols or the agent/tool loop.

Both also expose a standalone [`Toolkit`](sdk-toolkit.md) for image generation
and editing, speech export/playback, file/microphone transcription, and prompt
optimisation. Python includes `AsyncToolkit`. These methods require ABI 1.2+
and do not create an agent runtime or session.

| SDK | Package | Binding | Scheduler model |
| --- | --- | --- | --- |
| Python | `sdk/python` (`tny`) | cffi ABI mode | sync owner thread or one dedicated asyncio executor |
| TypeScript | `sdk/typescript` (`@thehumanworks/tny`) | C Node-API addon | one dedicated native owner thread per runtime |

Both can load stable ABI 1.0 and its frozen sized prefixes, copy every borrowed event and
capability field before freeing its owner, expose unknown events explicitly,
and validate `tny_abi_version()` before creating runtime state. Capability
snapshots—not platform guesses—govern supported behavior.

Task presets are first-class runtime configuration in both adapters. Python
uses `RuntimeConfig(task_preset=TaskPreset("review"))`; TypeScript accepts
`Runtime.create({ workspace, taskPreset: "review" })`. Built-ins are `review`,
`optimizer`, `document`, `retro`, and `task-creation`; an explicit `{name, instructions}` form
supplies custom instructions. SDK runtimes never perform ambient task-file
discovery, preserving deterministic embedding.
Task creation requires ABI minor 1; against an ABI 1.0 library both SDKs keep
using the v1 creation path when no task is requested and raise an explicit
unsupported-feature error when one is.

Model and reasoning effort are per-runtime configuration too. Python uses
`RuntimeConfig(model="...", reasoning_effort="high")`; TypeScript accepts
`Runtime.create({ workspace, model: "...", reasoningEffort: "high" })`. The
canonical levels `off`, `light`, `medium`, `high`, `xhigh` and `max` map to the
provider's wire word; any other `[A-Za-z0-9_.-]{1,32}` token is sent verbatim,
and omitting it leaves the provider default. The CLI's
`TNY_REASONING_EFFORT` and settings defaults are never read. Effort requires
ABI minor 3 ([ADR 0163](adr/0163-sdk-reasoning-effort.md)); without one, both
SDKs keep their earlier creation paths, so older libraries still load. Because
every workflow task may carry its own runtime configuration, a graph can mix
models and efforts per role — see `examples/sdk/models.json`.

## Python

The Python package provides context-managed `Runtime` and `Session` objects,
typed immutable events and errors, sync iterators, asyncio adapters,
permissions, steering, cross-thread cancellation, and a high-level `Workflow`
DAG that uses one independent runtime per concurrently active task. Text and identifiers
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
`AbortSignal` cancellation, explicit async disposal, canonical typed events,
and a high-level `Workflow` DAG that uses one independent runtime per
concurrently active task. The C addon owns a bounded command/completion queue; all owner-affine
libtny calls run on its native owner thread, while ABI-1 cancellation uses
the runtime wake path.

Cross-thread cancellation applies after a turn has started. A first provider
connect/TLS handshake remains an owner-thread operation bounded by the native
15-second deadline; environment teardown may wait for that bound rather than
preempting an in-progress handshake.

Registry packaging uses one platform-neutral `@thehumanworks/tny` meta package
with exact-version optional dependencies on
`@thehumanworks/tny-darwin-arm64`, `@thehumanworks/tny-linux-x64`, and
`@thehumanworks/tny-linux-arm64`. Each platform package contains exactly one
matched addon, libtny library, and integrity manifest. The loader rejects a
missing, wrong-platform, wrong-version, ABI-incompatible, or SHA-mismatched
payload before loading native code. Source compilation remains an explicit
checkout fallback requiring libtny headers and a C11 compiler. See
[`sdk/typescript/README.md`](../sdk/typescript/README.md).

## Workflow orchestration

Both SDKs validate the complete graph before starting native work, run ready
nodes under a positive concurrency limit, inject direct dependency output in
declared edge order, and mark descendants of failed tasks as blocked while
independent branches continue. Each dependency edge independently controls
whether its successful output is injected, so one fan-in can mix context and
ordering-only dependencies.
Task failure remains represented in the aggregate result until the caller asks
it to raise.

Python offers `Workflow.run_async()` plus a sync `run()` wrapper; TypeScript
offers `Workflow.run({ signal })`. Native permission requests default to deny
unless a workflow permission callback returns a decision. Prompts, credentials,
outputs, and underlying exception text are omitted from representations and
aggregate errors. Both SDKs expose a custom runner seam for adapters and tests.

See [workflows.md](workflows.md) for complete examples, status semantics,
context bounds, cancellation, and the shell equivalent.

For worked, runnable workflows in both languages — an auto-research loop and a
code generation pipeline — see [`examples/sdk/`](../examples/sdk/README.md).

## Platforms and authority

The embedded runtime supports only native OpenAI-compatible HTTP conversations, with Responses and Chat Completions. Configure endpoints and credentials explicitly in memory. Shared C ABI constants remain stable; removed provider bits are unavailable. Custom tools remain supported where the capability snapshot advertises them.

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

Release versions have one authority: a canonical `vMAJOR.MINOR.PATCH` tag, or
the deliberately narrow `vMAJOR.MINOR.PATCH-(a|b|rc).N` prerelease form. The
tag body is the npm SemVer; Python receives its canonical PEP 440 equivalent
(for example, `v1.2.3-rc.4` becomes npm `1.2.3-rc.4` and Python `1.2.3rc4`).
Any other tag form, a package-version mismatch, divergent meta packages from
different runners, incomplete platform package set, or artifact hash mismatch
fails the release before GitHub publication. Release jobs also retain offline
package-content reports, source-commit-bound SHA provenance descriptors, and
SPDX SBOM inputs. All native runners use the same exact Node distribution; the
aggregate validator rejects differing Node/npm identities, duplicate native
packages or wheel targets, macOS deployment floors above 13.0, and content
reports which cannot be reproduced from their archives. Artifact metadata
schema 2 records ABI major and SONAME/install-name; Node build manifests use
schema 2, native registry manifests schema 3, and release provenance schema 2.
GitHub releases publish disjoint `libtny1-<triple>.tar.gz` and
`libtny0-compat-<triple>.tar.gz` assets with independent manifests; neither
archive may contain the other ABI major.

The repository currently has no project license grant. Owner-published GitHub
artifacts carry `LicenseRef-UNLICENSED` metadata and third-party notices but
grant downstream recipients no reuse rights. PyPI/npm registry publication
waits for the repository owner to choose a license. Trusted-publisher jobs are
additionally gated by the repository variable
`TNY_REGISTRY_PUBLICATION_ENABLED=true` and the protected `npm` / `pypi`
environments. Even if enabled accidentally, the offline validator fails closed
while the root license or SDK license metadata remains unpublishable.
After a license is adopted, its exact root bytes must be present in every npm
package and wheel. Publication is idempotent: identical existing versions are
verified, while partial or digest-mismatched versions fail. Native npm packages
are verified before the meta package, and final npm/PyPI clean-install,
native-load, download-hash, and import readbacks must both succeed.
