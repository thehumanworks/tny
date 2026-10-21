# 0215 — Native TypeScript sessions with private tool workers

Date: 2026-10-21
Status: accepted; platform acceptance remains a separate gate

## Goal and measure

Make the existing native TypeScript SDK useful for software engineering without
wrapping the tny CLI. Verify mocked provider turns can read/write files, stream,
resume and cancel from an installed package with no CLI executable available.

## Decision

Retain the Node-API addon, one libtny owner thread, shared native provider loop,
session store and pull-driven events. Do not add a JavaScript agent loop or
parse CLI output. ABI 1.5 adds `tny_runtime_set_execution_helper` and a
`TNY_CAP_FEATURE_CODE_EXECUTION` bit without changing any frozen record.
The setter copies an absolute trusted path on the owner thread before a session
exists. It rejects host/custom callback configurations and unsupported hosts.

A separately packaged `tny-executor` links the same headless native code and
embedded CPython. It has exactly four private protocol entry points: execution
server, parallel read worker, command guardian and Python cell. It has no public
CLI commands, provider login, inference entry point or runner interface.
Only the initial libtny-to-worker launch uses the captured path. Nested workers
self-exec their own executable using existing process ownership/teardown rules.
No process-global path or environment override selects a helper.

The TypeScript loader requires ABI 1.5 payloads to include a matching,
executable, architecture-checked and SHA-256-verified adjacent helper. Before
libtny sends any runtime snapshot, the helper identifies protocol 1 and its exact
build version over the existing framed private socket. There is no PATH lookup,
CLI fallback, direct execution fallback or retry of uncertain effects. A C
embedder supplying the setter path is explicitly trusting that executable; the
identity handshake detects mismatches, not malicious code.

`Runtime.create` enables the bundled helper for native HTTP by default;
`codeExecution: false` retains inference-only behavior. `true` requires a bundled
helper and rejects ACP. Older packages can still run inference without one.
`capabilities.codeExecution` means the runtime is configured for native workers,
not that every future OS launch or tool will succeed.

## Preserved boundaries

The restored execution context retains `library_mode`. File/search/edit and web
tools use their existing native implementations. Session jobs, subagents,
teams, terminal, ambient skills/MCP, interactive questions and multimedia agent
tools remain unavailable. Standalone Toolkit methods are unchanged.
Custom JavaScript tools and host-service callbacks are not reconstructed in a
child and remain unsupported on this path. Registering custom callbacks after
helper configuration is rejected instead of silently losing them.

Nested ask-mode calls that need approval deny when no synchronous permission
owner exists. `respondPermission` is not a bridge into this blocked owner stack.
Direct Python retains the OS user's host authority as in ADR 0180; this is not a
sandbox and nested permission rules do not mediate direct Python effects.
Cancellation, unknown-outcome errors, process cleanup and no replay reuse the
existing execution protocol. Browser/wasm remains unsupported.

## Cost and acceptance

The platform SDK now includes the native worker and embedded CPython in addition
to the addon/library. This deliberately trades package size for reuse of tested
isolation and tool semantics, not a second engine. Inference is in-process;
calling tools creates isolated native workers. “Native” does not mean every
operation is subprocess-free.

Acceptance must measure the packaged artifact and exercise both HTTP wires,
streaming, resume without replay, cancellation and fail-closed permissions with
no tny CLI on PATH. Wrong-version and missing-identity helpers must receive zero
runtime-context bytes. Verification must cover the exact source and packaged artifact under review;
a local pass does not substitute for hosted Linux/macOS gates.
