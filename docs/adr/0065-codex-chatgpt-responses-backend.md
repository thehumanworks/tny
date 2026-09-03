# 0065 — Codex runs on the ChatGPT Responses backend, not `codex app-server`

Date: 2026-09-03
Status: accepted (supersedes the codex parts of 0002, 0004, 0011, 0013,
0017, 0019, 0031, 0045; retires `docs/backends/codex-app-server.md`)

## Context

Since the first release, `--provider codex` meant a **host backend**: tny
spawned (or attached to) `codex app-server`, spoke its JSON-RPC-shaped
WebSocket dialect, and mapped its items, approvals, steer, and login
notifications onto the shared event set. That bought Codex's own harness —
and cost a lot:

- **A second agent loop we did not own.** Permissions were advisory,
  `--ssh` was refused, tool profiles and OS sandboxing did not apply, MCP
  and skills were the host's, extensions saw a reduced capability matrix,
  and every new native feature (steer ownership, shell-first tools,
  in-process verbs, forked runners) needed a codex-specific mapping or an
  exception.
- **Protocol drift.** The app-server surface is version-pinned
  (`rust-v0.149.1`), experimental on TCP, and moved under us (item keys,
  login RPCs, `-32001` retry rules). Eight source files, a scripted mock,
  a host registry, and a pre-warm path existed only to keep up.
- **Process plumbing.** Ephemeral ports, bearer token files, stderr
  drains, orphan reaping, a `~/.tny/codex-host.json` registry so one-shots
  could adopt a TUI's host, wasm attach-only rules.

Meanwhile the Codex CLI itself talks to a plain **Responses-compatible
HTTP backend** with the user's ChatGPT OAuth token:

```text
POST https://chatgpt.com/backend-api/codex/responses
Authorization: Bearer <access_token>      # from $CODEX_HOME/auth.json
chatgpt-account-id: <account_id>
OpenAI-Beta: responses=v1
{ "model": …, "instructions": …, "input": [ … ], "stream": true, "store": false }
```

That is the wire tny already speaks by default ([ADR 0016](0016-responses-api-default-wire.md)):
`store:false`, `stream:true`, `instructions`, an `input` item array (the
Responses API accepts a string or an array; function-call items only exist
in the array form, which tny always sends). The only differences from
`api.openai.com` are the base URL, the two headers, and where the bearer
comes from.

## Decision

1. **`codex` becomes a builtin profile of the openai backend** (like
   `claude` and `grok`, [ADR 0019](0019-subscription-logins-claude-grok.md)),
   with base URL `https://chatgpt.com/backend-api/codex`, the Responses
   wire, and the two extra headers on `ctx->extra_headers`. `TNY_BK_CODEX`,
   `src/backends/codex/`, the host registry, `--codex-ws`,
   `--ws-token-file`, `CODEX_REMOTE_TOKEN`, and `TNY_CODEX_WS` are removed.
   The backend enum is `openai | cursor | acp`. Provider name, `--provider
   codex`, `/provider codex`, `models.codex`, `effort.codex`, `fast.codex`,
   and `"provider":"codex"` in JSON output are unchanged for users.

2. **tny reads `$CODEX_HOME/auth.json` directly** (`src/core/codex_auth.c`):
   `tokens.access_token` is the bearer; the account id is
   `tokens.account_id` or the `https://api.openai.com/auth`
   `.chatgpt_account_id` JWT claim; an `OPENAI_API_KEY` entry selects
   `api.openai.com` in API-key mode. The file's presence keeps its place
   at the head of the auto-detection order.

3. **tny refreshes the token itself.** Without the CLI's background
   refresher a login would die with its first access token, so at provider
   resolve tny runs the CLI's own grant — `POST
   https://auth.openai.com/oauth/token` with client id
   `app_EMoamEEZ73f0CkXaXp7hrann`, `grant_type=refresh_token` — when the
   access token's `exp` is within 60 s or `last_refresh` is older than
   eight days, and writes the rotated tokens back into `auth.json` (0600,
   atomic). This mirrors the grok refresh in [ADR 0021](0021-native-grok-device-login.md)
   and honors the CLI's `CODEX_REFRESH_TOKEN_URL_OVERRIDE`.

4. **Login stays in the Codex CLI; logout goes native.** `tny --provider
   codex login [--device]` runs `codex login` / `codex login
   --device-auth` (the CLI owns the localhost PKCE callback and device
   flow), then reports whether `auth.json` appeared. `tny --provider codex
   logout` deletes `auth.json` — what `codex logout` does — without needing
   the CLI. `--codex-bin` / `TNY_CODEX_BIN` name the helper; `doctor`
   reports it as a login helper only.

5. **Nothing codex-specific remains in the loop.** Effort maps through the
   openai table (`max` → `xhigh`), `--fast` is `service_tier:"priority"`,
   `--system-prompt` rides `instructions`, steer is the native loop's,
   permissions/sandbox/tool profiles/`--ssh`/extensions are the native
   ones, sessions are tny transcripts (no host pointer), and the TUI
   pre-warm does not apply (a per-turn HTTPS request has no host to warm;
   MCP warm-up still runs as for every openai profile).

6. **Overrides that keep the profile.** `TNY_CODEX_BASE_URL` redirects the
   ChatGPT-mode URL for mocks and gateways *with* the headers; a settings
   or env profile named `codex` still **shadows** the builtin entirely,
   exactly as for `claude`/`grok`.

## Consequences

- Users with a `codex login` get tny's full native harness: real
  permissions and OS sandboxing, shell-first tools and in-process verbs,
  `--ssh`, MCP/skills/extensions parity, `tny acp` over their subscription.
  Codex's own approvals/threads/plan items are gone; that harness is the
  Codex CLI's.
- The binary loses eight source files, wslay stays only for ACP-over-WS;
  stripped Linux size drops (measured 826 KB, well under the 1 MiB gate).
- No spawn means no pre-warm, no host registry, no orphan risk, and
  identical behavior on wasm: `test_codex_chatgpt.py` replaces
  `test_codex_attach.sh` in the wasm CI job. `tests/bench/bench_ttft.py`
  now benches the TUI and `ask` paths against the openai mock; the codex
  spawn-vs-attach bench no longer exists (there is nothing to attach to).
- `tests/integration/mock_openai.py` gains `MOCK_EXPECT_HEADERS` /
  `MOCK_REJECT_HEADERS`; `mock_codex_ws.py`, `test_codex.sh`,
  `test_codex_attach.sh`, and `tests/test_codex.c` are deleted.
- Removed flags fail as unknown flags. Session transcripts created by the
  old backend (`host_pointer` = thread id) resume as empty native sessions;
  the Codex thread itself is still `codex resume`-able in the CLI.
- Risk: the ChatGPT backend is not a public API contract. tny sends only
  what the user-facing Codex client documents (bearer, account id, beta
  header, the standard Responses body); if OpenAI adds a required header,
  it lands in `apply_codex` in `profiles.c` and this ADR. The API-key mode
  and any OpenAI-compatible profile are the fallback.
