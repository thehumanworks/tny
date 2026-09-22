# Sources

## TypeSafe Jev decision engine (tnyjev)

Primary API documentation consulted 2026-09-22; model default `jev-latest`
(versioned IDs can be selected explicitly). The no/yes CLI `score` maps to
Noul, not the separate ordinal Score primitive. See [tnyjev](tnyjev.md).

- https://docs.typesafe.ai/api — `POST https://api.typesafe.ai/v1/systemone`,
  typed Noul/Choice requests and answers, Bearer auth, usage, HTTP errors.
- https://docs.typesafe.ai/introduction/quickstart — `TYPESAFE_API_KEY` and examples.
- https://docs.typesafe.ai/primitives/noul — `[0,1]` no/yes probability.
- https://docs.typesafe.ai/primitives/choice — option descriptions and distribution.
- https://docs.typesafe.ai/models — aliases and versioned model IDs.

## Bounded instruction evolution (2026-09-19)

Primary-source snapshots and applicability are recorded in
[ADR 0153](adr/0153-bounded-instruction-evolution.md):

- SoL-Pi: https://github.com/NVlabs/SoL-Pi/tree/bd005888b9b8a3fcdb511feb91fc27d3dfa8f2b1
  and https://arxiv.org/html/2609.20519v1
- GEPA: https://arxiv.org/html/2507.19457v1
- Darwin Gödel Machine: https://arxiv.org/html/2505.22954v1
- ACE: https://arxiv.org/html/2510.04618v1

These inform bounded experiments and evidence retention. Their published gains
are not claims about tny. The workflow's offline benchmark is a deterministic
replay, not a live-model study.

Fetched or rechecked 2026-08-31. Pin implementations to these pages and to a
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
- https://fx.sh/docs/configure-fx/permissions.md
- https://fx.sh/docs/configure-fx/configuration.md
- https://fx.sh/docs/configure-fx/models.md
- https://fx.sh/docs/configure-fx/project-instructions.md
- https://fx.sh/docs/capabilities/tools.md
- https://fx.sh/docs/capabilities/mcp.md
- https://fx.sh/docs/capabilities/mcp/protocol.md
- MCP Streamable HTTP (ADR 0051): https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http
- MCP versioning: https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning
- MCP `server/discover`: https://modelcontextprotocol.io/specification/2026-07-28/server/discover
- Legacy Streamable HTTP initialize: https://modelcontextprotocol.io/specification/2025-06-18/basic/transports

## MCP import formats (ADR 0052, captured 2026-08-31)

- Codex `config.toml` `[mcp_servers.*]`: https://learn.chatgpt.com/docs/config-file/config-reference.md (user file `$CODEX_HOME/config.toml`, default `~/.codex/config.toml`). Stable Codex pin remains rust-v0.149.1.
- Claude Code user/local/project `mcpServers`: https://code.claude.com/docs/en/mcp
- Cursor global/project `mcp.json`: https://docs.cursor.com/context/model-context-protocol
- Official xAI Grok Build user/project `[mcp_servers.*]`, pinned commit `bc7f02eddd3d84085849dc19ed216f11c23b0571` (stable channel 1.0.13; public snapshot 1.0.12): https://github.com/xai-org/grok-build/blob/bc7f02eddd3d84085849dc19ed216f11c23b0571/crates/codegen/xai-grok-pager/docs/user-guide/07-mcp-servers.md
- Grok Build home resolution: https://github.com/xai-org/grok-build/blob/bc7f02eddd3d84085849dc19ed216f11c23b0571/crates/codegen/xai-grok-home/src/lib.rs
- Grok Build project discovery/precedence: https://github.com/xai-org/grok-build/blob/bc7f02eddd3d84085849dc19ed216f11c23b0571/crates/codegen/xai-grok-shell/src/util/config/mcp.rs
- https://fx.sh/docs/capabilities/skills.md
- https://fx.sh/docs/capabilities/subagents.md
- https://fx.sh/docs/getting-started/authentication.md


## Codex (ChatGPT Responses backend, docs/adr/0065)

- Responses API reference (`input` is a string **or** an item array; `store`, `stream`, `instructions`): https://developers.openai.com/api/reference/resources/responses/methods/create
- Codex CLI client to `chatgpt.com/backend-api/codex` (headers, body): https://github.com/openai/codex/blob/main/codex-rs/core/src/client.rs
- `auth.json`, JWT account-id claim, refresh grant and client id: https://github.com/openai/codex/blob/main/codex-rs/login/src/token_data.rs and `codex-rs/login/src/auth/manager.rs`
- Refresh contract tests (`/oauth/token`, `CODEX_REFRESH_TOKEN_URL_OVERRIDE`): https://github.com/openai/codex/blob/main/codex-rs/core/tests/suite/auth_refresh.rs
- Browser PKCE login (authorize URL parameters, `localhost:1455` / 1457 callback, token exchange): https://github.com/openai/codex/blob/main/codex-rs/login/src/server.rs
- Device-code login (`/api/accounts/deviceauth/usercode`, `…/token`, `/codex/device`, `/deviceauth/callback`): https://github.com/openai/codex/blob/main/codex-rs/login/src/device_code_auth.rs
- Independent implementation of both flows plus refresh (pi): https://github.com/badlogic/pi-mono/blob/main/packages/ai/src/utils/oauth/openai-codex.ts

