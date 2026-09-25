# tny documentation

Research and implementation contract for **tny**: a C11 TUI + CLI agent harness.

The public static site (Geist Mono) is generated from `scripts/site_build.py` into [`site/`](../site/) and published by GitHub Pages. Rebuild with `make site`. The landing terminal is the real CLI compiled to wasm ([ADR 0017](adr/0017-wasm-browser-parity.md)); BYOK intake rules from [ADR 0005](adr/0005-client-side-landing-terminal.md) still apply. User-facing pages live there, including [tnytty](https://thehumanworks.github.io/tny/docs/tnytty.html); this tree remains the harness implementation contract. tnytty's contract is [`tnytty/docs/`](../tnytty/docs/README.md).

Do not start product code until you have read this index and the files it names. tny is a harness for agents, built by agents, focused on the agent ([ADR 0150](adr/0150-agent-first-harness-and-measured-footprint.md)). Use one native OpenAI-compatible HTTP backend, including Codex subscriptions and Grok ([ADR 0152](adr/0152-native-http-only-providers.md)). Stay fast, portable and small through measurement; there is no binary-size ceiling and no competitor-size target.

[Automatic workflow learning](instruction-improvement.md) runs by default during
normal agent work, without selecting a task preset (ADR 0154). The same page
covers optional broader instruction experiments (ADR 0153). Both distinguish
reproducible offline evidence from unmeasured live-model improvement.

## Read first

| Doc | Why |
| --- | --- |
| [product.md](product.md) | Goal, non-goals, success metrics |
| [architecture.md](architecture.md) | Process model, event bus, backend roles |
| [language-and-runtime.md](language-and-runtime.md) | Why C11, library bill of materials |
| [size-and-speed.md](size-and-speed.md) | Measured footprint, speed budgets, historical bake-off |
| [implementation-plan.md](implementation-plan.md) | Ordered phases and acceptance gates |
| [ci.md](ci.md) | GitHub Actions: Linux arches and Darwin arm64; automatic tagged releases after green CI and SDK gates; Nix is developer-only |
| [nix.md](nix.md) | The flake: `nix run`, overlay, dev shell, TLS/version specifics, native CI on x86_64-linux / aarch64-linux / aarch64-darwin |

## User surfaces

| Doc | Why |
| --- | --- |
| [images.md](images.md) | Image generation/editing, provider adapters, CLI and agent tools |
| [speech.md](speech.md) | Ephemeral speech, Codex login, agents and optional MP3 export |
| [dictation.md](dictation.md) | Microphone/file transcription into prompts, independent STT providers |
| [optimisation.md](optimisation.md) | Project-aware prompt rewriting, independent model, draft review |
| [tnyjev.md](tnyjev.md) | Independent typed Jev decisions: `score` and `choose` |
| [cli.md](cli.md) | Command tree, flags, agent-friendly output |
| [workflows.md](workflows.md) | Dependency DAGs and parallel agents from shell, Python, and TypeScript |
| [team-control.md](team-control.md) | Job-backed async teams, captured parent identity, bounded collection and truthful verification state |
| [team-mailbox.md](team-mailbox.md) | Durable addressed collaboration, safe-boundary delivery, acknowledgment and retirement |
| [purposeful-swarms.md](purposeful-swarms.md) | Strict file-defined nested teams, activation, persistence and platform limits |
| [swarm-factory.md](swarm-factory.md) | Version-2 contribution contracts, causal dependencies, isolated writers and bounded evidence |
| [swarm-messages.md](swarm-messages.md) | Typed named-peer messages, approval-bound delivery, retries and acknowledgment |
| [swarm-review.md](swarm-review.md) | Immutable review packets, truthful evidence boundaries and explicit follow-up work |
| [task-workspaces.md](task-workspaces.md) | Isolated editing, provenance, explicit integration and conflict-safe cleanup |
| [admission.md](admission.md) | Shared launch permits, cleanup holds, scope limits and honest usage policies |
| [settings.md](settings.md) | settings.json defaults, schema, env-key HTTP profiles |
| [tui.md](tui.md) | Interactive shell, slash commands, keys |
| [gui.md](gui.md) | Experimental Slint desktop companion, feature boundaries and platform support |
| [worktrees.md](worktrees.md) | Isolated Git checkouts, named reuse, merge/remove/keep on exit |
| [libtny.md](libtny.md) | Experimental headless C embedding ABI |
| [sdks.md](sdks.md) | Python/cffi and TypeScript/Node-API SDK contracts |
| [verification/stream-interruption.md](verification/stream-interruption.md) | Verification contract for interrupted provider streams (ADR 0087): requirement ids and the tests that prove them |
| [adr/0171-formal-verification-smt-lib.md](adr/0171-formal-verification-smt-lib.md) | SMT-LIB/Z3 formal verification boundary and runnable proof gate |
| [adr/0172-release-site-metadata.md](adr/0172-release-site-metadata.md) | Site version from release tag, measured binary size and Pages republishing |
| [sdk-toolkit.md](sdk-toolkit.md) | Standalone SDK image, audio, and prompt optimisation APIs |
| [extensions.md](extensions.md) | Trusted Python event hooks, actions, ordering, provider limits |

## Backends

| Doc | Why |
| --- | --- |
| [backends/README.md](backends/README.md) | Which loop owns tools and auth |
| [backends/codex.md](backends/codex.md) | Codex subscriptions on the ChatGPT Responses backend (native loop) |
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

## Sibling apps

| Doc | Why |
| --- | --- |
| [tnytty/docs](../tnytty/docs/README.md) | tnytty implementation contract (VT core, CLI, HTTP API, platforms) |
| [Public tnytty pages](https://thehumanworks.github.io/tny/docs/tnytty.html) | User-facing tnytty docs on GitHub Pages |

- [Optional ACP client agents](backends/acp.md) and [verification matrix](verification/acp-client/README.md)
