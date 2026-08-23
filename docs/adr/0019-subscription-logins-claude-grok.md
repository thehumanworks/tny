# 0019 — Subscription logins: app-server codex login, builtin claude and grok profiles

Status: accepted. Decision 4 is amended for grok by
[ADR 0021](0021-native-grok-device-login.md): the device-code login,
token refresh, and logout are now native — no grok CLI required.

## Context

Until now tny only *reused* subscription credentials that other CLIs had
already minted: `tny_codex_auth_present()` detected `$CODEX_HOME/auth.json`,
and `tny login` for codex was a bare `system("codex login")`. There was no
way to sign in to Codex from inside tny's own transport, no Claude
subscription path at all (Claude was an OpenRouter model id or an ACP
agent), and no Grok provider.

Meanwhile the credential surfaces are well defined:

- **Codex**: `codex app-server` v2 exposes `account/login/start`
  (`{"type":"chatgpt"}` browser flow returning `authUrl`;
  `{"type":"chatgptDeviceCode"}` returning `verificationUrl` + `userCode`)
  and signals the result with the `account/login/completed` notification.
  The host owns the OAuth ceremony, the localhost callback, token storage
  (`auth.json`), and refresh.
- **Claude**: Claude Code OAuth tokens (`claude setup-token`, or the
  `claudeAiOauth.accessToken` in `~/.claude/.credentials.json` from
  `claude /login`) authenticate against the Anthropic API as
  `Authorization: Bearer` **plus** the `anthropic-beta: oauth-2025-04-20`
  header. They are subscription credentials, distinct from Console API keys
  (`ANTHROPIC_API_KEY`, normally `x-api-key`). Anthropic's OpenAI-compat
  surface is `/v1/chat/completions`.
- **Grok**: `grok login --device-auth` (RFC 8628) stores a session token in
  `~/.grok/auth.json` under `"https://accounts.x.ai/sign-in".key`. That
  token drives the OpenAI-compatible CLI chat proxy
  (`https://cli-chat-proxy.grok.com/v1/chat/completions`) when
  `X-XAI-Token-Auth: xai-grok-cli` rides along; the proxy routes models via
  the `x-grok-model-override` header. `XAI_API_KEY` against
  `https://api.x.ai/v1` is the keyed fallback.

## Decision

1. **`tny --provider codex login [--device]` drives the app-server login
   RPC.** It connects like any codex turn (attach or spawn), calls
   `account/login/start`, prints the auth URL (best-effort browser launch)
   or the device code, and pumps the socket until
   `account/login/completed`. The host must stay alive throughout — it
   serves the localhost OAuth callback. Hosts that reject the method fall
   back to `system("codex login")`. tny never reads, parses, or prints the
   tokens; success is observed via `tny_codex_auth_present()`.

2. **`claude` and `grok` become *builtin profiles*, not new backends.**
   Both speak OpenAI-compatible HTTP, so they run on the openai backend the
   way user-named profiles do (`provider_name` + config swap), keeping the
   backend enum at four and the size budget untouched. A settings.json
   object or `NAME_BASE_URL` env var named `claude`/`grok` **shadows** the
   builtin — explicit user config always wins.

3. **Credential resolution is read-at-resolve, never persisted.**
   - claude: `CLAUDE_CODE_OAUTH_TOKEN` → `ANTHROPIC_API_KEY` →
     `~/.claude/.credentials.json` (`$CLAUDE_CONFIG_DIR` honored). OAuth
     tokens (by source, or the `sk-ant-oat` prefix) add the
     `anthropic-beta: oauth-2025-04-20` header; Console keys do not.
     Wire is `chat`; default model `claude-sonnet-4-6`.
   - grok: session token from `~/.grok/auth.json` → CLI chat proxy on the
     `chat` wire with the `X-XAI-Token-Auth` and `x-grok-model-override`
     headers, default model `grok-build`; else `XAI_API_KEY` → `api.x.ai`
     on the default Responses wire, default model `grok-4.6`.
   These ride a new `ctx->extra_headers` list the openai backend appends to
   every request. Like every other secret, tny stores env-var *names* and
   host-owned files, never token values.

4. **Login ceremonies stay in the host CLIs where they exist.**
   `tny --provider claude login` reports the resolved credential or runs
   `claude setup-token` (the token prints to the user, who exports it —
   tny does not capture it). `tny --provider grok login` runs
   `grok login --device-auth`; the grok CLI owns `auth.json` and refresh.
   This follows the repo invariant: host processes stay external, tny does
   not re-implement OAuth flows a host binary already ships.

5. **Auto-detection order** (docs/cli.md) now prefers subscription logins
   as: codex `auth.json` → claude OAuth artifacts (`CLAUDE_CODE_OAUTH_TOKEN`
   or the credentials file — a bare `ANTHROPIC_API_KEY` never hijacks the
   default) → grok `auth.json` → `CURSOR_API_KEY` → openai.

## Consequences

- Users with only a ChatGPT/Claude/Grok subscription can start from zero:
  `tny --provider codex login`, `tny --provider claude login`, or
  `tny --provider grok login`, then `tny ask "hi"`.
- `tny providers` lists claude and grok rows with credential hints;
  `/provider` and `--provider` accept the names everywhere.
- The grok proxy path pins the model-override header at resolve time; a
  mid-session `/model` switch on the proxy takes effect at the next
  provider resolve (documented in backends/openai-compatible.md).
- The codex login flow is covered by `tests/integration/test_codex.sh`
  run 7 against the scripted mock; profile resolution by
  `tests/test_core.c` (`builtin_*` tests).
