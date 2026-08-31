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
| [0022](0022-ssh-execution-boundary.md) | `--ssh user@host[:port]` / `/ssh` keep tny local and run every native-loop workspace tool (files, grep, terminal) on the remote host over one OpenSSH ControlMaster; no tny needed remotely; host backends are refused. Project `AGENTS.md` follows the remote cwd ([0040](0040-ssh-agents-md.md)) |
| [0023](0023-libtny-embedding-abi.md) | libtny is one headless runtime with an experimental pull-driven C ABI; CLI/TUI/ACP are adapters; ABI 0 ships shared libraries on macOS arm64 and Linux glibc |
| [0024](0024-unlimited-steps-default.md) | The native agent loop is unlimited by default; `--max-steps` / `/max-steps` / `.tny.json` `"steps"` set an explicit cap (0 = unlimited everywhere, libtny included) |
| [0025](0025-clipboard-images-paste-as-paths.md) | Ctrl-V materializes clipboard images and inserts the path as provider-neutral prompt text; `/image` remains the explicit attachment path |
| [0026](0026-color-vs-attribute-sgr.md) | `NO_COLOR` gates SGR colors only — bold/dim/reverse are structural; `--color=always`/`CLICOLOR_FORCE` force, `--color=never`/`--no-color` silence all SGR; dumb mode announces itself |
| [0027](0027-python-event-hooks.md) | Trusted global Python extensions consume versioned normalized events through one optional persistent host; `agent_end` may continue with visible context, `agent_settled` is final |
| [0028](0028-extension-parity-contract.md) | Extension parity is capability-scoped; one versioned vocabulary, immutable provider matrix, deterministic folding, and explicit unsupported states govern all lanes |
| [0029](0029-named-acp-agent-profiles.md) | Reusable ACP commands resolve as `acp:NAME`; requested models are applied through advertised session config options before the first prompt |
| [0030](0030-settings-schema-and-acp-map.md) | Publish a settings JSON Schema; provider/model/effort/fast become user defaults; canonical ACP profiles use `acp.NAME` and `acp@NAME` with legacy compatibility |
| [0031](0031-background-ask.md) | `tny ask -B` detaches the turn into a forked child keyed by the session id; flock is truth, pid file is control, session.json is the record; `session stop` signals the group, `--resume --steer` is interrupt-and-redirect |
| [0032](0032-libtny-capability-discovery.md) | libtny capabilities are a sized, side-effect-free snapshot separating availability, selection, initialization, reachability and packaging |
| [0033](0033-libtny-multi-runtime-cancel.md) | libtny supports independent runtimes; cancel is the sole cross-thread-safe operation and wakes the owner through the tny_poll seam |
| [0034](0034-native-python-and-node-sdks.md) | Python/cffi and TypeScript/Node-API packages are thin native schedulers over libtny with bounded ownership, cancellation and packaging contracts |
| [0035](0035-nix-flake-packaging.md) | Nix consumes tny through a first-party flake that builds from source; TLS resolves via a post-fixup RUNPATH, the version comes from the flake revision; native `nix flake check` covers all claimed systems |
| [0036](0036-libtny-host-services.md) | libtny host callbacks use a copied, versioned, owner-thread and non-reentrant v1 service table |
| [0037](0037-libtny-abi-1.md) | Active ABI 1 freezes capacity-aware prefix negotiation and ABI0.8 compatibility after immutable Linux consumers and full matrices passed |
| [0038](0038-libtny-custom-tools.md) | libtny custom tools use copied specs, native permission gating and generation-tagged async completion |
| [0039](0039-quality-gates.md) | First-party formatting, lint, strict warnings, static analysis, and workflow checks are required through `make quality` and CI |
| [0040](0040-ssh-agents-md.md) | `--ssh` / `/ssh` keep `$HOME/.tny/AGENTS.md` (labeled as local user policy) and load `AGENTS.md` from the remote cwd; launch-dir / ancestor project files stay out so the model is not instructed about the wrong tree |
| [0041](0041-session-wait.md) | `tny session <id> --wait [--timeout SECS]` blocks on the 0031 writer-lock probe until a background turn finalizes and exits with the turn's `exit_code` (124 on timeout) |
| [0042](0042-help-flag-alignment.md) | CLI parser flags and rendered help are checked in both directions, and root help must list every dispatched subcommand |
| [0043](0043-macho-version-floor.md) | Mach-O current_version floors at 1.0.0 for every pre-1.0 product version so 0.y.z release tags build libtny on macOS |
| [0044](0044-cursor-stays-on-sdk-bridge.md) | Cursor stays on the `sdk.v1` bridge; the private `agent.v1` HTTP/2 route measured slower per warm turn (2.2–2.6 s vs 1.3–1.4 s) and would cost HTTP/2, protobuf, and a CLI-pinned executor protocol |
| [0045](0045-system-prompt-flag.md) | `--system-prompt` rides the openai system/instructions field natively; cursor, codex and ACP pin no such schema field, so the engine prepends the text to the first user message of a fresh session |
| [0045](0045-monorepo-and-tnytty.md) | The repo is a monorepo by addition: the harness keeps the root (Pages/release/packaging pin root paths), sibling apps like `tnytty/` are self-contained top-level dirs sharing `third_party/` and the quality gates |
| [0047](0047-scriptable-workflow-dags.md) | Shell, Python and TypeScript expose one validated dependency DAG: bounded parallel tasks, ordered direct-output fan-in, blocked descendants, independent branch completion, and explicit cancellation |
| [0048](0048-runtime-task-presets.md) | Runtime-owned task presets with deterministic discovery, source-safe metadata, ABI 1.1 embedding, workflow isolation, and session-scoped resume semantics |
| [0049](0049-mcp-background-warmup.md) | Native sessions warm `~/.tny/mcp.json` servers on background threads at session start and inject a capped cached `server/tool` catalog into the system prompt; calls stay on `mcp_select_tool`, failures stay silent until a call, wasm stays lazy |
| [0050](0050-complete-cursor-sdk-v1.md) | Implement the complete public Cursor SDK Bridge v1.0.30 contract while retaining the external host and rejecting private agent.v1 |
| [0051](0051-mcp-streamable-http.md) | Remote MCP uses POST-only Streamable HTTP with legacy sessions and stateless 2026-07-28 negotiation; GET/SSE and SSE responses fail actionably; wasm is remote-only |
