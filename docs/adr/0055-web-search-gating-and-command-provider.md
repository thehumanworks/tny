# 0055 — Gate `web_search` on a configured provider; add a command provider

Date: 2026-09-02
Status: accepted (relates to 0017 wasm seams, 0022 `--ssh` remote tools)

## Context

tny ships no search engine. `web_search` was still listed in the native
loop's tools array, so a model that decided to search spent a call and a
round trip to receive `no web search provider configured`, then usually
tried again with `web_fetch`. The only provider was `web_search_url`, a URL
template fetched over HTTP, which cannot drive a headless browser such as
lightpanda or any engine that needs a client program.

## Decision

1. **Advertise only what works.** `tools_schema_json` drops `web_search`
   when neither `web_search_command` nor `web_search_url` is set
   (`tool_web_search_configured`). This is a schema filter, not the embedded
   `schema_tool_disabled` refusal: a direct caller (SDK, `--json` replay)
   still reaches `tools_web.c` and gets the runtime error, which now names
   both keys. `perm.c`, `custom_tools.c`'s reserved-name list, and the ACP
   kind map keep the name; only the advertisement changes.
2. **`web_search_command`.** A shell command template. The expanded command
   is handed to the `terminal` tool's execution path with a 60 s timeout, so
   it inherits the workspace cwd, the `--ssh` remote runner when one is
   active, the kill-on-timeout, output cap, and `tool_bound_result`
   handling. The result is the terminal-shaped `exit code: N` plus output.
   If both keys are set, the command wins.
3. **One placeholder rule.** Both keys accept `{query}` and `{{query}}`;
   every occurrence is replaced. The query is always percent-encoded, for
   the command template too: the encoded form contains only `A-Za-z0-9-_.%`,
   which is both valid inside a URL and a single safe shell word, so the
   model's query can never inject into the shell whether or not the user
   quoted the placeholder. In practice the placeholder lands inside a URL
   argument, where encoding is what the engine expects.
4. **wasm.** `web_search_url` is unchanged (browser fetch). The command
   provider is a clean error, `web_search_command is not available in
   wasm`, behind `#ifdef __EMSCRIPTEN__` in `tools_web.c`; the tool stays
   advertised because a provider is configured. This is a process-spawning
   seam of the same kind as `runner.c`'s stubs (0053), not a new transport.

## Consequences

- Models without a provider never see the tool; users who set either key
  see it exactly as before. `web_search_command` joins the reserved
  top-level settings keys, so it can never be mistaken for a provider
  profile.
- Command output is whatever the program prints; tny does not parse it.
  Users pick a program that emits text the model can read (markdown dumps
  work well).
- Covered by `tests/test_web_search.c`: schema gating for none / URL /
  command, both placeholder spellings and stray braces, a fake command that
  proves substitution and that shell metacharacters stay encoded, and the
  command-over-URL precedence.
