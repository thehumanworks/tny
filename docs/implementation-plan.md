# Implementation plan

The original implementation phases are historical. Optional ACP client support
is restored by [ADR 0164](adr/0164-optional-acp-clients.md), with acceptance tracked
in [ACP verification](verification/acp-client/README.md). The following native
HTTP migration steps remain historical and do not prohibit optional ACP clients. Current provider scope is
[ADR 0152](adr/0152-native-http-only-providers.md): one native HTTP backend,
Responses and Chat Completions, environment-key gateways, Codex subscription
OAuth, and Grok public/subscription HTTP. No agent executable or ACP server.

1. Remove obsolete protocol implementations, vendor code and process machinery.
2. Keep native tools, MCP, sessions, runners, jobs, teams, workflows and the C ABI.
3. Reject removed selectors/settings and migrate BYOK settings to env names.
4. Verify both HTTP wires, subscription login/refresh and no-binary operation.
5. Update current CLI/TUI, SDK, site, build/Nix/CI contracts together.
6. Run release/library builds, `make test`, `make quality` and report measured
   artifact size/dependencies in the verification directory.

Automatic recovery-policy learning is default-on in the native workflow
([ADR 0154](adr/0154-default-automatic-workflow-learning.md)). Use typed execution
facts and bounded workspace evidence; preserve task and permission authority.
The optional broader instruction experiment remains separate (ADR 0153), with
independent evaluation, explicit promotion and held-out reporting.

Private C++20 remains scoped by ADRs 0114, 0126 and 0133. Other application and
OS seams stay C11. No public event or C ABI layout changes. wasm builds share
the native HTTP tool loop through fetch and `tny_poll`. Streaming decoders must
retain split-boundary tests. Use fixtures and synthetic credentials, never
live inference without authorization. Nix invokes the same Makefile.

## Collective swarm extension (ADR 0156)

[Review continuity](swarm-review.md) adds bounded immutable review packets,
profile-aligned collaboration guidance and observational mailbox capacity without
turning claims into acceptance. Current gates and deferred roadmap items are in
[review-continuity evidence](verification/swarm-review/evidence.md).

Mode, shared admission, atomic run publications and native mailbox waits extend the
existing team infrastructure. Acceptance status and remaining platform/gate evidence
are tracked in [collective-swarm evidence](verification/collective-swarm/evidence.md).

## Purposeful swarm definitions (ADR 0157)

Strict versioned JSON definitions add named purposes and bounded nested groups. The
validated tree compiles to one existing durable team, with a single admission scope
and persisted canonical provenance. Implementation evidence and remaining platform
gates are tracked in [purposeful-swarms evidence](verification/purposeful-swarms/implementation.md).

## Execution server and code-only tools (ADR 0174)

Replace the provider-facing native tool registry with `run_code`, preserve the
filtered nested catalog, and execute each bounded Lua cell in a fresh process.
Native Chat/Responses and verified ACP share the same authority checks. The
[acceptance contract](verification/execution-code-mode/contract.md) and
[evidence ledger](verification/execution-code-mode/evidence.md) track protocol,
policy, platform and regression proof separately. Wasm returns a clean
unsupported-execution error; no direct-tool fallback is permitted.
