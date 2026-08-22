# 0011 — Mid-turn input: steer where the host can, queue everywhere else

Date: 2026-08-22
Status: accepted

## Context

Pressing Enter while a turn runs used to print "a turn is already running —
esc interrupts it" into the transcript, interleaved with streaming agent
text, and then drop the message. Users expect what every modern agent shell
does: the message is either **steered** into the running turn or **queued**
and sent the moment the turn ends.

Where can a second message go while a turn is live? Checked against the
pinned protocol sources (`docs/sources.md`):

- **Codex app-server** — `turn/steer {threadId, expectedTurnId, input:
  UserInput[]}` (v2 schema, `codex app-server generate-json-schema`). It is
  plain user input appended to the active turn — not a tool result and not
  a reasoning injection. `expectedTurnId` is a required precondition; the
  request fails when it does not match the active turn, and some turns
  (`/review`, manual `/compact`) are *non-steerable* and reject it. The
  host echoes the steered text as a `userMessage` item, which tny already
  suppresses on render.
- **Cursor bridge** — a second `Send` on an agent with a live run is
  rejected with `sdk_error_code = AGENT_BUSY` (`docs/errors.md`). There is
  no steer RPC; the only options are `CancelRun` or waiting.
- **ACP v1** — one `session/prompt` pending per connection, and it stays
  pending for the whole turn. No steer.
- **Native OpenAI-compatible loop** — tny owns the loop, so a user message
  can be appended to the transcript at the next model-call boundary (after
  the current tool results are recorded). That is exactly where a chat
  completion expects new user input; nothing is smuggled into tool results
  or reasoning.

## Decision

**Enter during an active turn steers when the backend can deliver the text
into the running turn, and queues otherwise. Queued messages are sent, in
order, as soon as the turn ends on its own; Esc/Ctrl-C interrupts the turn
and drops the queue. Neither path writes a note into the transcript — the
state is shown in a row next to the composer and disappears once sent.**

- Backend contract: an optional `steer(b, text, errbuf, errlen)` in
  `tny_backend`. `0` = the text is on its way into the current turn; `-1` =
  not possible right now (no turn id yet, host busy, unsupported) and the
  caller must queue. A host that accepts the request but rejects it later
  emits `TNY_EV_STEER_REJECTED`; the TUI then moves the text to the front
  of the queue so nothing is lost.
- codex: `turn/steer` with the active `turnId`. `-1` until the `turn/start`
  response has delivered the id. A JSON-RPC error on the steer request is
  `STEER_REJECTED`, never the end of the turn.
- openai (native): the text is parked on the backend and appended as a
  `user` message before the next POST. If the model has already produced
  its final answer when steer arrives, the loop runs one more round with
  the steered message instead of ending the turn, so a steer is always
  answered within the turn it targeted.
- cursor, acp: no `steer` — Enter queues.
- TUI: a queue row above the status row reads
  `queued (n): <first message>… · sends when this turn ends · esc drops`.
  A steered message is echoed in the transcript as `› text` with a dim
  `steer` tag so the conversation reads in order. After `TURN_END` with
  `stop == DONE`, the first queued message is submitted through the normal
  `tui_submit` path (images attached at that time ride along); other stop
  reasons (interrupted, denied, step limit, error) drop the queue with a
  one-line note, because sending more work after a failure is rarely what
  the user meant. Slash commands are never queued — they run immediately as
  before.

## Consequences

- No transcript noise: the composer area carries the pending state, and it
  vanishes on send.
- Hosts keep their own semantics: codex steers mid-turn (its CLI default),
  cursor/acp batch. Users see which happened (`steer` tag vs. the queue
  row), and `/status` does not need to explain it.
- `tny ask` is unaffected: one prompt, one turn.
- Tests: the openai mock asserts a steered user message lands after the
  tool result and before the final answer (`tests/integration/test_tui.py`);
  the codex mock validates `turn/steer` against the active turn id and
  rejects a stale one; the cursor/acp paths are covered by the queue test
  (message sent after `TURN_END`, dropped on Esc).
