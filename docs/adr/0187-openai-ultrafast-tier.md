# 0187 — OpenAI Ultrafast is a distinct Responses speed tier

Status: accepted

## Context

OpenAI's [Ultrafast guide](https://developers.openai.com/api/docs/guides/ultrafast-mode.md)
requires `service_tier: "ultrafast"` with `gpt-6-astra` or `gpt-6.1-sol`.
It explicitly supports HTTP Responses; WebSockets are recommended, not required.
Ultrafast has higher cost and separate rate limits. The existing fast selector
maps `fast`/`priority` to `priority`; treating Ultrafast as that alias would
silently request the wrong tier.

## Decision

- Add `--ultrafast`, `/fast ultrafast`, and `ultrafast` in the existing settings
  `fast` scalar/per-provider value. Retain the exact selector in `service_tier`.
- Limit support to the `openai` provider, Responses wire, and the exact two
  documented model IDs. Generic `TNY_CAP_FAST` is not evidence that other
  compatible providers or Codex support Ultrafast.
- Reject incompatible combinations at CLI/settings resolution, TUI selection,
  and before each native request. Failed TUI selection preserves the tier.
  Provider switches use existing failure-atomic staging. Changed models/wires
  cannot silently discard or downgrade the tier on the next turn.
- `--fast` and `--ultrafast` conflict; flags override settings. Existing fast,
  priority, standard omission and effort semantics are unchanged. Bare `/fast`
  toggles any paid tier off. Session/checkpoint/worktree copies already own the
  tier string; no public ABI or persistence layout changes are required.
- Use existing HTTP streaming, including wasm fetch. No new transport or OS
  seam. Existing browser auth/CORS and unsupported local execution limits apply.
- ChatGPT plan requests can carry the tier when the model is available, but
  entitlement is not guaranteed by the sign-in docs. Keep server errors visible.

## Verification

Focused C unit checks cover exact selectors, provider/model/wire rejection,
CLI parsing and flag/settings precedence, transactional provider switching,
TUI preservation/toggle behavior, and the captured first HTTP request for each
supported model. A schema-only check covers both settings shapes.
No end-to-end or live-inference tests are run, as requested. No performance
claim is made and no benchmark is required for this selector-only change.

Observed checks:

- `make -j8 debug release`: passed (ASan/UBSan unit binary and stripped release).
- `build/tny-test -s core_suite -t fast_`: 6 passed, including the new validation
  and CLI/settings cases.
- `build/tny-test -s core_suite -t settings_general_defaults`: 1 passed.
- `build/tny-test -s tui_suite -t fast_command`: 2 passed.
- `build/tny-test -s openai_suite -t ultrafast`: 1 passed. The fixture stops
  after capturing the first POST; no CLI or tool/inference loop is exercised.
- Schema-only `unittest discover ... -k SpeedTierSchemaTests`: 1 passed;
  enum/shape assertions run without optional `jsonschema` installed.
- Stripped macOS arm64 release: 25,212,368 bytes; dynamic dependencies remain
  `libSystem` and `libc++`. The linker warns that cached CPython archives target
  macOS 27.2 while the executable targets 27.0; build still succeeds.
- Nine additional focused core regressions passed: wire resolution/flags,
  failure-atomic provider switching, named profiles, scoped remembered models,
  OpenAI sign-in precedence/mode, and Codex/Grok profiles. Total: 19 unique
  focused C tests plus the schema-only check.
- `make -j4 quality`: passed on the final source tree. Darwin explicitly skips
  GCC `-fanalyzer`; no Linux analyzer or wasm runtime check was run locally.
