# 14 — Assert hash secrets leave `page.url`

Low. From the repository-wide test audit. Independent of 08/11 except
that it belongs in the same Playwright smoke.

`test_site_wasm.py` loads secrets via the URL hash and only checks they
are absent from DOM HTML. `tests/site/test_term.js` unit-tests
`history.replaceState` in isolation. Nothing in the browser asserts
`page.url` was scrubbed, so a bootstrap/history regression can leave
credentials in the address bar while every test stays green.

## Work

- After the wasm page has taken secrets from the location (banner
  painted is a sufficient wait), assert `page.url` contains neither the
  API key nor the mock base URL, and that `history.replaceState` left a
  shareable URL.
- Cover both the `OPENAI_*` hash used in the first smoke turn and the
  named-provider hash used in the second.
- Keep the existing DOM-HTML leak check; URL scrub is additional.

## Acceptance

- Commenting out the `replaceState` call in `takeSecretsFromLocation`
  fails `test_site_wasm.py` on `page.url`, not only the helper unit test.
- The key still never appears in `page.content()`.
