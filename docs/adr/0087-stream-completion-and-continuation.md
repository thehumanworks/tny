# 0087 — Stream completion contract: an interrupted stream continues, never truncates

Date: 2026-09-12
Status: accepted (amends [ADR 0069](0069-native-loop-stream-error-recovery.md);
relates to [0016](0016-responses-api-default-wire.md),
[0053](0053-forked-turn-isolation.md), [0065](0065-codex-chatgpt-responses-backend.md))

## Context

Users of the `codex` profile (the ChatGPT Responses backend, ADR 0065) —
and at times other hosted OpenAI-compatible gateways — reported turns that
"stop abruptly with the response interrupted mid-stream". Driving the
pre-change binary against a scripted provider reproduced three distinct
shapes behind that one symptom:

1. **Silent truncation.** The body ended cleanly — a chunked terminator or
   the Content-Length reached — without `response.completed` (or `[DONE]`
   on the chat wire). `oa_dispatch` treated any body end as a finished step:
   the fragment was persisted as the assistant's answer, the turn ended
   `done`, exit code 0, and nothing was printed. The same path ran a
   function call whose arguments had been cut mid-JSON, because the call
   set was non-empty. This is what a proxy in front of a dying upstream
   produces: it closes the client body properly, it just never got the end.
2. **A terminal error with budget to spare.** The socket was reset mid-body
   after some answer text. ADR 0069 made every failure after text terminal
   ("retrying would print the answer twice"), so the turn ended with
   `stream aborted mid-response` even though the provider would have
   answered a second request, and the user had to type "continue".
3. **A hang.** The stream went silent — a half-open TCP connection after a
   NAT or load-balancer drop, or a hung upstream — and tny waited on the
   open socket forever; only Ctrl-C ended the turn. The backend had no
   notion of time on an open stream.

The Codex CLI drives the same backend with a whole-request retry
(`stream_max_retries`, five by default) and a 300-second stream idle
timeout, accepting that the retried answer streams twice. tny cannot
accept the duplicate: `tny ask` streams answer text to stdout as it
arrives, and the transcript records what was shown.

## Decision

### 1. A response is complete only when its terminal event arrived

`oa_stream_complete()` is the one predicate for "this step's stream
finished":

| Wire | Terminal evidence |
| --- | --- |
| responses | `response.completed`, `response.incomplete`, `response.failed`, a bare `error` event, or a whole Response document |
| chat | `[DONE]`, a terminal `error`, or a chunk carrying `finish_reason` (gateways exist that never send the sentinel) |
| either | a whole JSON document body: the framing that delivered it is its terminal event |

A body that ends any other way is an **interrupted stream**, whatever it
carried so far, and takes the interruption path with one of three
diagnostics: `stream aborted mid-response` (transport error), `stream
closed before completion` (clean close without the terminal event), or
`stream stalled (no data for Ns)`. The existing `provider closed the stream
without a response` stays for a body that carried nothing at all. A
fragment never becomes an answer and a cut tool call never runs.

### 2. After answer text, an interruption continues the answer

ADR 0069's "terminal after text" rule is replaced. When a retryable
failure — an interruption above, or a terminal error event whose category
is not permanent — happens after answer text was shown and the retry
budget (`TNY_PROVIDER_RETRIES`, shared with plain retries) is not spent,
the next attempt is a **continuation** rather than a repeat:

- the partial stays on screen and in the step's text buffer;
- the request carries the transcript, then the partial as a trailing
  `assistant` message, then one `user` turn: *"The connection dropped while
  you were writing your previous message. The user has already seen it
  exactly as written above, ending mid-way. Continue from precisely where
  it stopped: do not repeat or rephrase anything already written, do not
  apologise, and do not mention the interruption."* — the same two message
  shapes both wires already accept, so no provider sees anything new;
- neither message is persisted: the continuation's deltas append to the
  partial and the step ends with **one** assistant message, exactly what
  the user saw;
- the status line reads `<diagnostic>: continuing the answer in 1.0s
  (attempt 2/4)`; a plain retry keeps `retrying`.

Before any text (reasoning, a half-received tool call) the attempt is
still repeated whole, as in 0069. Permanent failures and a spent budget
stay terminal: the diagnostic is emitted, the turn ends `error`, and the
partial is written to `recovery.json` for `--continue-recovery`.

### 3. A stall clock on every open stream

`TNY_PROVIDER_STALL_SECS` (default **300**, matching the Codex CLI; `0`
disables; capped at 3600) bounds silence on an open connection, measured
from the POST and reset by every received byte. The deadline is published
through the backend's existing `poll_timeout` hook, so the engine's single
loop sleeps on it — no thread, no signal, cancel still wakes it. Before
the response headers the diagnostic is `provider sent no response for Ns`;
inside the body it is `stream stalled (no data for Ns)`; both take the
interruption path (retry or continuation).

## Consequences

- A dropped or stalled ChatGPT/gateway stream costs a backoff, not the
  turn, and never a truncated answer presented as complete. The forked
  runner (ADR 0053) and wasm get the same behavior from the shared source;
  the stall clock rides `tny_poll` timeouts, never a raw `poll(2)`.
- A chat gateway that streams neither `[DONE]` nor a `finish_reason` is now
  retried and finally reported as `stream closed before completion` instead
  of accepted silently; `TNY_PROVIDER_RETRIES=0` turns that into one plain
  error with the partial recoverable. Accepted: an answer without an end is
  indistinguishable from a cut one.
- A continuation depends on the model honoring the nudge. A model that
  restarts its sentence leaves a visible seam; that seam is in the
  transcript exactly as shown, and is still far cheaper than a lost turn.
- New knob `TNY_PROVIDER_STALL_SECS`; new fixture modes
  `MOCK_CUT_ANSWER_ONCE` / `MOCK_CUT_CALL_ONCE` (`clean|abort|stall`) in
  `tests/integration/mock_openai.py`, which also verifies the continuation
  request shape. The verification contract is
  [docs/verification/stream-interruption.md](../verification/stream-interruption.md);
  every test names the requirement it covers.
- Out of scope: requesting reasoning summaries from the codex profile (a
  visibility feature, not a reliability one) and continuing across a
  runner crash (ADR 0053 already checkpoints partials).

## Reproduction and verification

Scripted provider (`POST /v1/responses`, two text deltas, then the cut),
release binary at `b80c04b` versus this change:

| Cut | Before | After |
| --- | --- | --- |
| chunked terminator, no `response.completed` | exit 0, fragment persisted as the answer, no diagnostic | `stream closed before completion: continuing the answer in 1.0s (attempt 2/4)`, answer completed once, exit 0 |
| TCP reset mid-body | `stream aborted mid-response`, exit 2, no retry | `stream aborted mid-response: continuing the answer …`, answer completed once, exit 0 |
| socket open and silent | waits indefinitely (killed by a 12 s timeout) | `stream stalled (no data for 2s): continuing the answer …` with `TNY_PROVIDER_STALL_SECS=2` |

Recorded gate results (unit, integration, leaks, quality, mutation, size) are
in the verification contract's *Recorded results* table. The focused mutation
run over the new predicate, parser, continuation pair, and interruption path
killed every valid mutant; the stripped Linux release grew by 4 KiB to
953,512 bytes.
