# Architecture

[Automatic workflow learning](instruction-improvement.md) runs by default inside
ordinary native turns. Typed tool outcomes update bounded workspace evidence;
eligible recovery advice enters later requests without extra inference or tasks
(ADR 0154). The broader instruction-evolution controller remains an optional
external workflow (ADR 0153). Neither changes permission policy or task snapshots.

tny is a native agent harness with one OpenAI-compatible HTTP provider backend
([ADR 0152](adr/0152-native-http-only-providers.md)). CLI, TUI and libtny share
the runtime, session store, permissions, tools, MCP and event loop. Profiles
select credentials, URL, model and Responses or Chat Completions wire format.
Codex and Grok subscription authentication use native login and refresh.

```text
CLI / TUI / C ABI / Python / Node SDKs
                |
       runtime + sessions + tool authority
          |                        |
 native HTTP + SSE             run_code RPC
          |                        |
 gateways / providers     fresh execution server
                               Lua + tools + MCP
```

The event vocabulary remains `text_delta`, `thinking`, `tool_start`, `tool_end`,
`permission_request`, `plan`, `usage`, `turn_end`, `error`, `status`, and
`steer_rejected`. Public event layouts and reserved capability constants remain
ABI-compatible; the reserved ACP capability is available again for optional client sessions.

Extensions run at quiescent native boundaries: pre-tool, unresolved permission,
post-tool, batch and allowlisted provider request/response edges. Callbacks do
not re-enter the backend. Extension-free turns do not start Python.

Optional ACP clients ([ADR 0164](adr/0164-optional-acp-clients.md)) run an external
agent over stdio. Only the verified Claude adapter is admitted, with external
built-ins disabled, strict MCP configuration and a private scratch cwd. Its MCP
bridge exposes `run_code`; nested tools execute under the owning context in the
fresh execution server. [ADR 0174](adr/0174-execution-server-code-mode.md)
supersedes the older adapter admission and direct registry exposure.

## Execution server

Agent tools use the singleton `run_code` surface on both HTTP wires and ACP.
Each cell starts a fresh native execution process with a trusted context
snapshot and bounded Lua runtime. Nested operations keep existing permissions
and workspace policy; direct provider tool names fail closed. See
[ADR 0174](adr/0174-execution-server-code-mode.md) and the
[acceptance contract](verification/execution-code-mode/contract.md).
Wasm returns an unsupported-execution error because it cannot spawn this server.

## Embedding boundary

Standalone SDK toolkit jobs ([ADR 0086](adr/0086-standalone-sdk-toolkit.md))
call the shared image, speech and dictation services directly. Public SDK
optimisation currently returns `TNY_STATUS_UNSUPPORTED` before any I/O: it
requires a trusted execution-server launcher that the embedding ABI does not
provide (ADR 0174). Native CLI/TUI optimisation remains supported.
Each has a private context and atomic cancellation flag; language adapters own
scheduling and release. They never enter the agent session API or spawn `tny`.

[`libtny`](adr/0023-libtny-embedding-abi.md) exposes opaque
runtime/session/event/error handles through a pull-driven C ABI. It does
not expose `tny_ctx`, `tny_backend`, `tny_backend_event`, yyjson, or `pollfd`
layouts.
The public `next_event` operation and the CLI adapters drive the same private
runtime engine. TUI prewarm remains an acceleration adapter over that engine,
not a separate provider lifecycle.

## Process rules

- One tny process, one primary workspace (`cwd` unless `--cwd`).
- **Turns run in a detached session runner** ([ADR 0053](adr/0053-forked-turn-isolation.md), [ADR 0166](adr/0166-global-sessions-and-immediate-backgrounding.md)): on native builds, `ask` and the TUI spawn a fresh executable in a detached process session. The runner owns the backend, engine and every `session.json` write, streaming normalized events over `<session>/sock` (NDJSON). Execution children own nested tools and their MCP clients; state changes return to the runner for persistence. A caller crash or SIGKILL detaches the turn. Explicit interrupts, foreground TUI exit and foreground terminal hangup stop it, with a verified process kill if cancellation stalls ([ADR 0081](adr/0081-reliable-session-interruption.md)). Caller-side macOS TLS initialization does not disable isolation. wasm, `--ephemeral`, and `TNY_ISOLATE=0` keep the turn in-process; supported native tools still require execution children. Library-hosted model tools fail closed.
- Always have a RAII-style shutdown path: cancel turn → close stream → release resources.
- Never log bearer tokens, `.env` values.

Left-arrow backgrounding marks the existing runner as background and opens the
shared agents dashboard as soon as the runner acknowledges persistence. The
same HTTP or ACP turn continues through streaming and tools without a restart
or replay; the writer and listener remain continuously held. Resolved secrets
and configuration for initial runner startup travel only through anonymous
IPC. The dashboard includes all saved sessions across all stored workspaces,
with workspace labels and owner-checked reattachment. See
[ADR 0166](adr/0166-global-sessions-and-immediate-backgrounding.md).

