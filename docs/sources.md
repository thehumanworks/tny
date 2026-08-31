# Sources

Fetched or rechecked 2026-08-25. Pin implementations to these pages and to a
**release tag**, not `main`. Moving-main observations are labeled and are not
release claims.

## Pi extensions (behavior reference)

- Release: https://github.com/earendil-works/pi/releases/tag/v0.84.3
- Pin: `v0.84.3`, commit `4e58f324fae8ebfa98a3d45181fb248072a2afac`
- https://github.com/earendil-works/pi/blob/4e58f324fae8ebfa98a3d45181fb248072a2afac/packages/coding-agent/docs/extensions.md
- https://github.com/earendil-works/pi/blob/4e58f324fae8ebfa98a3d45181fb248072a2afac/packages/coding-agent/src/core/extensions/types.ts

## Claude Code hooks (behavior reference)

- npm release: `@anthropic-ai/claude-code@2.1.245`
- npm shasum: `cceab6b3a7a4d899e2a94963852304aaba43d6ac`
- npm integrity: `sha512-+7baJddJXZukgd6AgC7xStHGsMTVHDPlRcAoqTSPx2NQ+QwKGtvCZQLgbnKuhjkwq9v9vKvYwIhLOwGiE77mVQ==`
- https://registry.npmjs.org/@anthropic-ai/claude-code/2.1.245
- Official hook reference captured 2026-08-25:
  https://code.claude.com/docs/en/hooks

## fx (parity target)

- https://github.com/vercel-labs/fx
- https://fx.sh
- Hook baseline: https://github.com/vercel-labs/fx/releases/tag/v0.0.5
- Hook pin: `v0.0.5`, commit `df7e6245e1992758d4060c97477ceafa27770551`
- https://github.com/vercel-labs/fx/blob/df7e6245e1992758d4060c97477ceafa27770551/src/core/hooks/definitions.zig
- Non-normative main observation captured 2026-08-25:
  `16eda256ca3c94a50744a5fb57d033ec18011f24`
- Historical size/performance baseline only:
  https://github.com/vercel-labs/fx/releases/tag/v0.0.3
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
- MCP Streamable HTTP (ADR 0050): https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http
- MCP versioning: https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning
- MCP `server/discover`: https://modelcontextprotocol.io/specification/2026-07-28/server/discover
- Legacy Streamable HTTP initialize: https://modelcontextprotocol.io/specification/2025-06-18/basic/transports
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
- Pin: tag object `3c91b3a6cdf9a5fdeb2917816275fb2aedbf9cda`, commit `260a73d33f906abe9f4adfde486bbdeb133344b7`
- https://github.com/cursor/sdk-bridge/blob/v1.0.28/proto/sdk/v1/sdk_messages.proto
- https://github.com/cursor/sdk-bridge/blob/v1.0.28/docs/streaming.md
- https://cursor.com/docs/sdk/typescript
- https://cursor.com/docs/sdk/python
- https://connectrpc.com/docs/protocol
- https://cursor.com/docs/cli/acp (ACP path; not the required Cursor backend)

## Codex app-server

- https://developers.openai.com/codex/app-server
- https://github.com/openai/codex/tree/main/codex-rs/app-server
- https://github.com/openai/codex/blob/main/codex-rs/app-server/README.md
- https://github.com/openai/codex/blob/main/codex-rs/app-server-client/src/remote.rs
- Stable release: https://github.com/openai/codex/releases/tag/rust-v0.149.1
- Stable tag object: `980a6d12110b110d29ec13bdcbe14011100b3566`
- Stable commit: `ff29a44391deccde0aba0f8390337d7f3c319ea4`
- https://github.com/openai/codex/blob/ff29a44391deccde0aba0f8390337d7f3c319ea4/codex-rs/app-server/README.md
- https://github.com/openai/codex/blob/ff29a44391deccde0aba0f8390337d7f3c319ea4/codex-rs/protocol/src/protocol.rs#L1502-L1514
- Official hooks guide: https://learn.chatgpt.com/docs/hooks
- Alpha observation only (`Interrupt`, not the stable baseline):
  https://github.com/openai/codex/releases/tag/rust-v0.150.0-alpha.9
- https://openai.com/index/unlocking-the-codex-harness/
- `codex app-server generate-json-schema` (version-accurate methods)

## ACP

- https://agentclientprotocol.com/get-started/introduction
- https://agentclientprotocol.com/llms.txt
- https://agentclientprotocol.com/protocol/v1/overview
- https://agentclientprotocol.com/protocol/v1/transports
- https://agentclientprotocol.com/protocol/v2/overview
- https://github.com/agentclientprotocol/agent-client-protocol/releases/tag/schema-v1.20.0
- Pin: tag object `4908af80fe0285fc765cddec8aeb54627a81e9ec`, commit `5e89c71497fe07dd4ae633c181a17224f4a8956d`
- https://raw.githubusercontent.com/agentclientprotocol/agent-client-protocol/schema-v1.20.0/schema/v1/schema.json
- https://agentclientprotocol.com/protocol/v1/prompt-turn
- https://agentclientprotocol.com/protocol/v1/tool-calls
- https://cdn.agentclientprotocol.com/registry/v1/latest/registry.json
- https://agentclientprotocol.com/get-started/agents
- https://github.com/agentclientprotocol/agent-client-protocol

## OpenAI-compatible

- https://developers.openai.com/api/docs/guides/streaming-responses
- https://developers.openai.com/api/docs/guides/migrate-to-responses
- Responses API (default wire, docs/adr/0016): `POST /v1/responses` typed SSE
- Chat Completions (`wire_api:"chat"`): `POST /v1/chat/completions` SSE
- https://openrouter.ai/docs/api/reference/streaming
- https://openrouter.ai/docs/cookbook/administration/usage-accounting
- https://openrouter.ai/docs/guides/best-practices/reasoning-tokens

## C libraries (intended)

- https://github.com/ibireme/yyjson
- https://github.com/h2o/picohttpparser
- https://github.com/tatsuhiro-t/wslay
- https://github.com/nanopb/nanopb
- https://github.com/silentbicycle/greatest
