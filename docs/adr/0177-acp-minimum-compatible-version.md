# ADR 0177: Minimum-compatible Claude ACP admission

Status: accepted. Supersedes only the exact adapter-version pin in ADRs 0164/0174.

## Problem and evidence

`ac_connect` compared the adapter's initialize `agentInfo.version` to the literal
`0.75.1`. Consequently the user's global mise package, version `0.81.2`, was
rejected before session creation with a misleading identity-mismatch message.
This was a tny admission policy, not an ACP protocol version mismatch.

The installed `@agentclientprotocol/claude-agent-acp` package reports `0.81.2`
via both `--version` and an initialize-only JSON-RPC exchange, with protocol `1`
and `loadSession: true`. Its `dist/acp-agent.js` still allows `tools`,
`settingSources`, and `strictMcpConfig` in `_meta.claudeCode.options`; explicit
`tools` overrides the legacy `disableBuiltInTools` shorthand, and user-provided
options override default setting sources in SDK query creation. The installed
package pins Claude Agent SDK `0.3.280`. No inference or account turn was used
for this investigation. See [sources](../sources.md).

## Decision

Admit exactly `@agentclientprotocol/claude-agent-acp`, with a canonical **stable
SemVer >= 0.75.1**, and require negotiated ACP protocol version `1` as before.
Compare the three numeric components, not strings. There is no upper version
cutoff: `0.75.2`, `0.81.2`, `0.100.0`, and `1.0.0` satisfy the version rule.

- Core components are ASCII decimal uint32 values, without leading zeros except
  `0`. Overflow, missing fields/components, `v` prefixes, signs, whitespace,
  embedded JSON NUL suffixes and trailing data fail closed.
- Optional `+build.metadata` consists of nonempty dot-separated ASCII
  alphanumeric/hyphen identifiers. It is validated but ignored for ordering.
- Prereleases (even above the floor) fail closed. They are not evidence of
  stable tools-only support. Other adapter identities remain unsupported.
- Keep `tools: []`, `settingSources: []`, `strictMcpConfig: true`, private scratch
  cwd, disabled ACP fs/terminal callbacks, and the singleton tny `run_code`
  bridge on **both** session creation and loading. No direct-tool fallback.
- Verification belongs to a live initialize handshake, never to a saved session.
  Disconnect/managed stop clears it; reconnect checks identity/version/protocol
  again. Resume also requires the new process to advertise `loadSession`.
- Diagnostics name the required identity and stable minimum; an unsupported
  version is no longer reported solely as an identity mismatch.

This replaces a known-support release **pin** with a compatibility **floor**.
Neither rule attests the executable, authenticates self-reported metadata, or
proves future releases retain the extension. Standard ACP offers no negotiated
capability acknowledging all these Claude-specific options. We assume stable
upstream releases retain that contract, based on the inspected newer release;
a future breaking release needs a policy revision, not a silent fallback.
Self-reported version admission is not a sandbox against malicious executables.

Native C11 implements the small allocation-free policy. No public ABI changes.
Wasm retains its existing clean unsupported-ACP error before spawning; this is
not browser ACP support. Managed/SSH/embedding use the same handshake gate.

## Verification contract

`tests/formal/acp/` is a standalone pinned Lean 4 project, with no Mathlib,
`sorry` or added axioms. It proves numeric floor acceptance, rejection below
it, upward closure, exact identity and build-metadata irrelevance. Its abstract
lifecycle proves admitted/protocol/tools-only invariants for reachable active
states, failure blocking sessions/prompts, load capability guards, and fresh
verification after disconnect, downgrade or identity changes.

`make verify-acp-proofs` checks the proofs and regenerates golden tables, failing
on drift. CI runs it in `lean-proofs`. Ordinary `make test` needs no Lean:
`tests/test_acp_compat.c` replays the version table against production C and
adds numeric/malformed/overflow checks. `test_admission_transitions_match_lean`
replays the lifecycle table over the real subprocess protocol, including resume
into a changed adapter. The ACP integration, managed-job and embedding fixtures
use synthetic `0.81.2` by default, retaining minimum-release coverage. Newer
versions must still carry tools-only metadata and bridge operations/permissions.
Mutation tests target the numeric floor, parser, identity and handshake gate.

These are proofs of the specification with concrete implementation conformance
checks, **not** proofs of the C parser, transport, external SDK or isolation.
No live model compatibility or performance claim follows from these tests.
