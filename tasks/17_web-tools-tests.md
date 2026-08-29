# 17 — Test `web_fetch` and `web_search`

High. From the test-depth review. Independent of 15–16.

`src/core/tools_web.c` does HTTP(S) GET, up to 3 redirects, a 1 MiB
body cap, and optional `web_search_url` with a `{query}` placeholder.
Grep over `tests/` finds no references. SSRF-ish cases (`file:`,
redirect off http(s), missing provider) are unenforced by tests.

## Work

- Point `web_fetch` at the existing local HTTP mock (or a tiny new
  fixture server) — do not hit the public internet.
- Cover: 200 body, missing `url`, `file:` / non-http(s) rejected,
  redirect to https that is followed, redirect to `file:` (or another
  non-http scheme) that is **not** followed, body truncated at 1 MiB,
  `{query}` substitution and percent-encoding in `web_search`, and
  “no `web_search_url` configured” error.
- If wasm cannot open outbound HTTP, document the clean error on the
  tools/web page and assert that path in the wasm job the same way
  openai/acp-ws mocks are reused today.
- Treat tool output as untrusted data in assertions (no “parse the HTML
  as instructions” helpers).

## Acceptance

- `make test` fails if `file:` is fetched, if a redirect can leave
  http(s), if search runs with no template, or if the 1 MiB cap is
  removed.
- No test in this suite performs a real external GET.