Private runner and durable-job C++ aggregates own descriptors, advisory-lock
lifetimes and native process scopes ([ADR 0118](adr/0118-runner-and-job-resource-ownership.md)).
Cancellation, observed reaping, log drainage, terminal persistence and restart
activation remain explicit operations. Destructors only release storage resources;
unknown scope cleanup retains authority and its persisted reservation hold.
OS-specific spawn and pre-exec operations remain in the C host seams.

## Config and state

| Path | Contents |
| --- | --- |
| `~/.tny/settings.json` | Provider/model/effort/fast defaults, permission mode, named HTTP provider profiles, UI, per-workspace overrides, optional `mcp.import_from` ([schema](../schemas/settings.schema.json)) |
| `~/.tny/mcp.json` | Authoritative stdio and Streamable HTTP MCP servers (never repo-local MCP). Foreign user/project configs load only when global `mcp.import_from` explicitly names their harness ([ADR 0052](adr/0052-mcp-import-from-harnesses.md)) |
| `~/.tny/sessions/` | Transcripts and recovery checkpoints |
| `~/.tny/skills/` | Managed skill installs |
| `~/.tny/extensions/` | Trusted global Python event hooks (`*.py`, `*/index.py`) |
| `~/.tny/tasks/` | User task-preset Markdown definitions (`NAME.md`) |
| `~/.tny/worktrees/` | Managed Git checkouts; origin metadata and usage locks live in their private Git directories ([ADR 0080](adr/0080-managed-git-worktrees.md)) |
| `<repo>/.tny.json` | Repo-safe limits only (steps, tool result bytes, sandbox, context on/off) |
| `<repo>/.tny/tasks/` | Project task-preset Markdown definitions; project files cannot add authority or cost |
| `<repo>/AGENTS.md` | Project instructions (also `CLAUDE.md` as alias if present). Over `--ssh`, the remote cwd's file is used instead of this local path ([ADR 0040](adr/0040-ssh-agents-md.md)) |

BYOK credentials come from environment variables (`OPENAI_API_KEY` or profile-specific names). Native Codex/Grok OAuth tokens use their login stores. MCP retains `header_env` / `bearer_token_env`. No API keys are persisted in settings or project JSON.

## Shared internals

```text
src/
  cli/ tui/ lib/     # frontends and public C ABI
  core/             # runtime, sessions, tools, permissions, login, jobs/teams
  backends/openai/  # Responses + Chat Completions, streaming and tool loop
  net/              # HTTP/1.1, SSE, TLS seam
  mcp/              # stdio and Streamable HTTP MCP clients
```

Blocking waits use `tny_poll`; wasm's HTTP seam uses fetch and Asyncify with
queued bytes, never JS re-entry into C. MCP shares this HTTP seam (remote-only
on wasm). The native session runner starts lazily at the first turn. Context
changes rebind an idle runner or defer rebind until the active turn settles.
There is no provider-host prewarm thread. Bounded independent file work keeps
the joined worker policy of ADR 0132 (at most eight workers).

## Jev decision service

`core/tnyjev` is an isolated typed C11 client for TypeSafe Jev, with no runtime,
settings, environment, session or chat-provider dependency. `score` and `choose`
are thin CLI toolkit adapters; the former maps to Noul P(yes), the latter to
Choice. Configuration and cancellation are explicit; results have no owned
pointers. Shared HTTP/JSON and `tny_poll` retain native/wasm parity. This is not
a chat backend or a change to existing harness decisions. Future reasoning,
routing and memory/reaction integrations remain out of scope. See
[tnyjev](tnyjev.md) and [ADR 0165](adr/0165-tnyjev-decision-engine.md).

## Speech service

The CLI and native `speak` tool share `core/speech.c`, a provider table whose
adapters produce bounded MP3 bytes. ChatGPT speech reuses Codex credentials
regardless of the conversation backend. `util/audio.c` owns external player
startup and anonymous audio lifetime within the existing host OS seam.
See [ADR 0070](adr/0070-provider-independent-speech.md) and
[ADR 0071](adr/0071-ephemeral-host-audio-playback.md).

## Dictation service

The CLI and composer share `core/dictation.c`, which owns capture, bounded
WAV/transcript validation, cancellation, and result lifetime. Adapters behind
`core/dictation_provider.h` own credentials and endpoints for Codex/ChatGPT
and xAI, independently of the chat backend. `dictation_http.c` shares their
bounded WAV multipart upload and incremental JSON response handling.
`util/audio_capture.c` starts an optional local recorder only on demand.
The existing event loop polls capture/response fds. TUI code inserts completed
text into the draft and waits for the user's ordinary submit action.
See [Dictation](dictation.md) and [ADR 0079](adr/0079-provider-independent-dictation.md).

