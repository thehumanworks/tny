# ADR 0137: Lazy, selective workflow context

- Status: Accepted
- Date: 2026-09-18
- Issue: #159; independent SDK reporting subset of #158

## Decision

Keep Python and Node workflow APIs ephemeral. Durable jobs remain a separate
API; do not add a checkpoint store, worker daemon, provider loop, or implicit
paid summarization layer.

Resolve dependency status before admission, but compose input only inside the
local semaphore slot. Cancellation must not construct waiting inputs. Preserve
runner signatures, declared edge order, ordering-only edges, default raw-output
framing, failure isolation, partial results, and blocked descendants.

Add explicit edge selection: caller-supplied summary, top-level JSON fields,
and content-addressed in-memory artifact references with exact byte slices.
Keep the complete original in each task result. Provenance identifies task,
session (when available), original size and SHA-256. It is neither trust nor
execution verification. Original output and all selected material stay untrusted.

A reference is not a local path that a remote worker is assumed to share.
Explicit slices are base64-encoded into the prompt; host applications also have
an exact bounded read API. Metadata-only references are allowed but do not
install an agent retrieval tool. Applications must supply an explicit tool and
transport for dynamic retrieval. This limitation is preferable to silently
inaccessible paths or a new persistent store. Artifact lifetime is the lifetime
of the SDK result, not a durable run.

Retain the default 1,048,576-byte dependency payload bound and its raw-output
semantics. Add a positive, separately configurable 2,097,152-byte complete-input
bound that includes roots, base prompt, metadata and framing. This deliberately
rejects formerly accepted oversized base prompts. Existing ordinary workflows
keep byte-identical input; no automatic truncation or fallback occurs. A third
1,048,576-byte selection-read bound limits JSON parsing and requested artifact
slices. Public artifact reads default to 65,536 bytes. Token estimates are not
exposed without a reliable provider tokenizer.

Shell shares the complete-input bound and retains existing output/no-context
edges and file-backed results. Selection is a native Python/Node SDK feature,
not a shell flag or browser/wasm feature. Inline slices cross provider/SSH/
workspace boundaries without assuming shared filesystem visibility.

For the independent #158 subset, retain the last available native usage event
per task and sum it once per task, never per consuming edge. Absent events and
absent costs remain unknown. Blocked tasks are excluded. This is reporting, not
shared admission, durable retry deduplication, cost enforcement or a hard budget.
Exceptions that escape native execution/cleanup can still leave usage unknown.

## Evidence and reproduction

Base: `89bcd5918da0e225e1806813a206d007daafac0a`.
Fixture: one 262,144-byte producer, 32 direct consumers, concurrency one, first
consumer held at an explicit barrier. No network/provider calls. Python wraps
`_render_prompt`; JS counts `_prompt()` calls at composition and measures the
actual admitted prompt's UTF-8 byte length. All consumers use identical input.
The JS counter does not force waiting V8 rope strings to flatten.

Build prerequisites: `make test-sdk-python test-sdk-typescript` (native libtny,
cffi, Node addon and SDK conformance; use the pinned project toolchain).

```sh
base=$(mktemp -d)
git show 89bcd5918da0e225e1806813a206d007daafac0a:sdk/python/src/tny/workflow.py > "$base/workflow.py"
cp -R sdk/typescript "$base/typescript"
git show 89bcd5918da0e225e1806813a206d007daafac0a:sdk/typescript/dist/index.mjs > "$base/typescript/dist/index.mjs"
# Repeat each command five times, in fresh processes.
PYTHONPATH=sdk/python/src python3 sdk/python/tests/bench_workflow_context.py "$base/workflow.py"
PYTHONPATH=sdk/python/src python3 sdk/python/tests/bench_workflow_context.py
node --expose-gc sdk/typescript/test/bench-workflow-context.mjs "file://$base/typescript/dist/index.mjs"
node --expose-gc sdk/typescript/test/bench-workflow-context.mjs
```

Measured on Darwin arm64, Python 3.14.7, Node v26.8.2. Medians of five fresh
processes (bytes are decimal); Python peak is tracemalloc over the complete
fixture, JS heap peak is sampled at composition/runner/barrier boundaries, RSS
is process high-water including native SDK loading. These are not live TTFT
measurements or tokenizer measurements.

