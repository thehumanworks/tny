# Cursor SDK Bridge

Canonical docs: [cursor.com/docs/sdk/bridge](https://cursor.com/docs/sdk/bridge), repo [cursor/sdk-bridge](https://github.com/cursor/sdk-bridge).

The bridge is a **local Connect server** that embeds `@cursor/sdk` and exposes `sdk.v1` over **HTTP/1.1**. Pin standalone + protos to **[v1.0.28](https://github.com/cursor/sdk-bridge/releases/tag/v1.0.28)** (or newer matching `manifest.json`). Host binary is ~23–43 MiB (Bun) — never link it. Classic gRPC/HTTP2 will not connect.

JSON Connect uses **camelCase** (`apiKey`). Confirm with the [curl smoke test](https://github.com/cursor/sdk-bridge/blob/main/docs/smoke-test.md) before codegen.

This path is **headless**: tools run unless sandbox / `auto_review` / hooks deny them. There is no Allow/Deny RPC. Per-call approvals are **ACP** (`agent acp`), a separate backend.

## Obtain the binary

Do not bundle it into `tny`. Resolve in order:

1. `CURSOR_SDK_BRIDGE_BIN`
2. `cursor-sdk-bridge` on `PATH` (also shipped inside `cursor-sdk` Python wheels)
3. Optional later: download `cursor-sdk-bridge-standalone-<os>-<arch>.tar.gz` from the [release](https://github.com/cursor/sdk-bridge/releases) whose tag matches the vendored `proto/sdk/v1`

Archives unpack flat: `bin/cursor-sdk-bridge`, `proto/sdk/v1/`, `manifest.json`. Assert `manifest.protocol == "sdk.v1"`. Pin protos to that tag; never edit `proto/`.

`manifest.json` example fields: `bridgeVersion`, `sdkVersion`, `runtime` (e.g. `bun-1.3.9`). That runtime weight is why tny must not statically link the bridge.

## Spawn and ready line

```text
CURSOR_API_KEY=… CURSOR_SDK_CLIENT_LANGUAGE=c \
  cursor-sdk-bridge --workspace <cwd>
```

Capture **stderr**. Scan for the exact prefix `cursor-sdk-bridge ready ` (trailing space). Parse JSON after it.

Required fields: `schemaVersion == 1`, `transport == "tcp"`, `protocol == "connect"`, `url` or `host`+`port`, `authTokenFile`. Ignore unknown fields. Startup timeout 30s. If the process exits first, return captured stderr.

Read `authTokenFile` (mode 0600), trim, send on every RPC:

```text
Authorization: Bearer <token>
```

Older bridges may inline `authToken` in the ready JSON — prefer it if present, **never log the ready line**.

Keep draining stderr for the life of the process.

Useful flags: `--host` (default `127.0.0.1`), `--port 0`, `--workspace`, `--state-root`, `--local-store`, `--verbose` / `CURSOR_SDK_BRIDGE_LOG=1`.

## Auth (two secrets)

1. **Cursor API key** — set `CURSOR_API_KEY` in the bridge env **and** `options.api_key` on `CreateAgent` / `ResumeAgent` and every `SdkCursorService` call. Catalog RPCs hard-require the per-call key.
2. **Bridge bearer** — per-process token from the ready line. Loopback only by default.

Team Admin API keys are not supported. User keys and service-account keys are.

## Wire format

```text
POST http://127.0.0.1:<port>/sdk.v1.<Service>/<Method>
Connect-Protocol-Version: 1
Authorization: Bearer <token>
```

| Kind | Content-Type | Body |
| --- | --- | --- |
| Unary | `application/json` or `application/proto` | Bare JSON/protobuf message |
| Server stream (`Send`, `ObserveRun`) | `application/connect+json` or `application/connect+proto` | Enveloped frames |

Streaming frame (Connect):

```text
flags:1byte | length:4byte big-endian | payload
```

`flags & 0x02` is end-of-stream. That frame's payload is a JSON `EndStreamResponse` (error details), not a `RunStreamMessage`. Empty envelopes are keepalives (~15s idle) — ignore them.

v1 may speak **JSON** to avoid shipping a perfect protobuf story on day one; pin field names to the release protos. Prefer nanopb `application/proto` before calling the backend production-ready.

## Services tny must call

From [docs/services.md](https://github.com/cursor/sdk-bridge/blob/main/docs/services.md):

| Service | RPCs |
| --- | --- |
| `SdkBridgeControlService` | `Ping`, `GetVersion`, `Shutdown` (`grace_seconds`), optional `SetToolCallback` |
| `SdkCursorService` | `Me`, `ListModels` (need model id for local agents), `ListRepositories` |
| `SdkAgentService` | `CreateAgent`, `ResumeAgent`, `Send`, `ObserveRun`, `WaitLiveRun`, `CancelRun`, `GetRun`, `CloseAgent` |

Local agents: `options.local.cwd = [workspace]`, **explicit** `options.model`. Cloud: `options.cloud.repos`. Omitting both defaults to local — always set one.

## Send stream

`RunStreamMessage` oneof: `sdk_message`, `result`, `done`, optional `interaction_update` / `step`. Track last non-empty `offset` only from `ObserveRun` for resume. Dropped `Send` does **not** cancel the run — reconnect with `ObserveRun` (from start if you only have live offsets) or `WaitLiveRun`.

`sdk_message` is **an envelope**: a `type` string plus the @cursor/sdk stream
event as a `google.protobuf.Struct` in `message` (`sdk_messages.proto`). All
interesting fields — the tool-call union, status text, `run_id` — live inside
that payload, not on the envelope; `src/backends/cursor/map.c` unwraps it once
before mapping. `type` values to render: `system` (subtype `init` has
`run_id`), `assistant`, `user`, `tool_call`, `thinking`, `status`, `task`,
`usage`. Failure text often lives on the status payload's `message`, not
`result.error_code`. The terminal `result` carries a `RunResult` whose
`result` field is the final assistant text — used as fallback when no
assistant event streamed.

Enable deltas with `SendOptions.enable_deltas` when the TUI wants token streaming.

## Tool call payloads

The `tool_call` payload does **not** carry flat `name`/`args` fields. It nests
a per-tool union keyed by a `*ToolCall` variant, matching the
[cursor-agent stream-json format](https://cursor.com/docs/cli/reference/output-format):

```json
{"type":"tool_call","subtype":"started","call_id":"…",
 "tool_call":{"readToolCall":{"args":{"path":"file.txt"}}}}

{"type":"tool_call","subtype":"completed","call_id":"…",
 "tool_call":{"readToolCall":{"args":{"path":"file.txt"},
   "result":{"success":{"content":"…","totalLines":54}}}}}
```

Known variants include `readToolCall`, `writeToolCall`, `editToolCall`,
`deleteToolCall`, `shellToolCall` / `bashToolCall`, `grepToolCall`,
`lsToolCall`, `globToolCall`, `todoToolCall`, `mcpToolCall`. tny maps them by
stripping the `ToolCall` suffix (`read`, `shell`, …), prefers the variant's
inner `name` when present (MCP tools), and clips `args` / the unwrapped
`result.success` or `result.error` into `tool_detail`. Unknown future variants
still render under their derived key name — an opaque `tool` line is a mapping
bug, and `tests/test_cursor.c` plus the mock-bridge integration test guard it.
`result.error` (or a `failed`/`error` subtype) marks the call failed.

## Remote use, ssh, and known incompatibilities

The bridge is a **headless local process on loopback TCP**: tny running in a
plain terminal on a remote box you ssh'd into works fine — it needs the
`cursor-sdk-bridge` binary and `CURSOR_API_KEY` on *that* machine, and no
Cursor IDE or GUI anywhere. Cursor exposes no API to drive a locally running
Cursor IDE; the bridge (which embeds `@cursor/sdk`) is the only supported
local programmatic surface, so tny uses it and inherits its limits.

Because the bridge owns the whole tool loop, some tny features cannot cross
into it. tny calls each one out at use time rather than failing silently:

- **`tny --ssh` (remote tool runtime, [ADR 0022](../adr/0022-ssh-execution-boundary.md))**
  is native-loop only: `--provider cursor --ssh …` is refused at startup with
  a pointer to an openai-compatible provider. The bridge's tools always run on
  the machine where the bridge runs.
- **Per-call approvals**: headless, no Allow/Deny RPC. `--perm ask|auto`
  emits one status line pointing at `--backend acp` ([ADR 0001](../adr/0001-run-all-agents-in-yolo-mode.md)).
- **Image input**: not wired yet; `--image` is refused with a pointer to
  `--provider openai`.
- **wasm**: clean error — the browser build cannot spawn the bridge
  ([backends/README.md](README.md)).
- **tny-side tools/MCP/skills**: the native loop's tool surface does not
  apply; the agent uses Cursor's own tools, sandbox, hooks and MCP config.

## Shutdown

`Shutdown` or SIGTERM → wait ~5s → kill. Also on tny exit so bridges cannot leak.

## Callbacks (later)

Custom tools / custom stores invert the connection (tny hosts a loopback Connect server). Not required to send a first turn. If implemented, decode **chunked** request bodies; store outputs must be the bare record.

## Smoke

Follow [smoke-test.md](https://github.com/cursor/sdk-bridge/blob/main/docs/smoke-test.md) with curl before debugging tny. First tny milestone: spawn → Ping → Me → CreateAgent (local) → Send → wait → Shutdown.

## Implemented JSON shapes (v1 JSON codec — verify against release protos)

tny's v1 uses the Connect **JSON** codec (nanopb deferred). Field names follow
protojson camelCase. Written by `src/backends/cursor/cursor.c` only; readers
accept camelCase *and* snake_case. Re-pin these against the release `.proto`s
and smoke-test.md before shipping against a new bridge release.

- `CreateAgent` → `{"options":{"model":{"id":M,"params":[{"id":P,"value":V}]},"apiKey":K,"local":{"cwd":[cwd],"dirs":[…extraDirs]}}}`
  — `model` is a `ModelSelection`, `local.cwd` carries at most one entry
  (when `--model` is absent tny calls `ListModels` and uses the first item id).
  `params` appears only when `--effort` and/or `--fast` are set:
  `--fast` / `/fast` (`TNY_CAP_FAST`) adds `{"id":"fast","value":"true"}`
  (`"false"` for `/fast default`) — fast is a per-model parameter, and
  omitting it leaves the model's own default variant (which may itself be
  the fast one); `--effort` adds `{"id":"effort","value":V}`
- `ListModels` → `{"options":{"apiKey":K}}` (`CursorRequestOptions`; catalog
  RPCs hard-require the per-call key), response models are in `items`
- `ResumeAgent` → same options plus `"agentId"` (the stored host pointer)
- `Send` (server-streaming) → `{"agentId":A,"message":{"text":T},"options":{"enableDeltas":true,"model":…}}`
  — the field is `message` (a `UserMessage`), not `prompt`; `options.model`
  (same `ModelSelection`) is sent only when `--effort` is set, so `/effort`
  applies mid-conversation without a new agent
- `CancelRun` → `{"agentId":A,"runId":R}`
- `Shutdown` → `{"graceSeconds":5}`

Reasoning effort: model params are **model-specific** — a top-level key or an
unknown param id is silently dropped by the bridge. tny therefore resolves
`--effort` against the model's `ListModels` entry (`SdkModel.parameters`, a
`ModelParameterDefinition` whose id names an effort, values in
`values[].value`) and sends only a catalog-allowed value
([ADR 0009](../adr/0009-reasoning-effort.md)). No match → one status line and
the model default runs; catalog unreachable → param id `effort` is sent
unverified with a status saying so.

Permission mode is **not** wired into `CreateAgent`: the bridge is headless
with no per-call approval RPC, so tny emits one status line when
`perm_mode != yolo` pointing at `--backend acp` for per-call approvals.
