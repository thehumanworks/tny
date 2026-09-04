# 0069 — Native loop: stream-error recovery, transcript invariants, and reasoning passthrough

Date: 2026-09-04
Status: accepted (extends [ADR 0016](0016-responses-api-default-wire.md) and
[ADR 0028](0028-extension-parity-contract.md))

## Context

Users of hosted gateways (OpenRouter, AIProxy, the ChatGPT `codex` backend)
reported the same shape of failure: a turn runs a tool call, the follow-up
model call fails ("stream error"), and from then on the conversation cannot
recover — every later prompt fails too. A review of the native loop found
several independent causes that all end there:

1. **No retry.** The tool-result POST is the request most likely to hit a
   gateway hiccup (an upstream 502/503/529, a 429, an idle connection the
   gateway closed, an SSE `error` event relayed from upstream). Any of them
   ended the turn with `TNY_STOP_ERROR`. The transcript already held the
   assistant tool calls and their results, so the user had to type
   "continue" and hope the next request landed.
2. **Misread failures.** Some gateways put `"error": null` into every chunk;
   the chat parser treated the member's presence as a failure and ended
   every stream with "provider stream reported an error". A JSON error
   document behind HTTP 200 (`Content-Type: application/json`) parsed as no
   SSE events at all, so an empty 200 — a gateway idle timeout — was recorded
   as a *successful* empty answer.
3. **Transcripts strict providers reject.** An empty assistant message
   (`content: ""`) was persisted whenever a stream ended with no text;
   Anthropic-fronted and other strict endpoints reject it on every later
   request. The assistant tool-call batch is persisted *before* the tools run
   (deliberately, for recovery), so a runner killed mid-batch — OOM, `kill
   -9`, power loss, an allocation failure — leaves calls without results and
   the provider answers "no tool output found for function call …" forever.
4. **Reasoning dropped.** Thinking models fronted by OpenRouter (Anthropic
   signed thinking blocks, Gemini thought signatures), DeepSeek/Kimi
   (`reasoning_content`), and OpenAI reasoning models on the Responses wire
   with `store:false` (encrypted `reasoning` items) require the reasoning
   that produced a tool call to be sent back with that call. tny displayed it
   and discarded it, so the tool-result request was rejected — again on every
   later turn, because the offending assistant message stays in the
   transcript.
5. **Opaque diagnostics.** Every HTTP failure surfaced as "provider returned
   HTTP N", the error body was never read (and its bytes poisoned the
   kept-alive connection for the next POST), and a proxy's 403 was reported
   as a bad API key.

## Decision

### Bounded, non-blocking retry of one model call

A model call that fails **before anything user-visible was produced** is
retried on a fresh connection with exponential backoff: 1s, 2s, 4s (a
longer numeric `Retry-After` wins, capped at 30s), three retries by default.
`TNY_PROVIDER_RETRIES=N` changes the budget (0 disables). Retryable:

- HTTP 408, 409, 425, 429, and every 5xx (the body is read for its
  category, then the connection is dropped);
- the connection lost before any response byte (a reused keep-alive still
  gets its immediate single re-POST first, as before);
- a stream aborted, or closed empty (no event, no text, no call, no
  finish reason), before any output;
- a terminal SSE error whose numeric code is retryable or whose category
  is not permanent (`invalid_*`, `context_length_*`, `insufficient_*`,
  `model_not_found`, `not_found`, `unsupported*`, `unknown_model`,
  `billing*`).

The backend parks in `ST_RETRY_WAIT` and publishes the deadline through the
existing `poll_timeout` vtable hook, so the engine's single event loop
sleeps instead of the backend; cancel still interrupts immediately. Each
retry is announced as a `TNY_EV_STATUS` line (`provider error (HTTP 502):
retrying in 1.0s (attempt 2/4)`), and every attempt is a distinct
`provider_request`/`provider_response` extension pair, as ADR 0028 defines.
Once answer text has been streamed, a failure stays terminal and the
partial text stays recoverable — retrying would print the answer twice.
Reasoning already shown does not block a retry: a gateway that dies after a
long think is the common case, and repeating dim reasoning costs far less
than losing the turn. A failed re-POST (the gateway itself restarting)
consumes a retry rather than ending the turn.

### Failures are classified, never echoed

