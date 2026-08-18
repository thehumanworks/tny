# Backends

Pick a backend per process with `--backend` or `settings.json`. Switching mid-session is v2; v1 starts a new session.

## Ownership

| Backend | Transport | Auth | Tools | Sessions |
| --- | --- | --- | --- | --- |
| `cursor` | Spawn/attach `cursor-sdk-bridge`, Connect HTTP/1.1 `sdk.v1` | `CURSOR_API_KEY` on env **and** RPC options; bridge bearer from ready-line | Cursor runtime | Bridge local store / cloud IDs |
| `codex` | WebSocket (`ws://`, `wss://`, `unix://`) to `codex app-server` | Codex CLI login + optional WS bearer | Codex core | Codex threads |
| `acp` | Spawn agent, JSON-RPC 2.0 JSONL stdio | Agent's `auth/login` or pre-auth | Agent | `session/new` / `resume` |
| `openai` | HTTPS `POST /v1/chat/completions` SSE | Bearer or custom header | **tny** | `~/.tny/sessions` |

## Decision rule

- User wants Cursor's local/cloud SDK agent → `cursor` (bridge), not `agent acp`.
- User wants Codex's full harness (approvals, threads, steer) → `codex` WebSocket, not Codex-via-ACP.
- User wants Gemini / Claude Code / OpenCode / Copilot / … → `acp`.
- User has an OpenAI-compatible base URL → `openai`.

Cursor also speaks ACP (`agent acp`). Support that only as a generic ACP agent, not as the Cursor backend. The product requirement is the **SDK bridge**.

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
