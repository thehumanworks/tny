# 0044 — Cursor stays on the `sdk.v1` bridge; the private `agent.v1` route is rejected

Date: 2026-08-28
Status: accepted

## Context

[thehumanworks/cursor-direct-sdk](https://github.com/thehumanworks/cursor-direct-sdk)
reverse-engineered the Cursor Agent CLI (build `2026.08.26-143298b`) and
reimplemented its private wire in TypeScript: API-key exchange on
`api2.cursor.sh`, `GetServerConfig` for a dynamic agent endpoint, then one
HTTP/2 Connect **bidirectional** stream `POST /agent.v1.AgentService/Run`
carrying `application/connect+proto` envelopes (gzip-flagged) with the
`exec` (shell/read/write/grep/ls/mcp), KV blob, control, heartbeat and
interaction-update sub-protocols. The question was whether tny should drop
`cursor-sdk-bridge` and speak that route directly from C11 for faster turns.

## Measurements (2026-08-28, `composer-2.5`, prompt "Reply with exactly OK",
macOS arm64, same network, same minute)

| Path | Cold (process start → answer) | Warm turn → first token |
| --- | --- | --- |
| tny → `cursor-sdk-bridge` (`tny --provider cursor ask`) | 4.9–6.8 s | — |
| bridge, warm process: `CreateAgent` 0.94 s, then `Send` #1 / #2 / #3 | — | 2.83 s / **1.40 s** / **1.27 s** |
| cursor-direct-sdk `client.prompt()` (turn 1 / 2 / 3, same process) | 3.6 s | 3.5 s / 2.4 s / 2.8 s |
| cursor-direct-sdk phases, everything cached | auth 304 ms + config/models 431 ms | `Run` → first delta **2.6 / 2.2 / 2.5 s** (`openRaw` 1–16 ms) |

The bridge's warm-agent turn (1.3–1.4 s) is faster than the direct route's
best case (2.2–2.6 s) even with tokens, server config, model catalog and
the HTTP/2 session all cached. The direct client starts a fresh
conversation per run and serves an empty KV/blob store, so the server
rebuilds context on every turn; the bridge keeps conversation state and its
blob cache. Matching it would mean reimplementing that state, which is the
expensive part of the private protocol (the SDK's own `RECON.md` reaches
the same conclusion and recommends `sdk.v1`).

Cold-start cost of the bridge (spawn 0.2–0.3 s + `CreateAgent` ~0.9 s +
first-`Send` penalty ~1.5 s) is already hidden from the TUI by the pre-warm
thread ([0002](0002-tui-provider-prewarm.md),
[0004](0004-time-to-first-token.md)); one-shot `ask` pays it once.

## What the direct route would cost tny

- An HTTP/2 client (HPACK, flow control, bidi streams) plus ALPN through the
  dlopen'd SecureTransport/OpenSSL shim. `src/net/` is HTTP/1.1 + WebSocket
  only; the `RunSSE`/`BidiAppend` HTTP/1 fallback exists in the CLI bundle
  but is unverified and still needs everything below.
- A protobuf codec and a vendored, **CLI-build-pinned** private schema
  (1,034 `agent.v1` descriptors; the SDK vendors 323 of them and re-extracts
  them from the installed CLI on every Cursor update).
- The executor protocol: tny would run shell/read/write/grep/ls/edit and
  the KV blob cache itself, turning Cursor from a *host* backend into a
  second native loop with Cursor-private semantics and no documented
  contract, breaking on any CLI release without notice.
- Size: all of the above against the < 2.0 MiB budget, for a slower turn.

## Decision

Keep `cursor-sdk-bridge` (`sdk.v1`, Connect HTTP/1.1 JSON) as the only
Cursor transport. Do not vendor the `agent.v1` schema or add HTTP/2 for it.
Revisit only if (a) a measured warm `Run` → first-token beats the bridge by
a margin that survives the pre-warm, or (b) Cursor publishes `agent.v1` as
a supported contract.

## Consequences

- `docs/sources.md` keeps only the public bridge sources for Cursor.
- Future Cursor TTFT work targets the bridge path: the ~1.5 s first-`Send`
  penalty on a fresh agent is the largest remaining warm cost and is not yet
  hidden by pre-warm.