Diagnostics carry the HTTP status (or the in-stream numeric code) and a
**category token**: the provider's `error.type` (else string `error.code`)
reduced to a lowercase identifier of at most 32 bytes — anything that is not
an identifier is dropped whole. Message text never reaches stdout, stderr,
the session, or extensions (ADR 0028), because it can echo credentials or
request content. `TNY_DEBUG_PROVIDER_ERRORS=1` is the explicit opt-in that
appends the provider's message (control characters blanked, 400 bytes) for
local debugging. 401 stays an authentication error; 403 is worded as a
refusal (a key without access to the model, or a gateway/proxy block).

Payload shapes recognised: `{"error":{…}}` and `{"message":…,"type":…}`
bodies, `"error": <object|string>` inside chat chunks (`null` is not an
error), `finish_reason: "error"`, Responses `response.failed` (nested
`response.error`) and bare `error` events, and a chat-shaped `{"error":…}`
document on the Responses endpoint. Body framing is sniffed from the
first bytes (gateways have streamed SSE under `application/json`): a body
opening with `{` is one JSON document — a chat completion, a whole Response
object, or an error behind HTTP 200 — dispatched as one event, which also
makes non-streaming gateways work; a document past 1 MiB is a terminal
"response too large" error rather than a retry. Error bodies are read
through the same non-blocking body path, bounded to 1.5s and 64 KiB,
before the connection is dropped.

### The provider sees a sound transcript

`session_provider_view()` builds what rides the wire from `messages[]`
without touching the stored transcript: every id in an assistant
`tool_calls` batch gets exactly one `role:tool` message before anything
else follows it (a missing one is synthesized as `error: tool result missing
(the turn was interrupted before this call finished)`), orphan or duplicate
tool results are dropped, and assistant messages with neither text nor
calls are skipped. Both wires build from the view; a repair is announced
once per turn as a status line. `finish_turn_ok` no longer records an empty
assistant message at all. Compaction boundaries start at user messages, so
the view never splits a batch.

### Reasoning rides back with the tool calls it produced

Mirror rule: **the reasoning member a provider streams is the member it
accepts back**, so nothing is sent to a provider that did not produce it.
Persisted on the assistant tool-call message only (final text messages
carry none; DeepSeek historically rejected it there):

| Wire | Streamed as | Stored as | Sent back as |
| --- | --- | --- | --- |
| chat | `delta.reasoning_details[]` (OpenRouter; fragments merged by `index`, textual members concatenated, signature kept) | `reasoning_details` | verbatim on the message |
| chat | `delta.reasoning_content` (DeepSeek, Kimi, vLLM) | `reasoning_content` | verbatim on the message |
| responses | `reasoning` output items with `encrypted_content` (`output_item.done`) | `reasoning_items` (tny-private; stripped from the chat wire) | the items, ahead of the message's `function_call` items |

The Responses request adds `include: ["reasoning.encrypted_content"]` for
`api.openai.com`, `chatgpt.com`, and the `codex` profile — the hosts known
to accept it; other gateways may reject unknown include values, and they
still get echoed items when they stream any. A reasoning item without
`encrypted_content` is never echoed: with `store:false` the provider cannot
resolve its id.

## Consequences

- A gateway hiccup on the tool-result request costs seconds, not the turn;
  a permanently broken session (unpaired calls, empty messages) heals on the
  next request instead of failing forever.
- Thinking models behind OpenRouter, DeepSeek-style providers, and OpenAI
  reasoning models keep working past their first tool call.
- Diagnostics name the failure class (`provider rate limit (HTTP 429)`,
  `provider rejected the request (HTTP 400, invalid_request_error)`) without
  leaking provider text; the opt-in switch exists for the cases where the
  text is the only clue.
- Two new session message members (`reasoning_details`, `reasoning_content`)
  ride the chat wire verbatim and one private one (`reasoning_items`) is
  translated on the Responses wire; old sessions lack all three and behave
  exactly as before.
- Fixtures: `mock_openai.py` gained the fault-injection and reasoning knobs
  (`MOCK_FAIL_TOOL_POST_ONCE`, `MOCK_FAIL_TOOL_POST`, `MOCK_EMPTY_STREAM_ONCE`,
  `MOCK_STREAM_ERROR_ONCE`, `MOCK_JSON_BODY_ONCE`, `MOCK_ERROR_NULL`,
  `MOCK_REASONING`, `MOCK_EXPECT_INCLUDE`, `MOCK_REJECT_INCLUDE`), and
  `tests/integration/live_provider_check.py` runs the same tool-call →
  follow-up → resume sequence against a real provider profile for manual
  verification.
