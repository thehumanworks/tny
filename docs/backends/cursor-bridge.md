# Cursor SDK Bridge

tny implements the complete public Cursor SDK Bridge **v1.0.30** contract over
Connect HTTP/1.1 JSON. The release is pinned at tag `v1.0.30`, tag object
`026d21b23641ee488a6650ba850327b8a66ab1cd`, commit
`8157597c625b5f642d3c4a1472d20c9c330a9d18`; the embedded SDK manifest names
source commit `a401fe7f346d4d3ba66fd596cc842b0ad5e5259c`.

The vendored contract contains 5 services, 29 RPCs, 114 messages, and 285
fields. Twenty-seven RPCs run from tny to the bridge. Two reverse services,
`SdkCustomToolCallbackService.CallCustomTool` and
`SdkStoreCallbackService.CallStore`, run from the bridge to tny. The external
bridge remains a 23–43 MiB Bun host and is never linked into the tny binary.
Classic gRPC/HTTP2 will not connect.

[ADR 0050](../adr/0050-complete-cursor-sdk-v1.md) records the complete public
implementation. [ADR 0044](../adr/0044-cursor-stays-on-sdk-bridge.md) still
rejects Cursor's private `agent.v1` HTTP/2 protocol.

## Install, spawn, and authentication

Resolve the bridge in this order:

1. `--bridge-bin PATH`
2. `CURSOR_SDK_BRIDGE_BIN`
3. `cursor-sdk-bridge` on `PATH`, including the copy in Cursor SDK wheels

The binary and vendored schema must come from the same v1.0.30 release.
Archives unpack `bin/cursor-sdk-bridge`, `proto/sdk/v1/`, and `manifest.json`;
`manifest.protocol` must be `sdk.v1`.

tny launches the host with `--workspace`, loopback `--host 127.0.0.1`, and an
ephemeral port. Configured `state_root`, `local_store`, and custom-store
callback URL/token become the corresponding bridge launch flags. The bridge
prints a `cursor-sdk-bridge ready ` JSON line. tny validates schema version 1,
TCP, Connect, URL/host/port, and the token file before sending any request.

There are two separate secrets:

- `CURSOR_API_KEY` goes into the child environment and every sdk.v1 request
  field that defines `apiKey`. User and service-account keys are supported;
  Team Admin API keys are not.
- The bridge bearer comes from the mode-0600 ready-line token file and is sent
  as `Authorization: Bearer …` on every RPC.

tny never prints the ready line, either bearer, callback tokens, API keys, MCP
auth, configured environment secrets, or raw requests containing them.

## Wire and negotiated contract

```text
POST http://127.0.0.1:<port>/sdk.v1.<Service>/<Method>
Connect-Protocol-Version: 1
Authorization: Bearer <bridge bearer>
```

Unary requests use `application/json` and a bare protojson object. The three
server streams—`Send`, `ObserveRun`, and `DownloadArtifact`—use
`application/connect+json` and five-byte envelopes:

```text
flags:1 | payload length:4 big-endian | payload
```

Empty data frames are keepalives. `flags & 0x02` is an `EndStreamResponse`,
not an application event. tny decodes its optional protobuf
`SdkErrorDetails`, retaining the stable error code, request ID, provider,
help URL, retry duration, and rate-limit data while tolerating additive fields.

Every connection runs `Ping` and `GetVersion`, requires
`protocolVersion == "sdk.v1"`, and retains the advertised capability set.
Calls fail locally when the route's required capability is absent. The
v1.0.30 bridge advertises ten capability groups; unknown future groups remain
available for diagnostics but do not invent behavior.

## Services and RPCs

