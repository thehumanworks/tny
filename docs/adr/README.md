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
