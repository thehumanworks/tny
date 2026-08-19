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

`sdk_message.type` values to render: `system` (subtype `init` has `run_id`), `assistant`, `user`, `tool_call`, `thinking`, `status`, `task`, `usage`. Failure text often lives on `status.message`, not `result.error_code`.

Enable deltas with `SendOptions.enable_deltas` when the TUI wants token streaming.

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

- `CreateAgent` → `{"options":{"model":{"id":M},"apiKey":K,"local":{"cwd":[cwd],"dirs":[…extraDirs]}}}`
  — `model` is a `ModelSelection`, `local.cwd` carries at most one entry
  (when `--model` is absent tny calls `ListModels` and uses the first item id)
- `ListModels` → `{"options":{"apiKey":K}}` (`CursorRequestOptions`; catalog
  RPCs hard-require the per-call key), response models are in `items`
- `ResumeAgent` → same options plus `"agentId"` (the stored host pointer)
- `Send` (server-streaming) → `{"agentId":A,"message":{"text":T},"options":{"enableDeltas":true}}`
  — the field is `message` (a `UserMessage`), not `prompt`
- `CancelRun` → `{"agentId":A,"runId":R}`
- `Shutdown` → `{"graceSeconds":5}`

Permission mode is **not** wired into `CreateAgent`: the bridge is headless
with no per-call approval RPC, so tny emits one status line when
`perm_mode != yolo` pointing at `--backend acp` for per-call approvals.
