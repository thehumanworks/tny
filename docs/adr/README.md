# Architecture decision records

Numbered, append-only. A new decision that changes an old one gets a new ADR
that names what it supersedes; the old file stays. Code and docs reference
these as `docs/adr/NNNN`.

| ADR | Decision |
| --- | --- |
| [0001](0001-run-all-agents-in-yolo-mode.md) | All agents run in yolo mode by default |
| [0002](0002-tui-provider-prewarm.md) | The TUI pre-warms the provider's host at startup |
| [0003](0003-transient-menu-overlay.md) | In-TUI menus are transient overlays, never transcript |
| [0004](0004-time-to-first-token.md) | Resume on the warm-up thread; ask overlaps connect with stdin; codex one-shots attach to a live host |
| [0005](0005-client-side-landing-terminal.md) | GitHub Pages terminal is a client-side BYOK preview; keys encrypted, base URL obfuscated |
| [0006](0006-ci-build-targets.md) | CI builds Linux x86_64+aarch64 (glibc + musl static), Darwin arm64 only, Windows via MSYS2 |
| [0007](0007-linux-tls-system-openssl.md) | Linux TLS dlopen's the system OpenSSL (libssl.so.3) at first use; nothing linked or vendored |
| [0008](0008-native-loop-images.md) | Native-loop images are user-role `image_url` parts; `read_image` replaces the vision stub |
| [0009](0009-reasoning-effort.md) | Canonical reasoning-effort levels (`off…max`), mapped per provider, catalog-discovered where possible |
| [0010](0010-fast-tier-capability.md) | `--fast` is a `TNY_CAP_FAST` capability mapped per provider; cursor composes it with effort in one `params` array |
| [0011](0011-mid-turn-input-steer-or-queue.md) | Enter during a turn steers (codex `turn/steer`, native loop) or queues (cursor, acp); queue sends at turn end, Esc drops it |
| [0012](0012-self-contained-sgr-lines.md) | Streamed transcript color is per-line: SGR never crosses a newline |
| [0013](0013-steer-rejection-owns-the-text.md) | The backend owns a steered text: `STEER_REJECTED` carries it, unanswered steers resolve as rejected before `TURN_END`, stale responses never fail a later turn |
| [0014](0014-build-time-version-from-git.md) | `TNY_VERSION` comes from `git describe` at build time (generated header); releases pass the tag; nothing hardcoded |
| [0015](0015-settings-default-effort.md) | settings.json carries a user-authored default effort (global or per-provider); flag//effort > env > settings; tny still never writes it |
| [0016](0016-responses-api-default-wire.md) | The native backend defaults to the Responses API wire; Chat Completions is the `wire_api:"chat"` opt-in |
| [0017](0017-wasm-browser-parity.md) | The landing terminal is the real CLI compiled to wasm; parity enforced by shared sources, the tny_poll/net seams, and the same mock suites in CI |
| [0018](0018-provider-setup-stored-keys.md) | `provider setup` writes guided profiles (CLI flags/tty prompts, TUI wizard, browser hash pairs); a stored `api_key` is the fallback below env vars, settings.json drops to 0600 |
| [0019](0019-subscription-logins-claude-grok.md) | `tny login` drives the codex app-server `account/login/start`; claude and grok are builtin openai-compatible profiles fed by subscription credentials (Claude Code OAuth token, grok session token), never persisted by tny |
| [0020](0020-ephemeral-sessions.md) | `--ephemeral` keeps conversation state process-local, blocks resume/import, and applies provider no-store controls where defined |
| [0021](0021-native-grok-device-login.md) | `tny --provider grok login` runs the RFC 8628 device flow natively against auth.x.ai (no grok CLI), writes/refreshes the session in the grok CLI's own `~/.grok/auth.json` format, and logout removes only the xAI entries |
| [0022](0022-ssh-execution-boundary.md) | `--ssh user@host[:port]` / `/ssh` keep tny local and run every native-loop workspace tool (files, grep, terminal) on the remote host over one OpenSSH ControlMaster; no tny needed remotely; host backends are refused |
| [0023](0023-libtny-embedding-abi.md) | libtny is one headless runtime with an experimental pull-driven C ABI; CLI/TUI/ACP are adapters; ABI 0 ships shared libraries on macOS arm64 and Linux glibc |
| [0024](0024-unlimited-steps-default.md) | The native agent loop is unlimited by default; `--max-steps` / `/max-steps` / `.tny.json` `"steps"` set an explicit cap (0 = unlimited everywhere, libtny included) |
| [0025](0025-clipboard-images-paste-as-paths.md) | Ctrl-V materializes clipboard images and inserts the path as provider-neutral prompt text; `/image` remains the explicit attachment path |
| [0026](0026-color-vs-attribute-sgr.md) | `NO_COLOR` gates SGR colors only — bold/dim/reverse are structural; `--color=always`/`CLICOLOR_FORCE` force, `--color=never`/`--no-color` silence all SGR; dumb mode announces itself |
| [0027](0027-python-event-hooks.md) | Trusted global Python extensions consume versioned normalized events through one optional persistent host; `agent_end` may continue with visible context, `agent_settled` is final |
| [0028](0028-extension-parity-contract.md) | Extension parity is capability-scoped; one versioned vocabulary, immutable provider matrix, deterministic folding, and explicit unsupported states govern all lanes |
| [0029](0029-named-acp-agent-profiles.md) | Reusable ACP commands resolve as `acp:NAME`; requested models are applied through advertised session config options before the first prompt |
