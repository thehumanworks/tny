# 0216 — Buffered stream scheduling and measured code-mode choices

## Decision

Keep the 8 KiB provider dispatch budget, but schedule an immediate next dispatch
when that budget is exhausted. HTTP and TLS can have unread application bytes
when the underlying socket is no longer readable. Sleeping on that socket added
one caller poll interval before a large tool call could complete. The next probe
clears the pending-work flag before reading; EAGAIN restores the normal wait.
Cancellation and UI processing still happen between bounded batches.

Expose a private engine timeout clamp and use it in the pull API, detached
runner, in-process TUI and optimisation loops. No public ABI or protocol change.
The same scheduling applies to Responses and Chat Completions, including wasm's
existing fetch transport. Code execution on wasm remains a clean unsupported
error; no new interpreter is introduced.

Retain host-authorized CPython as the default code mode. Shell, host Python and
JavaScript work through `subprocess.run` today; no additional dispatcher is
needed. YOLO and unrestricted direct host networking remain the existing default.
Independent nested reads use the existing bounded `tools.parallel`; stateful
operations remain barriers. The prompt explicitly encourages batching independent
operations in one cell and saving reusable parameterized scripts in
`~/.tny/scripts/`, created on demand. Saved scripts are inspected before reuse,
receive workspace paths as arguments, and must not contain credentials.

## Evidence

On macOS arm64, baseline `0a40436d4091800ccae795c2d39c99f57758cad4`, ten
loopback runs per case: large tool continuation fell from **231.8 to 23.8 ms**
in-process and **136.8 to 23.9 ms** in the detached runner. Small-call latency and
turn startup did not materially improve. These are harness measurements, not
claims about model inference latency. The regression test sends over 8 KiB on a
peer that stays open, verifies immediate rescheduling, verifies EAGAIN stops it,
and completes the turn with a later terminal frame.

Five equal-workload code-mode runs through the production boundary measured
21.7 ms embedded CPython, 35.9 ms host shell, 45.1 ms host Python, 66.9 ms
JavaScript and 92.8 ms fresh Monty. A separate warm Monty checkout measured
roughly 0.48 ms, excluding the production boundary. Replacing CPython with a
persistent Monty service needs a separate lifecycle/host-capability design and
end-to-end evidence; the warm microbenchmark is not such evidence.

The live SIWC transport probe supports WebSocket continuation on this account.
Three paired, alternating transport trials with three turns each measured warm
TTFT medians of 2637 ms SSE and 1472 ms WebSocket. First-turn connection plus TTFT
was 2648 ms SSE and 3483 ms WebSocket. Retain the opt-in probe, but do not switch
the production default from this small, short-conversation sample: native
connection lifetime, cancellation, reconnection, full-context recovery and
long-session task quality are not verified by a Python transport probe.

Full measurements, reproduction commands and gate results are in
[the report](../verification/interaction-latency/report.md).
