# Implementation plan

The original implementation phases are historical. Current provider scope is
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
