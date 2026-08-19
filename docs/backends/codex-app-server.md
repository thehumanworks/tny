# Codex app-server (WebSocket)

Canonical: [developers.openai.com/codex/app-server](https://developers.openai.com/codex/app-server), source `openai/codex` → `codex-rs/app-server`. Design essay: [Unlocking the Codex harness](https://openai.com/index/unlocking-the-codex-harness/).

tny is a rich client of the same JSON-RPC surface the VS Code extension uses. Prefer **WebSocket** as required. stdio JSONL is the same messages and is useful for tests. Pin the installed `codex` (research snapshot: `rust-v0.148.0`) and generate types from **that** binary. Leave `experimentalApi` **false**.

OpenAI marks TCP WebSocket as **experimental**. Still implement it; default to loopback. Current source **refuses non-loopback without `--ws-auth`**. Do not send an `Origin` header (403). There is **no default TCP port**.

## Start or attach

Spawn:

```bash
codex app-server --listen ws://127.0.0.1:4500
```

Or attach to an already-running server (`tny --backend codex --codex-ws ws://127.0.0.1:4500`).

Also supported by Codex:

| Listen | Notes |
| --- | --- |
| `stdio://` (default) | JSONL, one object per line |
| `ws://IP:PORT` | One JSON-RPC message per **text** frame |
| `unix://` or `unix://PATH` | WebSocket upgrade over `$CODEX_HOME/app-server-control/…` or a path |
| `off` | No listener |

Health on the TCP listener: `GET /readyz`, `GET /healthz` (reject requests with `Origin` using 403).

Auth flags for WS:

```text
--ws-auth capability-token --ws-token-file /abs/path
--ws-auth capability-token --ws-token-sha256 HEX
--ws-auth signed-bearer-token --ws-shared-secret-file /abs/path
```

Client handshake header: `Authorization: Bearer <token>` on the HTTP Upgrade (enforced before `initialize`). Prefer token files over argv. Official client will not put a bearer on non-loopback `ws://` (only `wss://` or loopback). Pass `--session-source cli` if the crate default would tag threads as `vscode`.

Overloaded server: JSON-RPC error `-32001` `"Server overloaded; retry later."` — exponential backoff with jitter.

Generate the exact schema from the installed binary (version-specific):

```bash
codex app-server generate-json-schema --out ./schemas
```

Vendor that snapshot next to the Codex version `doctor` detected. Do not guess method names from memory when they drift.

## Framing

JSON-RPC 2.0 **shape** (request / response / notification / error) but the `"jsonrpc":"2.0"` header is **omitted on the wire**.

Request:

```json
{ "method": "thread/start", "id": 10, "params": { "model": "gpt-5.4" } }
```

Response: `{ "id": 10, "result": { "thread": { "id": "thr_123" } } }`
Error: `{ "id": 10, "error": { "code": 123, "message": "…" } }`
Notification: `{ "method": "turn/started", "params": { "turn": { "id": "turn_456" } } }`

Handle ping/pong and ignore binary frames. Official remote client max frame: **128 MiB**. No reconnect — resume the `thread.id`.

Unix-socket clients still send a dummy HTTP Upgrade URL such as `ws://localhost/rpc` on the UDS.

## Lifecycle

1. Connect WS.
2. `initialize` with `clientInfo` `{ name: "tny", title: "tny", version }`.
3. Notification `initialized`.
4. `thread/start` or `thread/resume` or `thread/fork`.
5. `account/read` then `turn/start` with `threadId` and `input: [{ "type": "text", "text": "…", "text_elements": [] }]`.
6. Concatenate `item/agentMessage/delta` by `itemId` until `turn/completed` (`completed` \| `interrupted` \| `failed`).
7. Optional `turn/steer` (in-flight only), `turn/interrupt` `{threadId, turnId}`.

Wire enums from current Rust (docs examples can be stale): approval `"untrusted"` \| `"on-request"` \| `"never"`; sandbox `"read-only"` \| `"workspace-write"` \| `"danger-full-access"`.

Must answer server requests or the turn hangs: `item/commandExecution/requestApproval`, `item/fileChange/requestApproval`, `item/permissions/requestApproval`, `item/tool/requestUserInput`, `mcpServer/elicitation/request`. Decisions include `accept` / `acceptForSession` / `decline` / `cancel`. Do not implement v1 `execCommandApproval` for a `turn/start` client.

`initialize` is once per connection. Before it: `Not initialized`. Second call: `Already initialized`.

Server result includes `userAgent`, `platformFamily`, `platformOs`.

## Primitives

- **Thread** — durable conversation; start / resume / fork / archive.
- **Turn** — one user request plus agent work.
- **Item** — typed unit with `item/started`, optional `item/*/delta`, `item/completed`.

Notifications to render: `turn/started`, `item/started`, `item/completed`, `item/agentMessage/delta`, tool progress, `thread/archived`, `turn/completed`.

The server can **send requests** (approvals). Pause the turn until tny replies allow/deny. This is bidirectional: tny must multiplex reads and writes on one socket.

## Codex login

Reuse the user's `codex login` / ChatGPT session. tny does not store OpenAI cookies. `doctor` should run `codex login status` when the binary exists.

## Client metadata

Set a stable `clientInfo.name` of `tny` so Codex usage attribution stays readable.

Reference implementation of the remote transport: `codex-rs/app-server-client/src/remote.rs` in [openai/codex](https://github.com/openai/codex).
