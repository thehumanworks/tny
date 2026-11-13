# Implementation plan

The original implementation phases are historical. Optional ACP client support
is restored by [ADR 0164](adr/0164-optional-acp-clients.md), with acceptance tracked
in [ACP verification](verification/acp-client/README.md). The following native
HTTP migration steps remain historical and do not prohibit optional ACP clients. Current provider scope is
[ADR 0152](adr/0152-native-http-only-providers.md): one native HTTP backend,
Responses and Chat Completions, environment-key gateways, Codex subscription
OAuth, and Grok public/subscription HTTP. No agent executable or ACP server.

1. Remove obsolete protocol implementations, vendor code and process machinery.
2. Keep native tools, MCP, sessions, runners, jobs, workflows and the C ABI.
3. Reject removed selectors/settings and migrate BYOK settings to env names.
4. Verify both HTTP wires, subscription login/refresh and no-binary operation.
5. Update current CLI/TUI, SDK, site, build/Nix/CI contracts together.
6. Run release/library builds, `make test`, `make quality` and report measured
   artifact size/dependencies in the verification directory.

The optional instruction experiment (ADR 0153) keeps independent evaluation,
explicit promotion and held-out reporting. Automatic learning was removed by ADR 0221.

Private C++20 remains scoped by ADRs 0114, 0126 and 0133. Other application and
OS seams stay C11. No public event or C ABI layout changes. wasm builds share
the native HTTP tool loop through fetch and `tny_poll`. Streaming decoders must
retain split-boundary tests. Use fixtures and synthetic credentials, never
live inference without authorization. Nix invokes the same Makefile.

## Execution server and code-only tools (ADR 0174)

Replace the provider-facing native tool registry with `run_code`, preserve the
filtered nested catalog, and execute each bounded Python cell in a fresh process
of the user's python3 ([ADR 0179](adr/0179-python-code-mode-cpython.md),
[ADR 0222](adr/0222-host-python3-code-cells.md)) with the OS user's host
authority ([ADR 0180](adr/0180-host-authorized-python-code-cells.md)). Without
a usable python3 the native tools stay model-facing.
Native Chat/Responses and verified ACP share the same authority checks. The
[acceptance contract](verification/execution-code-mode/contract.md) and
[evidence ledger](verification/execution-code-mode/evidence.md) track protocol,
policy, platform and regression proof separately. Wasm returns a clean
unsupported-execution error; no direct-tool fallback is permitted.
