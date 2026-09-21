# ADR 0164: Optional ACP clients with an owning-runtime MCP bridge

Status: accepted, 2026-09-21. Supersedes ADR 0152 only for optional ACP clients.

## Decision

Keep the native OpenAI-compatible HTTP backend, its Responses/Chat Completions
wires, subscription authentication, and no-vendor-binary operation. Restore an
**opt-in ACP client**, not an ACP server or the deleted vendor backends.
An external ACP agent owns its inference loop. It is not a model endpoint to
which tny can send native function-call responses.

For each ACP backend, tny creates a private Unix socket inside a newly created
0700 temporary directory. `session/new` and capability-gated `session/load`
receive a mandatory stdio MCP server definition. A small `tny --acp-mcp-bridge`
relay connects that server's stdin/stdout to the owning runtime. It cannot load
another session or independently dispatch tools. Tool execution stays on the
runtime's event loop and uses its borrowed context, session, permissions,
frontend controls, custom-tool registry and extension control callback.
The relay uses the existing host process and Unix socket seams; wasm rejects
ACP before starting a process. No additional OS abstraction or public struct
layout is introduced. Native embedders explicitly supply the tny relay path
with `TNY_ACP_BRIDGE_EXECUTABLE`; the embedder executable is never mistaken for tny.

The command setter and three additive usage getters ship in ABI 1.4's
`LIBTNY_1.4` node, inheriting `LIBTNY_1.3`. Existing symbols retain their
original nodes; frozen public layouts and the ABI 1.0 baseline stay unchanged.
The active ABI baseline/signature manifest and release metadata record this
minor-version addition independently of product SemVer (ADR 0037).

`tools/list` converts the current native function schema to MCP metadata without
changing parameter schemas or descriptions. `tools/call` uses
`tools_call_prepare` and `tools_call_execute`; imported MCP servers remain
available through the same native MCP discovery/call tools. Custom asynchronous
tools use the existing completion registry and wake descriptor. No toy shell or
filesystem implementation duplicates the native registry. Pre-tool rewrites,
permission decisions, post-tool replacement, intercepts and frontend control
pumping use the shared runtime seams. These controls cover bridge tools, not
unobservable provider internals.

## Bounds and authority

The bridge accepts four clients, one outstanding tool call per client, 8 MiB
input/output queues, bounded dispatch work and a five-minute pending-call
execution watchdog; human approval waiting is excluded and approval starts a
fresh execution deadline. Disconnect/cancellation invalidates asynchronous handles. MCP calls
are refused outside an active owning turn. The socket is 0600; directory
permissions isolate other users. This is the same-user trust boundary as local
agent execution, not a sandbox against the account running tny. No bearer token
is placed on command lines or logged. Agent stderr is discarded; setup errors
report protocol categories/codes rather than arbitrary agent error data.

ACP client fs and terminal capabilities stay false; unsupported callbacks return
method errors. Agents can use their own built-in tools. ACP permission requests
are supported, but tny cannot promise its path rules, schema validation, hooks,
or interception govern external built-ins. Model selection must be advertised
and confirmed using config options, or acknowledged via the legacy model API.
Explicit fast tier fails. Reasoning effort requires an advertised thought_level
option and confirmed selection; unsupported values fail rather than silently
changing behavior. Session loading requires `loadSession`.

## Consequences and differences

- HTTP users need no vendor executable. ACP users install and authenticate an
  adapter such as `claude-agent-acp` or a pi ACP adapter separately.
- ACP protocol messages and MCP tool results normalize into existing events.
  External usage reporting is incomplete and must not be presented as full
  native token/cost accounting.
- Native mid-turn checkpoint restart, steering, compaction and provider-wire
  hooks cannot be implemented by pretending to own the external agent loop.
  End-of-turn external session pointers support resume only when advertised.
- `--ssh` is supported for the verified Claude ACP 0.75.1 extension: tools and
  settingSources are empty and strictMcpConfig is true, agent cwd is a private scratch directory, and tny
  workspace dispatch/instructions use the remote seam. Other agents/versions
  fail before session creation because standard ACP cannot require an external
  agent to disable its local built-in tools. This is a compatibility/version
  gate, not a sandbox against a malicious executable.
- Image input requires the agent's image capability. Native captured read-image
  bytes can be returned as MCP image content; native generated-image preview
  continuation and recovery-policy learning remain native-loop features.
- wasm reports a clean unsupported diagnostic; this change does not revive
  obsolete WebSocket transports or claim browser process support.

Source and acceptance evidence: [ACP verification](../verification/acp-client/README.md).

## Managed launch extension

Managed ACP launches persist a required-tools-authority constraint, never a flag
claiming prior verification. Each owned child verifies the pinned Claude adapter
at initialize before session creation/loading. No unaccounted probe runs during
job parsing. DAG/admission and read-only team/swarm scheduling reuse the native
scheduler with frozen ACP selectors/argv/model/effort and an isolated credential
branch. Native platform/workspace/permission/nesting restrictions remain.

The child captures and stops its owned adapter process tree before completion.
A per-attempt private cleanup receipt is published only after observed success;
uncertainty holds cleanup-sensitive claims. This is same-user process accounting,
not executable/account attestation. External login/executable bytes can change;
admission counts launches, not tokens or fees. Positive max-steps uses the
verified adapter's SDK maxTurns semantics; native mid-turn checkpoint restart
remains unavailable.

ACP persists normalized user/assistant text for durable job result accounting;
the external session pointer still owns actual provider history. Swarm activation
and team receipt updates atomically queue context for the next ACP prompt.
Successful end-turn acknowledgement removes the forwarded prefix, while failed
turns retain it. Delivery occurs at tny prompt boundaries, not unobservable
external model-call boundaries.
