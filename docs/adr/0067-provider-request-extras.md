# 0067 — Per-provider request extras as a detachable table

Date: 2026-09-03
Status: accepted

## Context

Hosted OpenAI-compatible gateways sometimes ask clients to carry one small,
provider-specific extra on every request. OpenCode Go (`opencode.ai/zen/go`)
is the first concrete case: its support team notified us that requests
without an `x-opencode-session` header — one stable id per conversation,
used for prompt-cache routing and abuse detection — may start erroring
from 2026-09-06. tny reaches OpenCode Go like any other custom provider:
a settings profile with a `base_url`, or the `OPENCODEGO_BASE_URL` /
`OPENCODEGO_API_KEY` env pair.

The existing seam for provider-specific headers is `ctx->extra_headers`,
filled by the builtin subscription profiles in `profiles.c` (claude oauth
beta, grok proxy auth, codex account id — [ADR 0019](0019-subscription-logins-claude-grok.md),
[ADR 0065](0065-codex-chatgpt-responses-backend.md)). It is the wrong
place for this ask for two reasons:

- It is resolved once per provider, before a session exists, so it cannot
  carry a per-conversation value.
- It would put a third party's operational quirk into the profile
  resolver, coupling it to `tny_resolve_backend`, the settings schema, and
  every custom-provider code path. These asks come and go with the vendor;
  they should be trivial to change or delete.

## Decision

1. **One module, one table.** `src/core/provider_extras.c` holds a static
   array of entries `{name, match(scope), headers(scope, out, cap)}`. An
   entry sees only a `tny_request_scope` — provider name, base URL, session
   id — and returns malloc'd header lines. Adding or removing a provider
   quirk is adding or deleting one row plus its two functions; nothing else
   in the tree references the entry.
2. **One call site.** The openai backend calls
   `tny_provider_extras_headers` once per POST, after the profile's
   `extra_headers`, and frees the result after the request. No other
   backend, the profile resolver, the settings schema, and `tny provider
   setup` know the module exists. Catalog requests (`GET /models`) are not
   conversations and do not carry per-conversation extras.
3. **Matching is by evidence, not configuration.** An entry matches on the
   base URL host (`opencode.ai` or a subdomain, dot-boundary checked) or on
   the effective profile name (`opencode*`, the shape `NAME_BASE_URL` env
   profiles produce). Users never opt in; a user who names their profile
   `opencodego` or points it at `opencode.ai` gets the header.
4. **One kill switch.** `TNY_PROVIDER_EXTRAS=0` (also `off` / `false`)
   disables every entry, for debugging a gateway or proving a request is
   otherwise vanilla.
5. **The OpenCode Go entry** sends `x-opencode-session: <tny session id>`.
   The session id is already the one stable id per conversation (it
   survives `--resume`, background asks, and attach), so no new identifier
   is minted or persisted.

## Consequences

- Removing OpenCode Go support later is deleting one row and its two
  functions from `provider_extras.c` plus the tests that name it.
- A future vendor ask of the same shape (a client tag, a tenant header) is
  a new row, not a new field on `tny_ctx` or in `settings.json`.
- Header lines are built per request; the cost is one small allocation per
  matched entry per POST, nothing on unmatched providers beyond a URL parse.
- wasm: pure string handling on the shared source set, so the browser build
  sends the header too — `fetch()` passes custom headers through, subject to
  the gateway's CORS allowlist.
- Tests: `tests/test_provider_extras.c` (matching, dot boundaries, empty
  session, kill switch) and `tests/integration/test_provider_extras.py`
  (header on the wire, same value across `--resume`, absent on other
  providers, kill switch) via the mock's `MOCK_HEADER_LOG`.
