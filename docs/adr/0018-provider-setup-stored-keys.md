# 0018 — `provider setup`: guided profiles, stored keys, named pairs in the browser

Date: 2026-08-22
Status: accepted (builds on the named-provider profiles from settings.json /
`NAME_BASE_URL` env and on the wasm terminal from [0017](0017-wasm-browser-parity.md))

## Context

Named OpenAI-compatible providers existed but were expert-only: hand-edit
`settings.json` or export `NAME_BASE_URL`/`NAME_API_KEY`. Keys came from
env vars exclusively (`api_key_env`), which the browser wasm terminal has
no good way to supply beyond the `OPENAI_*` hash parameters — so the page
was effectively locked to one provider, and the native CLI had no guided
path at all.

## Decision

One writer, three fronts:

- **`tny provider setup NAME`** writes (merges into) a settings.json
  profile: `base_url`, `api_key` **or** `api_key_env`, `model`,
  `wire_api`. Flags carry every field; on a tty, missing fields are
  prompted for interactively (the key with echo off). It also sets
  `last_provider`, so a bare `tny` immediately runs on the new provider.
  `tny provider` / `tny provider list` is the existing providers listing.
- **`/provider setup [NAME]`** in the TUI is the same flow as a composer
  Q&A (name → base url → key or `$ENV_NAME` → model), `/cancel` aborts.
  Because it is ordinary TUI input, it works identically in the browser
  wasm terminal — where it is the primary way to add a provider.
- **The page hash accepts named pairs**: any
  `NAME_{API_KEY,BASE_URL,DEFAULT_MODEL,WIRE_API}` parameter is captured
  by `takeSecretsFromLocation` (sanitized at intake, stripped from the
  URL) and injected into the wasm environ, where the CLI's existing
  sole-pair auto-detection selects the provider. No new config surface —
  the browser rides the same env contract the shell does.

**Stored keys.** `--api-key` (and the wizard's key answer) stores the key
in `~/.tny/settings.json`, which drops to mode 0600. Precedence is
unchanged in spirit: **an environment variable always beats a stored
key** (`api_key_env`/`NAME_API_KEY` first, the profile's `api_key` as the
fallback), so shell-side rotation and CI secrets keep working without
editing files. Storing a key clears `api_key_env` and vice versa — one
effective source, never a silent shadow. `--api-key-env` remains the
recommendation for shared machines; the interactive flows say so.

Host providers (cursor/codex/acp) are refused by `provider setup` — they
have no base_url/key shape. Reserved settings keys (`workspaces`,
`models`, `permission`, `effort`, `last_provider`) can never become
profile names.

## Consequences

- `settings.json` may now contain a secret. It is never committed (it
  lives under `$HOME`), the file drops to 0600 the moment a key lands in
  it, and `tny` never prints stored keys. In the browser the "file" is
  per-tab MEMFS, gone when the tab closes.
- The old `tny setup` (openai-object-only, flags-only) stays for
  compatibility; `provider setup openai` writes the same object with the
  guided flow.
- On wasm-node (the CI vehicle) the flag path works and is tested; the
  tty prompt path stalls on Emscripten's `fgets` over a node pty and is
  deliberately untested there — interactive setup on wasm is the TUI
  wizard. `TCSADRAIN`, never `TCSAFLUSH`, around the echo-off prompt:
  FLUSH discards queued input and eats a fast paste (found by the pty
  test racing the prompt).
- Enforced by: unit tests (precedence, writer rules),
  `test_provider_setup.sh` (native + wasm in CI), a `/provider setup`
  pty test, `test_term.js` hash-intake cases, and a named-provider
  scenario in the browser smoke.