| Metric | Python baseline | Python candidate | JS baseline | JS candidate |
| --- | ---: | ---: | ---: | ---: |
| Rendered consumers at barrier | 32 | 1 | 32 | 1 |
| Composed bytes at barrier | 8,395,008 | 262,344 | 8,395,008 | 262,344 |
| Total composed bytes | 8,395,008 | 8,395,008 | 8,395,008 | 8,395,008 |
| Peak traced/sampled heap bytes | 8,739,350 | 607,440 | 11,401,064 | 7,745,744 |
| Peak RSS bytes | 43,958,272 | 35,012,608 | 72,597,504 | 69,369,856 |
| Barrier latency ms | 1.892 | 1.489 | 1.606 | 1.541 |
| Complete fixture latency ms | 2.959 | 3.123 | 3.193 | 3.700 |

Only the admitted consumer is composed while blocked. Python traced memory and
process RSS decline in this fixture. JS sampled peak heap and process RSS also
decline; JS **barrier heap does not decline**, since the old implementation can
retain cheap ropes and the new implementation accounts whole-input bytes.
Total serialization volume is unchanged. Completion latency is slightly higher
(Python) and higher (JS), including new hashing/accounting. Do not claim a speedup.

The workflow tests include both 32-consumer barriers, selective context,
original-artifact retention, provenance, exact/range bounds, UTF-8 accounting,
framing overhead, no-context, cancellation, failures, and usage unknowns. The
existing SDK suites cover native fixtures and runner contracts. Shell checks
cover shared context semantics under Bash and Zsh. Both barrier regression tests
were also run against the isolated baseline modules: each exited 1 specifically
on `32 != 1` rendered consumers (not an import/load error).

Final worker checks:

- `make test-sdk-python test-sdk-typescript test-shell-workflows`: exit 0.
  Python: 93 tests, one installed-wheel test skipped because
  `TNY_TEST_BUNDLED_WHEEL` was unset. Node: 51 tests, no skips. Both SDK
  conformance adapters passed. Native libtny and the Node addon were built;
  cffi 2.1.1 was available. Bash 3.2.57 and Zsh 5.9 passed.
- `make quality`: exit 0 after correcting Python formatting. Later SDK-only
  refinements were rechecked with `make format-check lint-py lint-sh
  lint-workflows lint-js` (exit 0); native inputs were unchanged. GCC analyzer
  was explicitly skipped on Darwin, as expected.
- `npx --yes --package typescript tsc --noEmit --strict --target ESNext
  --module NodeNext sdk/typescript/dist/index.d.ts`: exit 0. An earlier ES2022
  invocation exited 2 because the existing declarations require AsyncDisposable.
- `MYPYPATH=sdk/python/src uvx mypy --strict
  sdk/python/tests/typecheck_usage.py`: exit 0, including artifact and usage types.
- Both benchmark scripts: five baseline and five candidate fresh-process runs
  per language, all exit 0. No network calls in the benchmarks.
- Earlier full SDK run exited 2 because new enumerable JS selector defaults
  changed the existing public edge shape. The defaults are now non-enumerable;
  the original compatibility test and final SDK suites pass. An initial command
  used nonexistent `sdk-python-test`/`sdk-typescript-test` make targets (exit 2);
  the correct targets above ran successfully. Initial `make quality` exited 2
  on formatting, corrected before the successful run.

No root `make test`, leak gate, release-size check, Nix build, or live-provider
performance claim is made by this SDK-only worker. Full integrated gates and
independent post-integration review remain the lead's responsibility.

Integration must include `sdk/python` and `sdk/typescript` in `nix/source.nix`
(the existing test fileset includes only `sdk/conformance` and `sdk/schema`).
Add cffi to the Nix Python environment and verify the native Node headers are
available to `sdk/typescript/scripts/build.mjs`. Run the existing
`test-sdk-python` and `test-sdk-typescript` targets from the chosen SDK check;
no new Make target is needed. Existing workflow tests are already discovered by
those targets. Shell tests already have Nix source/tool/target registration.

Both new benchmark scripts live inside those SDK directories and need explicit
invocations if they are part of a Nix check; they are not default test discovery
entries. A baseline comparison in a sandbox needs the pre-exported baseline
module as an input, not an assumed Git object database or network access. The
delivery lead owns these build/Nix changes and full integrated native/leak/
platform gates. No such registration is claimed in this branch.
