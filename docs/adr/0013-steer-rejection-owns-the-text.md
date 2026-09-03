# 0013 — Steer rejections carry the text; the backend resolves in-flight steers at turn end

Date: 2026-08-22
Status: accepted (amends [0011](0011-mid-turn-input-steer-or-queue.md)). Codex references are historical: [ADR 0065](0065-codex-chatgpt-responses-backend.md) turned `codex` into a builtin profile of the native loop (no host process). The codex `turn/steer` bookkeeping it describes is deleted.

## Context

Review of the ADR 0011 implementation found two defects in how a steered
message was kept safe, plus two smaller contract holes:

1. **Lost-message race.** The TUI kept the steered text in a single
   `steer_inflight` slot and freed it on `TURN_END`. Codex delivers the
   `turn/steer` *response* and the `turn/completed` *notification*
   independently on the same socket, so a rejection may legally arrive after
   the turn-complete. `TNY_EV_STEER_REJECTED` then found the slot already
   freed and the user's text vanished silently.
2. **Ambiguous bookkeeping.** One slot cannot represent two in-flight
   steers: a rejection of the first would re-queue the *second* text. The
   TUI has no way to correlate a rejection with the steer it refers to —
   only the backend, which tracks request ids, can.
3. `cx_request` registered tracked requests on a best-effort basis: with all
   pending slots busy the frame still went out untracked, so a steer's
   rejection could never be matched and the text would never come back.
4. An error response with no matching pending request fell into the generic
   handler, which emits `TNY_EV_ERROR` and **ends the turn** — so a stale
   response to an already-resolved steer could kill an unrelated later turn.

## Decision

**Ownership of a steered text moves to the backend the moment `steer()`
returns 0. The backend must hand it back — inside `TNY_EV_STEER_REJECTED`,
which now carries the text (`text`/`text_len`) — for every steer that did
not demonstrably join the turn, and must do so before that turn's
`TURN_END`. The frontend keeps no copy and re-queues whatever text the
event delivers.**

- **codex** — each pending `turn/steer` stores its own text
  (`cx_pending.steer_text`). A JSON-RPC error on the request emits
  `STEER_REJECTED` with that text. When the turn ends with steer requests
  still unanswered, `cx_end_turn` resolves each one as rejected (with its
  text, in request order) before emitting `TURN_END` — an unanswered steer
  cannot have joined a turn that is already over, and re-queueing risks at
  worst a duplicate, never a loss. A response that arrives after this sweep
  finds no pending request and is logged as a status note; **an untracked
  error response never ends the turn that is running now.**
- **openai (native)** — the single funnel `emit_turn_end` emits
  `STEER_REJECTED` with any text still parked when the turn ends
  (interrupt, error, step limit before the next model-call boundary).
  On the normal path the text was consumed by `take_steer` and no event
  fires.
- **`cx_request`** now refuses (returns -1, frame not sent) when a tracked
  request cannot get a pending slot: an unmatchable response is worse than
  failing fast. `cx_steer` and `cx_send` surface that as -1 to the caller
  (the TUI queues the text instead); `cx_cancel` falls back to an untracked
  interrupt, because not interrupting at all is the worse outcome.
- **TUI** — `steer_inflight` is gone. `STEER_REJECTED` re-queues the text
  the event carries at the front of the queue. Since only the head of a
  rejection is order-sensitive and codex resolves in request order, relative
  order of multiple rejected steers is preserved.

## Consequences

- The race in (1) is structurally impossible: the rejection and the text
  travel together, and the backend guarantees the event precedes `TURN_END`.
- Multiple concurrent steers are each tracked by id; a rejection returns
  exactly the text of the request it answers.
- A steer may be re-queued even though the host *did* absorb it (accepted,
  but the response never arrived before turn end). That duplicate is
  visible to the user in the queue row and can be dropped with Esc; the
  silent alternative — losing the message — is strictly worse.
- Tests: unit (`tests/test_codex.c`) covers the pending-slot refusal, the
  turn-end sweep ordering, the text-carrying rejection, and the
  stale-response guard; `tests/integration/test_tui.py` drives both an
  immediate (`MOCK_STEER_REJECT=now`) and a post-turn-completed
  (`MOCK_STEER_REJECT=late`) rejection end-to-end and asserts the text
  comes back as the next `turn/start` prompt.
