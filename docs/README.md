# tny documentation

Research and implementation contract for **tny**: a C11 TUI + CLI agent harness.

The public static site (Geist Mono, same shape as [fx.sh](https://fx.sh)) is generated from `scripts/site_build.py` into [`site/`](../site/) and published by GitHub Pages. Rebuild with `make site`. The landing terminal is a client-side BYOK preview ([ADR 0005](adr/0005-client-side-landing-terminal.md)), not WASM tny. User-facing pages live there; this tree remains the implementation contract.

Do not start product code until you have read this index and the files it names. The goal is to beat [fx](https://github.com/vercel-labs/fx) on size and speed, keep its user-facing functionality, and add Cursor SDK Bridge, Codex app-server (WebSocket), ACP, and OpenAI-compatible providers.

## Read first

| Doc | Why |
| --- | --- |
| [product.md](product.md) | Goal, non-goals, success metrics |
| [architecture.md](architecture.md) | Process model, event bus, backend roles |
| [language-and-runtime.md](language-and-runtime.md) | Why C11, library bill of materials |
| [size-and-speed.md](size-and-speed.md) | fx baseline and tny budgets |
| [implementation-plan.md](implementation-plan.md) | Ordered phases and acceptance gates |
| [ci.md](ci.md) | GitHub Actions matrix: Linux arches, Darwin arm64, Windows |
| [nix.md](nix.md) | The flake: `nix run`, overlay, dev shell, TLS/version specifics |

## User surfaces

| Doc | Why |
| --- | --- |
| [cli.md](cli.md) | Command tree, flags, agent-friendly output |
| [settings.md](settings.md) | settings.json defaults, schema, named providers and ACP agents |
| [tui.md](tui.md) | Interactive shell, slash commands, keys |
| [libtny.md](libtny.md) | Experimental headless C embedding ABI |
| [sdks.md](sdks.md) | Python/cffi and TypeScript/Node-API SDK contracts |
| [extensions.md](extensions.md) | Trusted Python event hooks, actions, ordering, provider limits |

## Backends

| Doc | Why |
| --- | --- |
| [backends/README.md](backends/README.md) | Which loop owns tools and auth |
| [backends/cursor-bridge.md](backends/cursor-bridge.md) | Spawn `cursor-sdk-bridge`, Connect `sdk.v1` |
| [backends/codex-app-server.md](backends/codex-app-server.md) | WebSocket JSON-RPC to Codex |
| [backends/acp.md](backends/acp.md) | ACP client (other agents) and ACP server (native loop) |
| [backends/openai-compatible.md](backends/openai-compatible.md) | Chat Completions (+ optional Responses) |

## Feature parity with fx

| Doc | Why |
| --- | --- |
| [features/parity-with-fx.md](features/parity-with-fx.md) | Must-keep inventory vs deferrals |
| [features/extension-hook-parity.md](features/extension-hook-parity.md) | Release-pinned Pi/Codex/Claude/fx hook classifications and provider capabilities |
| [features/sessions.md](features/sessions.md) | Save, resume, compact, recover |
| [features/permissions.md](features/permissions.md) | ask / auto / yolo, rules, sandbox |
| [features/mcp-and-skills.md](features/mcp-and-skills.md) | MCP client, skills, subagents, tools |

## Sources

Primary URLs and version pins: [sources.md](sources.md).
