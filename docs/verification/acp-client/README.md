# ACP client restoration verification contract

Base revision: `ee4aa6c9a81395c8260f7b9c1536aeba16027fd9`.
Implementation and delivery use the isolated `feat/restore-acp-client` worktree.
Final follow-up gates are recorded in [delivery-validation.json](delivery-validation.json). Current results belong in [evidence.md](evidence.md).
Raw logs: `/Users/tomas/.cache/tny-acp-20260921/`.

## Acceptance requirements

| ID | Requirement | Evidence boundary |
| --- | --- | --- |
| A1 | Optional, lazy ACP client; native HTTP unchanged | CLI/config tests, release/library builds, complete suite |
| A2 | Bounded split-safe stdio RPC, model/capability validation | deterministic adapter mocks, mutation tests |
| A3 | Real MCP bridge into owning runtime | exact schema equality, file/shell roundtrips, tool failures |
| A4 | Shared permissions/hooks/intercepts and async custom tools | permission denial side effects, custom sync/async/cancel, extension tests |
| A5 | CLI/TUI/settings/resume/runner/subagent/SDK integration | configuration/ABI checks, repeated turns, runner protocol tests |
| A6 | Safe platform behavior | wasm clean-error/build, verified Claude tools-only SSH; generic agents rejected before session creation |
| A7 | Honest protocol differences | matrix below, ADR 0164, backend docs |
| A8 | Installed Claude account compatibility, Sonnet only | focused live MCP read/write/shell; actual model evidence |
| A9 | Combined-state quality/resource safety | make test, make quality, make leaks, mutation, stripped size |
| A10 | Managed verified ACP jobs, admission, DAGs, read-only teams and swarms | Child-side authority checks, frozen retry scopes, launch/result/order/permission/cancel/cleanup-hold tests |

A listed feature or isolated compile is not passing evidence. Record exact
commands, exit status, input revision/diff identity and gaps. Reproduce unrelated
baseline failures without modifying main or weakening tests. Live account tests
are authorized only for small ACP validation using Sonnet; never expose secrets.

## Capability matrix

| Capability | ACP client behavior |
| --- | --- |
| Text/thinking/tool/plan events | Normalized when agent reports them |
| Named Claude/pi or custom executable | Explicit argv, no shell parsing |
| Model selection | Advertised config option with confirmation; legacy advertised model acknowledged |
| Native tool schemas | Exact description/parameter conversion to MCP tools/list |
| Native files/shell/search/jobs/teams/subagents | Shared dispatch through tny MCP; tool's own configuration/platform restrictions still apply |
| Imported MCP tools | Same native discovery/describe/call tools and imported configuration |
| SDK custom tools | Same registry; sync/async completion on owning event loop |
| Permissions/intercepts/hooks | Shared for tny MCP calls; external built-ins only ACP permission surface |
| Skills/project/task/system context | Shared context helpers and runtime prompt injection |
| Images | Capability-gated ACP prompt images; captured native image bytes in MCP tool result |
| Native generated preview continuation | Unavailable: external agent owns continuation |
| External fs/terminal client callbacks | Not advertised; unsupported-method response |
| External agent built-in tools | Agent-owned; not governed by tny tool schema/path/intercept guarantees |
| Resume across process restart | Agent must advertise loadSession; no silent fresh-session fallback |
| Same-process repeated turns | Same external session; guarded managed turns reconnect with advertised load |
| Detached runner/client reattach | Existing runner transport; external turn stays in runner |
| Mid-turn checkpoint/restart, steer, compaction | Native-loop features unavailable on ACP |
| Managed jobs/DAGs/admission/read-only teams/swarms | Persisted tools-authority requirement; every owned child verifies pinned Claude before session setup. Commands/selectors and retry scopes are frozen. Native platform/workspace/nesting ceilings remain |
| Per-item ACP providers | Separate canonical ACP profile branch; native HTTP validation unchanged; parent HTTP credentials excluded |
| Cleanup-sensitive claims | Require owned descendant cleanup proof and per-attempt receipt; uncertainty retains capacity/workspace holds |
| Explicit max-steps | Verified Claude maps to SDK maxTurns (conversation turns); generic unsupported adapters reject before session creation |
| MCP-only cancellation during synchronous tools | Owner cancellation remains cooperative; an agent-only MCP cancellation notification is processed after the synchronous tool returns |
| Native automatic recovery-policy learning | Not reproduced for the external agent loop |
| Fast tier/reasoning effort | Fast unsupported; thought_level selected and confirmed when advertised |
| Provider wire request/response hooks | Unavailable: no visibility into agent's model connection |
| Usage and cost | Only fields the adapter actually reports; no inferred accounting |
| SSH | Verified Claude ACP 0.75.1 tools-only mode with private local cwd; generic/unverified adapters rejected before session creation |
| wasm | Clean unsupported diagnostic, no spawn |
| HTTP/SSE MCP bridge | Not used; mandatory stdio avoids optional capability assumptions |

## Provenance

- Historical initial ACP: `8f1274e`; selective recovery from `63af719^` (deletion
  commit `63af719`). No wholesale revert, ACP server, or vendor backend recovery.
- [ACP v1 session setup](https://agentclientprotocol.com/protocol/v1/session-setup):
  absolute cwd, mandatory stdio MCP, capability-gated load and optional transports.
- [Claude ACP adapter](https://github.com/agentclientprotocol/claude-agent-acp),
  inspected upstream `src/acp-agent.ts` for client MCP and model selection.
- [MCP tools](https://modelcontextprotocol.io/specification/2025-06-18/server/tools):
  tools/list metadata and tools/call content/results. Bridge negotiates the
  compatible `2024-11-05` stdio version.

## Security and resource contract

Private 0700 temporary directory, 0600 socket, no bearer secret on CLI/log,
same-user local-agent trust boundary. No filesystem or shell callback is
advertised without implementation. Sessions validated on ACP callbacks. MCP
dispatch requires an active owning turn. Four connections, one outstanding call
per connection, 8 MiB queues/frame cap, bounded per-tick work, pending-call
watchdog, asynchronous invalidation and owned-child cleanup on cancellation.

Adapter compatibility sources: [regadas/pi-acp limitations](https://github.com/regadas/pi-acp#limitations), [geohar package](https://pi.dev/packages/@geohar/pi-acp). These are adapter-specific; pi live execution is unverified.

Managed launch snapshots freeze command argv, profile selector, model/effort and
inherited permission/workspace authority. They do not freeze executable bytes or
external account login. Adapter identity/version is a compatibility check, not
cryptographic attestation. Admission counts owned launch attempts; it does not
promise account-wide token, request or monetary quotas. CLI ACP `steps` is null
because actual external model-call counts are unknown.
