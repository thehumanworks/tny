# Backends

Pick a backend per process with `--backend` or `settings.json`. Switching mid-session is v2; v1 starts a new session.

## Ownership

| Backend | Transport | Auth | Tools | Sessions |
| --- | --- | --- | --- | --- |
| `cursor` | Spawn `cursor-sdk-bridge` v1.0.30, Connect HTTP/1.1 `sdk.v1` | `CURSOR_API_KEY` on env **and** RPC options; bridge/callback bearers stay loopback-private | Cursor runtime for built-ins; tny only for registered custom callbacks | Bridge SQLite/JSONL, tny custom store, or cloud IDs/runs |
| `acp` | Spawn agent, JSON-RPC 2.0 JSONL stdio | Agent's `auth/login` or pre-auth | Agent | `session/new` / `resume` |
| `openai` | HTTPS `POST /v1/responses` SSE (`/v1/chat/completions` via `wire_api:"chat"`, [ADR 0016](../adr/0016-responses-api-default-wire.md)) | Bearer or custom header | **tny** | `~/.tny/sessions` |
| `codex` (builtin openai profile) | HTTPS `POST /responses` SSE to `chatgpt.com/backend-api/codex` ([codex.md](codex.md), [ADR 0065](../adr/0065-codex-chatgpt-responses-backend.md)) | ChatGPT OAuth bearer from `$CODEX_HOME/auth.json` + `chatgpt-account-id` | **tny** | `~/.tny/sessions` |

## Decision rule

- User wants Cursor's local/cloud SDK agent → `cursor` (bridge), not `agent acp`.
- User has a ChatGPT subscription (`codex login`) → the builtin `codex` profile on the native loop; Codex's own harness lives in the Codex CLI.
- User wants Gemini / Claude Code / OpenCode / Copilot / … agent harnesses → `acp`.
- User has a Claude subscription (Claude Code OAuth token) or a grok CLI
  session → the builtin `claude` / `grok` profiles on the openai backend ([ADR 0019](../adr/0019-subscription-logins-claude-grok.md),
  [openai-compatible.md](openai-compatible.md#builtin-subscription-profiles-claude-and-grok)).
- User has an OpenAI-compatible base URL → `openai`.

Cursor also speaks ACP (`agent acp`). Support that only as a generic ACP agent, not as the Cursor backend. The product requirement is the **SDK bridge**.

Cursor's complete public bridge surface is available through conversational
CLI/TUI/libtny runtimes and `tny cursor` management. The latter covers catalog,
agent/run lifecycle, messages, artifacts/download, usage, and a safe raw
27-route escape hatch. It does not expose the two reverse callback RPCs as
outbound calls.

## wasm behavior ([ADR 0017](../adr/0017-wasm-browser-parity.md))

Every backend states what it does in the wasm build; a new backend must add
its row here and its behavior is enforced by the wasm CI suites.

| Backend | wasm | How |
| --- | --- | --- |
| `openai` | ✓ works | both wires over `fetch()` |
| `codex` | ✓ works | plain HTTPS like `openai`; the wasm CI job runs `test_codex_chatgpt.py` against the same mock |
| `acp` | ✓ remote-only | `--agent ws://…` (below); no spawn |
| `cursor` | ✗ clean error | conversations report `cursor: conversational sdk.v1 bridge is unavailable in WebAssembly`; management reports `cursor: sdk.v1 management is unavailable in WebAssembly`; callback listeners are native-only |

## Shared client contract

Every backend implements:

```text
connect() / disconnect()
create_or_resume(session)
send(prompt, attachments) -> stream of normalized events
cancel()
respond_permission(id, decision)
```

`doctor` must be able to run each `connect()` in isolation and print a one-line diagnosis (binary missing, auth missing, handshake timeout).
