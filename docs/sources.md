# Sources

Fetched 2026-08-18. Pin implementations to these pages and to a **release tag**, not `main`.

## fx (parity target)

- https://github.com/vercel-labs/fx
- https://fx.sh
- https://github.com/vercel-labs/fx/releases/tag/v0.0.3
- https://fx.sh/docs
- https://fx.sh/llms.txt
- https://fx.sh/docs/using-fx/cli.md
- https://fx.sh/docs/using-fx/slash-commands.md
- https://fx.sh/docs/using-fx/fx-ask.md
- https://fx.sh/docs/using-fx/sessions.md
- https://fx.sh/docs/using-fx/acp.md
- https://fx.sh/docs/configure-fx/permissions.md
- https://fx.sh/docs/configure-fx/configuration.md
- https://fx.sh/docs/configure-fx/models.md
- https://fx.sh/docs/configure-fx/project-instructions.md
- https://fx.sh/docs/capabilities/tools.md
- https://fx.sh/docs/capabilities/mcp.md
- https://fx.sh/docs/capabilities/mcp/protocol.md
- https://fx.sh/docs/capabilities/skills.md
- https://fx.sh/docs/capabilities/subagents.md
- https://fx.sh/docs/getting-started/authentication.md

## Cursor SDK Bridge

- https://cursor.com/docs/sdk/bridge
- https://github.com/cursor/sdk-bridge
- https://github.com/cursor/sdk-bridge/blob/main/docs/protocol.md
- https://github.com/cursor/sdk-bridge/blob/main/docs/services.md
- https://github.com/cursor/sdk-bridge/blob/main/docs/streaming.md
- https://github.com/cursor/sdk-bridge/blob/main/docs/errors.md
- https://github.com/cursor/sdk-bridge/blob/main/docs/smoke-test.md
- https://github.com/cursor/sdk-bridge/releases/tag/v1.0.28
- https://cursor.com/docs/sdk/typescript
- https://connectrpc.com/docs/protocol
- https://cursor.com/docs/cli/acp (ACP path; not the required Cursor backend)

## Codex app-server

- https://developers.openai.com/codex/app-server
- https://github.com/openai/codex/tree/main/codex-rs/app-server
- https://github.com/openai/codex/blob/main/codex-rs/app-server/README.md
- https://github.com/openai/codex/blob/main/codex-rs/app-server-client/src/remote.rs
- https://github.com/openai/codex/releases/tag/rust-v0.148.0
- https://openai.com/index/unlocking-the-codex-harness/
- `codex app-server generate-json-schema` (version-accurate methods)

## ACP

- https://agentclientprotocol.com/get-started/introduction
- https://agentclientprotocol.com/llms.txt
- https://agentclientprotocol.com/protocol/v1/overview
- https://agentclientprotocol.com/protocol/v1/transports
- https://agentclientprotocol.com/protocol/v2/overview
- https://github.com/agentclientprotocol/agent-client-protocol/releases/tag/schema-v1.20.0
- https://cdn.agentclientprotocol.com/registry/v1/latest/registry.json
- https://agentclientprotocol.com/get-started/agents
- https://github.com/agentclientprotocol/agent-client-protocol

## OpenAI-compatible

- https://developers.openai.com/api/docs/guides/streaming-responses
- https://developers.openai.com/api/docs/guides/migrate-to-responses
- Responses API (default wire, docs/adr/0014): `POST /v1/responses` typed SSE
- Chat Completions (`wire_api:"chat"`): `POST /v1/chat/completions` SSE

## C libraries (intended)

- https://github.com/ibireme/yyjson
- https://github.com/h2o/picohttpparser
- https://github.com/tatsuhiro-t/wslay
- https://github.com/nanopb/nanopb
- https://github.com/silentbicycle/greatest
