# 0050 — Implement the complete public Cursor SDK Bridge v1 contract

Date: 2026-08-31
Status: accepted

## Context

ADR 0044 rejected Cursor's private `agent.v1` HTTP/2 protocol and retained the
supported SDK Bridge. tny initially used only the narrow turn path: Ping,
ListModels, Create/Resume, Send, Cancel, and Shutdown. That left supported
public functionality—cloud agents, durable run recovery, images, modes, MCP,
subagents, tool selection, custom tools/stores, artifacts, usage, and
management—unavailable even though sdk.v1 already defined it.

Cursor SDK Bridge v1.0.30 publishes an additive stable contract at tag object
`026d21b23641ee488a6650ba850327b8a66ab1cd`, commit
`8157597c625b5f642d3c4a1472d20c9c330a9d18`. Its manifest points to SDK source
commit `a401fe7f346d4d3ba66fd596cc842b0ad5e5259c`. The release contains 5 services,
29 RPCs, 114 messages, 285 fields, 8 enums, and 5 oneofs.

## Decision

Implement the complete v1.0.30 public contract while keeping the bridge an
external host:

- vendor the release schema plus a deterministic contract manifest and check
  its hashes/counts in CI;
- negotiate Ping/GetVersion and capability-gate the complete 27-RPC outbound
  route table;
- support local/cloud AgentOptions, SendOptions, images, modes, MCP/subagents,
  tool allow/deny, durable run observation, structured errors, artifacts, and
  usage;
- host authenticated loopback `CallCustomTool` and `CallStore` services for
  local agents, with bounded blocking-pump and normal event-loop ownership;
- store a versioned Cursor session pointer with agent/run/offset/runtime and
  reconnect dropped Send streams through ObserveRun without duplicating
  events;
- expose readable `tny cursor` management commands plus a route-checked raw
  JSON RPC escape hatch; require `--yes` for deletion;
- expose the conversational Cursor provider through libtny and its Python and
  TypeScript schedulers using the normal create/resume/send/cancel/custom-tool
  runtime API. Cursor embedding requires explicit state directory, API key,
  and model; provider management remains CLI-only.

`settings.cursor` is a trusted user-level configuration surface. Repository
configuration remains a limits-only, no-authority surface and cannot add
credentials or Cursor execution configuration.

## Security and ownership

Cursor remains a host backend. Cursor owns built-in tools, its sandbox/review,
MCP, subagents, and remote retention. tny owns transport validation, session
mapping, explicitly registered custom tools, and an optional local custom
store. There is no per-call approval RPC for Cursor built-ins.

Custom callback servers bind loopback, require independent bearer auth, accept
only exact Connect paths, accept valid fixed-length or chunked JSON bodies,
reject malformed or oversized framing, bound all results, and fail closed.
Sensitive custom tools require yolo mode. Store callbacks can execute on a
bounded pump thread during blocking bridge RPCs; custom tools cannot. Ephemeral
mode strips persistent local-store configuration and writes no tny session,
but makes no claim about bridge/Cloud retention.

## Consequences

- The supported bridge provides OpenAI-provider-like configuration breadth
  without importing Cursor's private HTTP/2/protobuf executor protocol.
- The C binary gains Connect callback/server and schema-validation code, but
  not Bun, gRPC, HTTP/2, or the private agent schema.
- Native bridge launch uses `posix_spawn` with a parent-built environment and
  file actions. Multithreaded SDK hosts never run non-async-signal-safe setup
  in a post-`fork()` child, and credentials remain absent from argv.
- wasm keeps a clean unsupported error because it cannot spawn the bridge.
- Schema updates must use a release tag and regenerate the checked contract;
  never patch vendored protobuf files by hand.
- Deterministic mocks prove the protocol/state machine without credentials.
  Live Cursor acceptance and billing remain a separate user-key smoke gate.

This ADR supersedes ADR 0044 only regarding how much of **public** `sdk.v1` tny
implements. ADR 0044's rejection of private `agent.v1` remains in force.
