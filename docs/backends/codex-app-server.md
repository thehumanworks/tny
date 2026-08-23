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

`--fast` / `/fast` (`TNY_CAP_FAST`) adds `"serviceTier":"priority"` to
`thread/start` params — the paid fast tier (`service_tier = fast|priority`
in codex `config.toml`; tny sends `"priority"`, the value every app-server
release accepts). Unset means the host's own default; the host ignores
unknown values.

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
5. `account/read` then `turn/start` with `threadId` and `input: [{ "type": "text", "text": "…", "text_elements": [] }]`. Optional `effort` overrides the reasoning effort "for this turn and subsequent turns" — tny sends it (mapped per [ADR 0009](../adr/0009-reasoning-effort.md)) whenever `--effort`/`/effort` is set, which is why a mid-conversation change needs no thread restart. Supported values per model come from `model/list` → `supportedReasoningEfforts[].reasoningEffort` (plus `defaultReasoningEffort`); tny surfaces them through `tny models`.
6. Concatenate `item/agentMessage/delta` by `itemId` until `turn/completed` (`completed` \| `interrupted` \| `failed`).
7. `turn/steer` `{threadId, expectedTurnId, input: UserInput[]}` (in-flight only; tny sends it for Enter-during-a-turn once the `turn/start` response has delivered the turn id — [ADR 0011](../adr/0011-mid-turn-input-steer-or-queue.md); a rejection, e.g. a non-steerable `/review` turn, re-queues the text and does not end the turn; a steer the host never answered before `turn/completed` is resolved as rejected at turn end so response/notification ordering on the socket can never lose the text — [ADR 0013](../adr/0013-steer-rejection-owns-the-text.md)), `turn/interrupt` `{threadId, turnId}`.

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

When no login exists yet, `tny --provider codex login` performs the sign-in
**through the app-server itself** ([ADR 0017](../adr/0017-subscription-logins-claude-grok.md)):

1. connect (attach or spawn) and `initialize` as usual;
2. `account/login/start` with `{"type":"chatgpt"}` (browser flow — the
   result carries `authUrl`, and the app-server hosts the localhost OAuth
   callback, so the host must stay up) or `{"type":"chatgptDeviceCode"}`
   with `--device` (result carries `verificationUrl` + `userCode`);
3. print the URL / code (best-effort browser open for the browser flow) and
   pump the socket until the `account/login/completed` notification
   (`{loginId, success, error}`); `account/login/cancel` on Ctrl-C/timeout.

The host writes `$CODEX_HOME/auth.json` and owns refresh; tny never parses
or prints the tokens — success is observable as `tny_codex_auth_present()`.
Hosts too old for the v2 account endpoints answer -32601 and tny falls back
to running `codex login`. The flow is covered by
`tests/integration/test_codex.sh` run 7 against the scripted mock.

## Client metadata

Set a stable `clientInfo.name` of `tny` so Codex usage attribution stays readable.

Reference implementation of the remote transport: `codex-rs/app-server-client/src/remote.rs` in [openai/codex](https://github.com/openai/codex).
