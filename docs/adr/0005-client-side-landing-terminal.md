# 0005 — Client-side landing terminal

Date: 2026-08-21
Status: accepted

## Context

The GitHub Pages landing page (`site/index.html`, same chrome as [fx.sh](https://fx.sh))
shipped a static terminal mock. Visitors asked to type in it. Embedding the C
harness as WASM is a documented non-goal (`docs/product.md`). A server-side
proxy would see API keys and contradict "host processes stay external" plus
the size budget (no Node on our side).

## Decision

The landing terminal is a **client-side BYOK preview** of the TUI chrome, not
tny itself.

- The browser talks **directly** to an OpenAI-compatible
  `/v1/chat/completions` endpoint. Nothing is posted to GitHub or a tny
  origin.
- The visitor supplies `OPENAI_API_KEY` and optionally `OPENAI_BASE_URL` /
  `OPENAI_MODEL` via the URL **hash** (preferred; never sent to GitHub Pages),
  the query string (stripped immediately), `/login`, `/setup`, or an
  `OPENAI_*=` assignment in the composer.
- `term-core.js` runs in `<head>` and `history.replaceState`s secret
  parameters out of the address bar before first paint.
- Credentials are sealed with **AES-GCM-256** (Web Crypto) using a
  non-extractable wrapping key in IndexedDB. The ciphertext lives in the same
  origin vault until `/logout`. Plaintext exists only in memory.
- `OPENAI_BASE_URL` is treated as a secret: never written into the DOM, never
  logged, shown only as an obfuscated `https://h••••t/***` form. `javascript:`
  and other schemes are rejected.
- The page sends `Referrer-Policy: no-referrer` so a fetch to the provider
  does not advertise the GitHub Pages URL.
- Slash commands that need a workspace (`@`, `$`, `/mcp`, host providers)
  degrade to "not available in the browser demo". The native tool loop here
  is `lookup_docs` plus conversation.
- This is **not** WASM tny and does not change the C size budget.
- API keys are sanitized at intake (`sanitizeApiKey` in `term-core.js`):
  whitespace and invisible characters (NBSP, zero-width, bidi marks, BOM) are
  stripped; any remaining non-printable-ASCII character is rejected with a
  clear error. `fetch()` requires header values to be ISO-8859-1, so an
  unsanitized pasted key with e.g. a smart quote or `…` would otherwise throw
  `Failed to read the 'headers' property from 'RequestInit'` at send time,
  far from the paste that caused it. Vault restores from before this rule
  drop invalid keys instead of resurrecting them.

## Consequences

Visitors can try the chrome against their own key (OpenAI, OpenRouter, or any
CORS-open gateway). Providers that refuse browser CORS fail closed with a
one-line error. XSS on this origin can still decrypt the vault — encryption
hides casual storage dumps and URL leakage, not a compromised page.