| Service | Direction | RPCs |
| --- | --- | --- |
| `SdkBridgeControlService` | tny → bridge | `Ping`, `Shutdown`, `GetVersion`, `SetToolCallback` |
| `SdkCursorService` | tny → bridge | `Me`, `ListModels`, `ListRepositories` |
| `SdkAgentService` | tny → bridge | `CreateAgent`, `ResumeAgent`, `ReloadAgent`, `CloseAgent`, `Send`, `WaitLiveRun`, `GetRun`, `ListRuns`, `GetRunConversation`, `ObserveRun`, `CancelRun`, `GetAgent`, `ListAgents`, `ArchiveAgent`, `UnarchiveAgent`, `DeleteAgent`, `ListAgentMessages`, `ListArtifacts`, `DownloadArtifact`, `GetUsage` |
| `SdkCustomToolCallbackService` | bridge → tny | `CallCustomTool` |
| `SdkStoreCallbackService` | bridge → tny | `CallStore` |

The CLI exposes readable management aliases and a route-table-checked raw
surface:

```sh
tny cursor models
tny cursor agents
tny cursor runs AGENT_ID
tny cursor observe RUN_ID AFTER_OFFSET
tny cursor artifacts AGENT_ID
tny cursor download AGENT_ID PATH > artifact
tny cursor rpc SdkAgentService GetRun '{"runId":"…","options":{…}}'
printf '%s' "$REQUEST" | tny cursor rpc SdkAgentService CreateAgent -
```

Raw `rpc` accepts only the 27 outbound routes, one bounded JSON object from an
argument or stdin, and preserves unary JSON or one JSON frame per stream line.
The `download` alias decodes bytes incrementally with an 8 MiB cumulative
limit. `delete` and raw `DeleteAgent` require explicit `--yes` before the
bridge starts.

## Agent and send options

`settings.cursor.agent_options` and `send_options` are validated protojson
objects. tny preserves supported fields and then overlays its owned values:
the runtime API key, explicit/discovered model, workspace roots, stream deltas,
and registered custom-tool definitions.

Supported agent configuration includes:

- local or cloud runtime; local `cwd` plus multi-root `dirs`, or cloud
  environments, repositories, branch/PR behavior, environment variables, and
  metadata;
- model parameter values, Agent or Plan mode, sandbox options, automatic
  review, setting sources, and per-send force/cloud environment overrides;
- MCP stdio or HTTP/SSE servers, including headers/auth, and named subagent
  definitions with inherited or explicit models;
- presence-sensitive built-in tool allowlists (`tools.names`) and
  `disallowedTools`, where deny wins;
- image data in `UserMessage.images`. `--image` uses the shared magic-byte
  validation and 8 MiB input limit, then sends base64 `SdkImageData` with the
  detected MIME type.

Model effort and fast mode are model parameters, not global request fields.
tny resolves effort IDs/values from `ListModels`; `--fast` adds the `fast`
parameter without replacing effort or configured parameters.

## Embedding and language SDKs

libtny ABI 1 accepts `provider: "cursor"` through the same pull-driven runtime
used for OpenAI-compatible providers. Cursor runtimes require explicit
`state_dir`, `api_key`, and `model`; they never ambiently load user settings.
`CURSOR_SDK_BRIDGE_BIN` selects the external host. Session creation/resume,
normalized streaming events, cross-thread cancellation, and registered
sync/async custom tools use the normal C API. The ABI 1 conversation surface
does not expose image items; image input remains a `tny ask`/TUI CLI feature.
Python and TypeScript expose the same provider selection and scheduling
semantics.

The public capability snapshot reports Cursor as available and reports it as
selected/initialized when appropriate. It does not claim that a bridge or
credential is reachable until the runtime actually connects. Catalog,
agent/run administration, artifact download, and raw RPC remain CLI-only under
`tny cursor`; no provider-specific structs enter the stable ABI.

## Custom tools and stores

For local agents, registered libtny tools are copied into
`local.customTools` as descriptions and JSON Schemas. tny starts one
authenticated loopback callback server, registers it with `SetToolCallback`,
validates each `CallCustomTool` name and arguments through the existing tool
registry, and returns a structured result. Cloud agents reject custom host
tools because the bridge cannot call a client-local handler from the cloud.