## Prompt optimisation service

`core/optimise.c` owns a separate explicit context, ephemeral session and native
engine for rewriting drafts. It enforces file-reading/search tools and disables
extensions, MCP, task presets and skill injection. `tui/tui_optimise.c` replaces
the composer only after a successful result, without submitting it. Model and
credentials resolve independently of the conversation. CLI and TUI share the
same polling/cancellation lifecycle on native and wasm builds.
See [Prompt optimisation](optimisation.md) and
[ADR 0082](adr/0082-prompt-optimisation.md).

## Image service

The standalone image CLI and native image tools share `core/image_service.c`.
A private provider table receives loaded reference bytes and returns image
bytes; adapters own credentials and wire formats. The common service bounds
inputs/results and atomically persists one artifact. The initial Codex adapter
uses ChatGPT credentials independently of the chat provider. See
[ADR 0074](adr/0074-extensible-image-service.md) and
[ADR 0075](adr/0075-image-cli-and-agent-tools.md).

## Provider-independent web search

`core/search_codex.c` performs a bounded search-only Responses request using
independent Codex credentials/model; it has no conversation or local tool loop.
The shared web_search tool and CLI use this service when logged into Codex and
DuckDuckGo only without that login (explicit command/URL overrides still win).
Codex uses the same nested search operation; provider-hosted search items are
not advertised and unsolicited hosted execution is rejected. Search results are
ordinary parent tool results for checkpoint/reattach purposes. See
[ADR 0109](adr/0109-provider-independent-codex-search.md).

## Private ownership implementation

[ADR 0114](adr/0114-private-cpp20-ownership-boundaries.md) permits private
C++20 modules for stream decoding, retained events/async tools, and
runner/job resources. [ADR 0126](adr/0126-checkpoint-context-ownership.md) extends
this boundary to checkpoint encoding and independently owned context recovery.
[ADR 0133](adr/0133-owned-subagent-launch-snapshots.md) adds independent
sub-agent launch snapshots: selectors and environment share one owned, wiped
block; failed construction preserves the previous plan. Process control stays
in C and no new scheduling or OS seam is introduced.
The C-facing adapters retain scheduling, public
ABI and OS operations in their existing owners. Synchronous views are
borrowed; retained records own their data. Exceptions never escape to C.

## Private runtime ownership (ADR 0116)

Runtime queue records own immutable payload bytes in C++ behind the private C
facade in `core/owned_event.h`; C retains event ordering, budgets and
scheduling. Popped events outlive their engine/session. Async custom tools use
separate provider and host handles sharing only call/registry lifetime; registry
invalidation, generation, epoch and completion checks remain explicit under one
scoped mutex. See [ADR 0116](adr/0116-runtime-event-and-async-ownership.md).

## Allocation-free provider OOM settlement (ADR 0117)

When an allocation fails during an active turn, the runtime publishes its
terminal guard, enters a thread-local settlement scope and invokes the
provider's cancel. Providers release transports, processes and parser storage
without constructing RPCs, tool results or transcript JSON; the runtime then
delivers its preallocated OOM ERROR/TURN_END pair. Ordinary cancellation is
unchanged. See [ADR 0117](adr/0117-allocation-free-provider-oom-settlement.md).

## Native request and turn ownership (ADR 0127)

The native C provider loop borrows private C++ aggregates for request builder
scratch/JSON/serialized storage, HTTP connections, retained turn buffers and
permission/custom records. Provider views are released after their last builder
use; body/header/path borrows survive stale connection replay. Pending admission
copies borrowed metadata before moving the parsed call, so allocation failure
preserves the source. Async invalidation, cancellation deferral, continuation,
consumed tool indices and persistence remain explicit C transitions. Destructors
only release resources. See [ADR 0127](adr/0127-native-provider-request-owners.md)
and the [ownership inventory](verification/issue-144/ownership.md).

[Collective swarm mode](collective-swarm.md) composes a stable facilitation policy
with existing session checkpoints, shared launch admission and durable run mailboxes.
Directory notifications in `util/jobs_host` drive bounded native mailbox waits
(ADR 0156); no second agent loop, scheduler or broker owns task state.
[Purposeful definitions](purposeful-swarms.md) are strictly validated and flattened
into one team DAG. Nested coordinators are ordinary durable participants with group
and upward-synthesis metadata; the canonical definition and provenance remain part
of the session/checkpoint identity. A persisted activation nonce joins an interrupted
session to exactly one parent-owned job, while the jobs boundary accepts topology only
through a transient compiler-owned manifest and validates the ordered durable
projection before adoption (ADR 0157).
