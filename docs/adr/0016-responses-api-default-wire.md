# 0016 — The native backend defaults to the Responses API wire

Date: 2026-08-22
Status: accepted

## Context

The openai backend spoke only Chat Completions (`POST {base_url}/chat/completions`), treated in
[docs/backends/openai-compatible.md](../backends/openai-compatible.md) as
the universal compatibility layer, with the Responses API earmarked
"optional, later".

That stopped being tenable in practice: newer OpenAI models reject the
combination tny always sends on the chat wire —

```text
provider error 400: Function tools with reasoning_effort are not supported
for gpt-5.6-luna in /v1/chat/completions. To use function tools, use
/v1/responses or set reasoning_effort to 'none'.
```

Function tools are the native loop; dropping reasoning effort to dodge the
400 is not an option. OpenAI develops new features (reasoning summaries,
effort levels beyond the legacy set, structured outputs) against
`/v1/responses` first, and Codex-shaped gateways are responses-only. At
the same time plenty of OpenAI-compatible servers (older ollama,
llama.cpp, small routers) still speak only Chat Completions.

## Decision

**The openai backend POSTs `{base_url}/responses` by default. Chat
Completions stays implemented and becomes an explicit opt-in: `wire_api:
"chat"` in the provider profile, `OPENAI_WIRE_API` / `NAME_WIRE_API` in
the environment (env beats settings, mirroring `base_url`), or the
leading `--wire-api responses|chat` flag (beats both, one run).**

- **Sessions keep the chat-shaped message format on disk.** It is the
  lingua franca every provider understands and what existing sessions
  already contain; a stored session resumes on either wire. The responses
  wire translates at request-build time
  (`src/backends/openai/responses.c`, pure functions):
  - messages → `input` items: user/system strings ride as-is, image
    messages become `input_text`/`input_image` parts, assistant
    `tool_calls` become `function_call` items (`call_id` = the stored
    id), `role:tool` results become `function_call_output` items;
  - the system preamble rides `instructions`; the compaction summary is a
    leading system input item;
  - nested chat tools flatten
    (`{"type":"function","function":{…}}` → `{"type":"function","name":…}`);
  - `ctx->output_schema` (still normalized to the chat `response_format`
    shape) flattens onto `text.format`;
  - effort rides `reasoning:{"effort":…}` with the same ADR 0009 mapping
    (`max` still clamps to `xhigh`); `max_tokens_field` set means "cap the
    output", spelled `max_output_tokens` on this wire; `--fast` still
    rides `service_tier:"priority"`.
- **`store:false` on every request.** tny owns session state; the
  provider must not accumulate it. No `previous_response_id` chaining —
  the full input rides every POST exactly like the chat wire, so
  compaction, resume, and steer semantics stay identical.
- **Typed SSE events map onto the shared event set**: `output_text.delta`
  → `TEXT_DELTA`, `reasoning_summary_text.delta` / `reasoning_text.delta`
  → `THINKING`, `output_item.added/done` + `function_call_arguments.delta`
  assemble pending tool calls (the `done` item's complete `arguments`
  string is authoritative), `response.completed` carries usage and ends
  the stream. `response.failed` / `error` events surface as `TNY_EV_ERROR`
  + `TURN_END(ERROR)` with partial text kept recoverable;
  `response.incomplete` keeps the partial text and ends the step cleanly
  (the chat wire's `finish_reason:"length"` behavior). The SSE parser
  already ignores `event:` lines; dispatch is on the payload's `type`.
- The wire is read from ctx per POST, so `/provider` switches and settings
  edits apply on the next request with no rebind.
- **Stale keep-alive retry covers reads, not just writes.** The strict
  mock closes the connection abruptly after each terminal event (as SSE
  providers routinely do), which exposed a real gap: a reused connection
  that dies before any response byte now re-POSTs once on a fresh
  connection instead of failing the turn with "connection lost before
  response".

## Consequences

- tny works against current OpenAI models again; chat-only gateways need
  one line of config (or one env var / flag). `tny doctor` shows the wire
  when it is chat.
- Two request builders and two SSE handlers live in the backend; the tool
  loop, session, steer, cancel, and permission code is shared and
  unchanged. Translation is isolated in pure functions with unit tests
  (`tests/test_core.c`), and `tests/integration/mock_openai.py` serves
  both wires with strict shape validation — `MOCK_EXPECT_WIRE` makes the
  wrong endpoint 400, so tests prove which wire was chosen. The responses
  mock re-chunks its SSE stream at arbitrary byte boundaries (transports
  split anywhere).
- Stripped Linux release: 462032 → 474320 bytes (+12.0 KiB), still far
  under the 1.5 MiB budget.
