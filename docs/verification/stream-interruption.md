# Verification contract — stream interruption (ADR 0087)

Scope: the native OpenAI-compatible loop (`src/backends/openai/openai.c`),
both wires, every profile including `codex`. Each requirement has an id;
tests cite the id in their docstring or comment, and a change to a
requirement must update its row and the tests that cite it.

| Id | Requirement | Verified by |
| --- | --- | --- |
| SI-1 | A step's stream is complete only when its terminal event arrived (`response.completed` / `.incomplete` / `.failed` / `error`, a whole Response document; chat `[DONE]`, `error`, or a `finish_reason`). A body ending any other way is an interruption, never a finished step. | `tests/test_openai.c` `stream_complete_needs_a_terminal_event`; `tests/integration/test_openai.py` `check_stream_interruption` (`MOCK_CUT_ANSWER_ONCE=clean`, both wires) |
| SI-2 | A fragment never acts: a tool call whose stream was cut before the terminal event is retried whole and never run with truncated arguments. | `check_stream_interruption` (`MOCK_CUT_CALL_ONCE=clean\|abort\|stall`; the mock rejects any echoed call whose arguments differ) |
| SI-3 | After answer text was shown, a retryable interruption continues the answer: the output appears exactly once (partial then continuation), the step records one assistant message, the turn ends `done`. | `check_stream_interruption` (`MOCK_CUT_ANSWER_ONCE=clean\|abort\|stall`, both wires: `MOCK-OK` counted once, `steps == 2`, exit 0) |
| SI-4 | The continuation request trails the transcript with the shown partial as an `assistant` message followed by one `user` turn asking to continue without repetition; neither is persisted; the shape is identical on both wires. | `tests/test_openai.c` `continuation_trails_partial_then_user_turn`; `mock_openai.py` `_continuation` (`need()` checks on the follow-up POST) |
| SI-5 | An open stream that stays silent for `TNY_PROVIDER_STALL_SECS` (default 300, `0` disables, cap 3600) is an interruption, before the headers and inside the body; the engine loop sleeps on the deadline rather than polling. | `tests/test_openai.c` `stall_window_parses_and_clamps`; `check_stream_interruption` (`stall` modes with `TNY_PROVIDER_STALL_SECS=1`, wall time under 10 s) |
| SI-6 | Diagnostics are fixed strings, never provider text: `stream aborted mid-response`, `stream closed before completion`, `stream stalled (no data for Ns)`, `provider sent no response for Ns`, and the status line `<diagnostic>: continuing the answer in D.Ds (attempt N/M)` (or `retrying` before any text). | `check_stream_interruption` (exact stderr assertions per mode) |
| SI-7 | With no retry budget left the interruption is reported: `error` set, non-zero exit, the partial kept in the output and in `recovery.json`, never presented as a completed answer. | `check_stream_interruption` (`TNY_PROVIDER_RETRIES=0`, `output == "The workspace "`) |
| SI-8 | A whole JSON document body (a gateway ignoring `stream:true`) is complete by construction and never retried as an interruption. | `check_stream_recovery` (`MOCK_NONSTREAM_ONCE=1`, no `retrying` on stderr) |
| SI-9 | Behavior that predates the contract is preserved: a transport that dies after the terminal event still completes the step and discards the socket; failures before any text still repeat the request whole. | `tests/integration/test_libtny.py` "abrupt terminal transport close" (`MOCK_TRUNCATED_TERMINAL=1`); `check_stream_recovery` (all transient cases) |

## Running

```sh
make test-unit                                   # SI-1, SI-4, SI-5 unit rows
python3 tests/integration/test_openai.py         # SI-1 … SI-9 integration rows
python3 tests/integration/test_libtny.py         # SI-9
```

## Recorded results

Filled in by the change that introduced the contract; update when a row changes.