## Codex app-server (retired backend; kept for the hook-parity baseline)

- https://developers.openai.com/codex/app-server
- https://github.com/openai/codex/tree/main/codex-rs/app-server
- https://github.com/openai/codex/blob/main/codex-rs/app-server/README.md
- https://github.com/openai/codex/blob/main/codex-rs/app-server-client/src/remote.rs
- Stable release: https://github.com/openai/codex/releases/tag/rust-v0.149.1
- Subscription usage route (`GET /backend-api/wham/usage`): https://github.com/openai/codex/blob/rust-v0.149.1/codex-rs/backend-client/src/client/rate_limit_resets.rs
- Usage window OpenAPI fields (`used_percent`, `limit_window_seconds`, `reset_at`): https://github.com/openai/codex/blob/rust-v0.149.1/codex-rs/codex-backend-openapi-models/src/models/rate_limit_window_snapshot.rs
- Stable tag object: `980a6d12110b110d29ec13bdcbe14011100b3566`
- Stable commit: `ff29a44391deccde0aba0f8390337d7f3c319ea4`
- https://github.com/openai/codex/blob/ff29a44391deccde0aba0f8390337d7f3c319ea4/codex-rs/app-server/README.md
- https://github.com/openai/codex/blob/ff29a44391deccde0aba0f8390337d7f3c319ea4/codex-rs/protocol/src/protocol.rs#L1502-L1514
- Official hooks guide: https://learn.chatgpt.com/docs/hooks
- Alpha observation only (`Interrupt`, not the stable baseline):
  https://github.com/openai/codex/releases/tag/rust-v0.150.0-alpha.9
- https://openai.com/index/unlocking-the-codex-harness/
- `codex app-server generate-json-schema` (version-accurate methods)


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
- https://github.com/nanopb/nanopb
- https://github.com/silentbicycle/greatest

## Image generation (ADR 0074, ADR 0084)

- Codex Images API release pin: **rust-v0.154.0-alpha.3**
- Client routes: https://github.com/openai/codex/blob/rust-v0.154.0-alpha.3/codex-rs/codex-api/src/endpoint/images.rs
- Request/response types: https://github.com/openai/codex/blob/rust-v0.154.0-alpha.3/codex-rs/codex-api/src/images.rs
- Original built-in image defaults: https://github.com/openai/codex/blob/rust-v0.154.0-alpha.3/codex-rs/ext/image-generation/src/tool.rs
- GPT Image 2.5 model and quality settings (verified 2026-09-09): https://developers.openai.com/api/docs/guides/image-prompting
- Subscription usage: https://learn.chatgpt.com/docs/image-generation

## Dictation (ADR 0079)

- Account transcription contract, release **rust-v0.105.0**: https://github.com/openai/codex/blob/rust-v0.105.0/codex-rs/tui/src/voice.rs#L751-L833
- FFmpeg input devices (AVFoundation audio selection and PulseAudio): https://ffmpeg.org/ffmpeg-devices.html
- ALSA recording CLI: https://github.com/alsa-project/alsa-utils/blob/v1.2.14/aplay/aplay.1
- The account route is live-tested separately; it is not the API-key-only `/v1/audio/transcriptions` route.

### xAI REST STT (verified 2026-09-09)

- Primary guide: https://docs.x.ai/developers/model-capabilities/audio/speech-to-text
- REST reference: https://docs.x.ai/developers/rest-api-reference/inference/speech-to-text
- Model overview: https://docs.x.ai/developers/models/speech-to-text
- Protocol pin: `POST https://api.x.ai/v1/stt`, Bearer xAI API key,
  `multipart/form-data` with `file` last (tny sends only `file`, `audio.wav`,
  `audio/wav`); JSON object with string `text`. Container WAV is auto-detected;
  omit raw-audio fields. REST has no `model` field or published versioned model
  selector; use the service default, not the realtime `grok-transcribe` option.
- Profile/chat base URLs are ignored by STT. Grok token fallback uses the
  existing auth reader/refresh and the same Bearer header, only at the official
  API endpoint. The STT docs do not guarantee Grok subscription entitlement.
- Local fixture builds replace only the xAI adapter URL; installed/release
  binaries contain no STT endpoint override. No live credentials enter tests.

## Native Grok HTTP

- Public Responses/OpenAI compatibility: https://docs.x.ai/developers/rest-api-reference/inference/responses
- Runtime scope: [ADR 0151](adr/0152-native-http-only-providers.md). Historical protocol sources remain in their original ADRs and verification evidence.