The callback does not turn Cursor into the native OpenAI loop. Cursor still
chooses and owns the agent/tool loop. tny owns only explicitly registered
custom callbacks. Sensitive callbacks require yolo mode; ask/auto fail closed
because `CallCustomTool` has no interactive approval round trip. During a
blocking bridge unary such as Create/Resume, a bounded callback pump serves
store requests and rejects tool execution rather than running an owner-thread
tool from the pump thread.

Store type `sqlite` is the default; `jsonl` needs `rootDir`; `custom` delegates
`agents`, `runs`, `runEvents`, and `checkpoints` to tny's authenticated
`CallStore` service. Records live under the selected state root in
`cursor-sdk-store/`. Operations are bounded, validate identity/filter fields,
use atomic files, preserve pagination/append ordering, and base64-encode
checkpoint blobs. Blocking unary and normal stream paths both keep callbacks
serviceable, preventing bridge↔client deadlock.

Callback listeners bind loopback only, use independent 256-bit bearer tokens,
accept exact POST paths and JSON content, accept valid chunked request bodies
across arbitrary transport splits, reject malformed framing, oversized bodies,
or bad auth, and are stopped before their state is freed.

## Runs, recovery, and cancellation

The saved host pointer is versioned:

```text
cursor-sdk.v1:{"agent_id":"…","run_id":"…","after_offset":"…","runtime":"local|cloud"}
```

Legacy bare agent IDs remain readable. New pointers prevent another provider
or protocol version from misinterpreting Cursor state. tny uses
`CreateAgent`/`ResumeAgent`, captures the run ID, and records the last exclusive
`ObserveRun` offset. If `Send` drops, it reconnects with `ObserveRun`, filters
events already delivered by `Send`, and requires an authoritative terminal run
result before reporting success. `WaitLiveRun` remains available through the
management/raw-RPC surface; it is not a hidden turn-recovery fallback.
`RunStreamMessage` supports SDK events, terminal result, done, interaction
updates, and conversation steps.

Cancellation sends `CancelRun` once the run ID is known and keeps observing
until an authoritative terminal `CANCELLED` result arrives. It does not treat
closing the local socket as cancellation. Management commands also expose run
snapshots, pagination, conversations, messages, archive lifecycle, artifacts,
and cloud usage/cost.

## Security, ephemeral mode, and platform boundaries

- Cursor's built-in tools, MCP, sandbox, review, and subagents are
  bridge-owned. tny's normal permission rules, skills, native MCP, `--ssh`,
  and `--max-steps` do not govern them. Use Cursor's supported option fields;
  do not assume an ACP-style Allow/Deny RPC exists.
- Registered tny custom tools are the exception described above and retain
  tny's validation/sensitivity boundary.
- Repository `.tny.json` may not add Cursor credentials, endpoints, tools,
  MCP, callbacks, or cost authority. User-level `settings.json` is the trusted
  configuration source; secrets should still come from environment/host auth.
- `--ephemeral` writes no tny session or custom Cursor store. It strips
  persistent `local.store` options. Cursor Cloud or the bridge may retain
  remote data according to their own policy; tny cannot promise remote
  deletion.
- wasm returns a clean unsupported error before bridge/callback work: browsers
  cannot spawn this local host. Conversations report
  `cursor: conversational sdk.v1 bridge is unavailable in WebAssembly`;
  management reports
  `cursor: sdk.v1 management is unavailable in WebAssembly`. A tny process on
  a remote shell works when the bridge and key are installed on that same
  machine.
- Shutdown stops streams and callbacks, asks the bridge to shut down, then
  applies bounded TERM/KILL process-group cleanup.

## Verification boundary

The deterministic suite covers all 27 outbound routes, both reverse callbacks,
local/cloud agents, every stream type, state/recovery/cancel behavior,
fragmentation, auth and structured errors, settings/options, CLI management,
secret non-leakage, teardown, wasm clean failure, and libtny/SDK provider
parity. Contract integrity is regenerated and checked from the pinned release.

Those fixtures do not prove Cursor Cloud currently accepts a credential,
model, repository, or billing request. A live smoke needs a user-provided
`CURSOR_API_KEY`; no live key is present in CI, and no live result is claimed.
